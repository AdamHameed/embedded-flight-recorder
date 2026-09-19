#include "flight_recorder/binary_log_writer.hpp"
#include "flight_recorder/circular_buffer.hpp"
#include "flight_recorder/crc32.hpp"
#include "flight_recorder/fault_injection.hpp"
#include "flight_recorder/flight_recorder.hpp"
#include "flight_recorder/log_format.hpp"
#include "flight_recorder/recovery_manager.hpp"

#include <chrono>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <deque>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

class TestFailure : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define EXPECT_TRUE(condition)                                                                    \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            throw TestFailure(std::string("EXPECT_TRUE failed: ") + #condition);                  \
        }                                                                                         \
    } while (false)

#define EXPECT_EQ(actual, expected)                                                               \
    do {                                                                                          \
        const auto actual_value = (actual);                                                       \
        const auto expected_value = (expected);                                                   \
        if (!(actual_value == expected_value)) {                                                  \
            std::ostringstream stream;                                                            \
            stream << "EXPECT_EQ failed: " << #actual << " != " << #expected;                    \
            throw TestFailure(stream.str());                                                      \
        }                                                                                         \
    } while (false)

struct TestCase {
    const char* name;
    std::function<void()> run;
};

class ScriptedWriterIo : public flight_recorder::WriterIo {
public:
    enum class MainWriteMode {
        Normal,
        InterruptOnce,
        ShortOnce,
        Fail
    };

    explicit ScriptedWriterIo(MainWriteMode mode = MainWriteMode::Normal,
                              bool preallocation_supported = false,
                              bool preallocation_fails = false)
        : mode_(mode),
          preallocation_supported_(preallocation_supported),
          preallocation_fails_(preallocation_fails) {}

    ssize_t pwrite_data(int fd, const void* data, std::size_t length, off_t offset) override {
        const bool main_batch = length != sizeof(flight_recorder::PersistedFileHeader) &&
                                length != sizeof(flight_recorder::PersistedJournalEntry) &&
                                length != sizeof(flight_recorder::PersistedCheckpointSlot);
        if (main_batch && !main_action_taken_) {
            main_action_taken_ = true;
            if (mode_ == MainWriteMode::InterruptOnce) {
                errno = EINTR;
                return -1;
            }
            if (mode_ == MainWriteMode::ShortOnce) {
                return ::pwrite(fd, data, length / 2u, offset);
            }
            if (mode_ == MainWriteMode::Fail) {
                errno = EIO;
                return -1;
            }
        }
        return ::pwrite(fd, data, length, offset);
    }

    bool supports_keep_size_preallocation() const override {
        return preallocation_supported_;
    }

    int preallocate_keep_size(int, off_t, off_t) override {
        ++preallocation_attempts;
        if (preallocation_fails_) {
            errno = ENOSPC;
            return -1;
        }
        return 0;
    }

    std::size_t preallocation_attempts {0};

private:
    MainWriteMode mode_;
    bool preallocation_supported_ {false};
    bool preallocation_fails_ {false};
    bool main_action_taken_ {false};
};

flight_recorder::PersistedJournalEntry make_journal_entry(std::uint64_t sequence,
                                                          std::uint64_t timestamp_us,
                                                          double altitude_m) {
    const flight_recorder::FlightRecord record {
        timestamp_us,
        altitude_m,
        140.0,
        90.0,
        250.0,
        630.0,
        2200.0,
        flight_recorder::StatusNominal
    };

    const auto payload = flight_recorder::make_persisted_payload(record);
    const flight_recorder::PersistedRecordHeader header {
        flight_recorder::kLogRecordMagic,
        flight_recorder::kLogFormatVersion,
        static_cast<std::uint16_t>(sizeof(flight_recorder::PersistedRecordHeader)),
        static_cast<std::uint32_t>(sizeof(flight_recorder::PersistedFlightPayload)),
        sequence,
        timestamp_us
    };

    flight_recorder::PersistedJournalEntry entry {
        flight_recorder::kJournalMagic,
        flight_recorder::kLogFormatVersion,
        static_cast<std::uint16_t>(sizeof(flight_recorder::PersistedJournalEntry)),
        flight_recorder::kJournalStatePending,
        static_cast<std::uint32_t>(sizeof(flight_recorder::PersistedJournalEntry)),
        sequence,
        header,
        payload,
        flight_recorder::compute_record_crc(header, payload),
        0
    };
    entry.journal_crc32 = flight_recorder::compute_journal_crc(entry);
    return entry;
}

void write_journal_file(const std::string& log_path, const flight_recorder::PersistedJournalEntry& entry) {
    const std::string journal_path = flight_recorder::RecoveryManager::journal_path_for_log(log_path);
    std::ofstream output(journal_path, std::ios::binary | std::ios::trunc);
    EXPECT_TRUE(output.good());
    output.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    EXPECT_TRUE(output.good());
}

void remove_if_present(const std::string& path) {
    std::remove(path.c_str());
}

flight_recorder::FlightRecord make_record(std::uint64_t timestamp_us, double altitude_m = 1234.5) {
    return flight_recorder::FlightRecord {
        timestamp_us,
        altitude_m,
        145.0,
        87.0,
        300.0,
        640.0,
        2200.0,
        static_cast<std::uint32_t>(flight_recorder::StatusNominal | flight_recorder::StatusEngineWarning)
    };
}

void create_valid_log(const std::string& path, std::size_t record_count = 2) {
    remove_if_present(path);
    remove_if_present(flight_recorder::RecoveryManager::journal_path_for_log(path));
    flight_recorder::BinaryLogWriter writer(path);
    EXPECT_TRUE(writer.open());
    for (std::size_t index = 0; index < record_count; ++index) {
        EXPECT_TRUE(writer.append(make_record(100 + index, 1000.0 + index), index + 1));
    }
    writer.close();
}

void flip_file_byte(const std::string& path, std::size_t offset) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    EXPECT_TRUE(file.good());
    file.seekg(static_cast<std::streamoff>(offset));
    char value = 0;
    file.read(&value, 1);
    EXPECT_TRUE(file.good());
    value ^= static_cast<char>(0x5A);
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(&value, 1);
    EXPECT_TRUE(file.good());
}

void rewrite_record_sequence(const std::string& path, std::size_t record_index, std::uint64_t sequence) {
    const auto offset = sizeof(flight_recorder::PersistedFileHeader) +
                        (record_index * flight_recorder::kPersistedRecordSize);
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    EXPECT_TRUE(file.good());
    flight_recorder::PersistedRecordHeader header {};
    flight_recorder::PersistedFlightPayload payload {};
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    file.read(reinterpret_cast<char*>(&payload), sizeof(payload));
    EXPECT_TRUE(file.good());
    header.sequence = sequence;
    const auto crc = flight_recorder::compute_record_crc(header, payload);
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(&payload), sizeof(payload));
    file.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
    EXPECT_TRUE(file.good());
}

void cleanup_log(const std::string& path) {
    remove_if_present(path);
    remove_if_present(flight_recorder::RecoveryManager::journal_path_for_log(path));
}

void test_circular_buffer_push_pop_correctness() {
    flight_recorder::CircularBuffer buffer(2);

    EXPECT_EQ(buffer.push_wait_for(flight_recorder::FlightRecord {.timestamp_us = 1},
                                   std::chrono::milliseconds(1)),
              flight_recorder::CircularBuffer::PushStatus::Pushed);
    EXPECT_EQ(buffer.push_wait_for(flight_recorder::FlightRecord {.timestamp_us = 2},
                                   std::chrono::milliseconds(1)),
              flight_recorder::CircularBuffer::PushStatus::Pushed);
    EXPECT_EQ(buffer.push_wait_for(flight_recorder::FlightRecord {.timestamp_us = 3},
                                   std::chrono::milliseconds(1)),
              flight_recorder::CircularBuffer::PushStatus::Timeout);
    EXPECT_EQ(buffer.stats().dropped_records, 1u);

    flight_recorder::FlightRecord record {};
    EXPECT_TRUE(buffer.try_pop(record));
    EXPECT_EQ(record.timestamp_us, 1u);
    EXPECT_TRUE(buffer.try_pop(record));
    EXPECT_EQ(record.timestamp_us, 2u);
}

void test_spsc_wraparound_full_empty_and_batch_drain() {
    flight_recorder::CircularBuffer buffer(4);
    for (std::uint64_t value = 1; value <= 4; ++value) {
        EXPECT_EQ(buffer.try_push(flight_recorder::FlightRecord {.timestamp_us = value}),
                  flight_recorder::CircularBuffer::PushStatus::Pushed);
    }
    EXPECT_EQ(buffer.push_wait_for(flight_recorder::FlightRecord {.timestamp_us = 99},
                                   std::chrono::microseconds(0)),
              flight_recorder::CircularBuffer::PushStatus::Timeout);
    EXPECT_EQ(buffer.stats().dropped_records, 1u);
    flight_recorder::FlightRecord output[4] {};
    EXPECT_EQ(buffer.try_pop_batch(output, 2), 2u);
    EXPECT_EQ(output[0].timestamp_us, 1u);
    EXPECT_EQ(output[1].timestamp_us, 2u);
    EXPECT_EQ(buffer.try_push(flight_recorder::FlightRecord {.timestamp_us = 5}),
              flight_recorder::CircularBuffer::PushStatus::Pushed);
    EXPECT_EQ(buffer.try_push(flight_recorder::FlightRecord {.timestamp_us = 6}),
              flight_recorder::CircularBuffer::PushStatus::Pushed);
    EXPECT_EQ(buffer.try_pop_batch(output, 4), 4u);
    for (std::size_t index = 0; index < 4; ++index) {
        EXPECT_EQ(output[index].timestamp_us, index + 3);
    }
    buffer.close();
    flight_recorder::FlightRecord record;
    EXPECT_EQ(buffer.pop_blocking(record), flight_recorder::CircularBuffer::PopStatus::Closed);
    EXPECT_EQ(buffer.try_push(record), flight_recorder::CircularBuffer::PushStatus::Closed);
}

void test_spsc_close_wakes_waiter() {
    flight_recorder::CircularBuffer buffer(2);
    std::atomic<flight_recorder::CircularBuffer::PopStatus> result {
        flight_recorder::CircularBuffer::PopStatus::Timeout};
    std::thread consumer([&] {
        flight_recorder::FlightRecord record;
        result.store(buffer.pop_blocking(record));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    buffer.close();
    consumer.join();
    EXPECT_EQ(result.load(), flight_recorder::CircularBuffer::PopStatus::Closed);
}

void test_spsc_full_to_nonfull_wakes_producer() {
    flight_recorder::CircularBuffer buffer(1);
    EXPECT_EQ(buffer.try_push(flight_recorder::FlightRecord {.timestamp_us = 1}),
              flight_recorder::CircularBuffer::PushStatus::Pushed);

    std::atomic<flight_recorder::CircularBuffer::PushStatus> result {
        flight_recorder::CircularBuffer::PushStatus::Timeout};
    std::thread producer([&] {
        result.store(buffer.push_blocking(flight_recorder::FlightRecord {.timestamp_us = 2}));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    flight_recorder::FlightRecord record;
    EXPECT_TRUE(buffer.try_pop(record));
    EXPECT_EQ(record.timestamp_us, 1u);
    producer.join();
    EXPECT_EQ(result.load(), flight_recorder::CircularBuffer::PushStatus::Pushed);
    EXPECT_TRUE(buffer.try_pop(record));
    EXPECT_EQ(record.timestamp_us, 2u);
}

void test_spsc_randomized_model_and_concurrent_ordering() {
    flight_recorder::CircularBuffer model_buffer(17);
    std::deque<std::uint64_t> expected;
    std::mt19937 generator(42);
    for (std::uint64_t step = 1; step <= 20'000; ++step) {
        if ((generator() & 1u) != 0u) {
            const auto status = model_buffer.try_push(flight_recorder::FlightRecord {.timestamp_us = step});
            if (expected.size() == 17) {
                EXPECT_EQ(status, flight_recorder::CircularBuffer::PushStatus::Timeout);
            } else {
                EXPECT_EQ(status, flight_recorder::CircularBuffer::PushStatus::Pushed);
                expected.push_back(step);
            }
        } else {
            flight_recorder::FlightRecord record;
            const bool popped = model_buffer.try_pop(record);
            EXPECT_EQ(popped, !expected.empty());
            if (popped) {
                EXPECT_EQ(record.timestamp_us, expected.front());
                expected.pop_front();
            }
        }
    }

    constexpr std::size_t total = 250'000;
    flight_recorder::CircularBuffer concurrent_buffer(1024);
    std::atomic<bool> ordered {true};
    std::thread producer([&] {
        for (std::size_t index = 1; index <= total; ++index) {
            if (concurrent_buffer.push_blocking(
                    flight_recorder::FlightRecord {.timestamp_us = index}) !=
                flight_recorder::CircularBuffer::PushStatus::Pushed) {
                ordered.store(false);
                break;
            }
        }
        concurrent_buffer.close();
    });
    std::size_t consumed = 0;
    flight_recorder::FlightRecord batch[64] {};
    while (true) {
        flight_recorder::FlightRecord first;
        const auto status = concurrent_buffer.pop_blocking(first);
        if (status == flight_recorder::CircularBuffer::PopStatus::Closed) break;
        ordered.store(ordered.load() && first.timestamp_us == consumed + 1);
        ++consumed;
        const auto count = concurrent_buffer.try_pop_batch(batch, 64);
        for (std::size_t index = 0; index < count; ++index) {
            ordered.store(ordered.load() && batch[index].timestamp_us == consumed + 1);
            ++consumed;
        }
    }
    producer.join();
    EXPECT_TRUE(ordered.load());
    EXPECT_EQ(consumed, total);
    EXPECT_EQ(concurrent_buffer.stats().dropped_records, 0u);
}

void test_recorder_rejects_zero_capacity() {
    flight_recorder::RecorderConfig config;
    config.output_path = "test_zero_capacity.bin";
    config.buffer_size = 0;
    flight_recorder::FlightRecorder recorder(config);
    EXPECT_TRUE(!recorder.start());
    recorder.stop();
    remove_if_present(config.output_path);
    remove_if_present(flight_recorder::RecoveryManager::journal_path_for_log(config.output_path));
}

void test_recorder_rejects_invalid_configuration() {
    flight_recorder::RecorderConfig config;
    config.output_path = "test_zero_rate.bin";
    config.sample_rate_hz = 0;
    flight_recorder::FlightRecorder zero_rate_recorder(config);
    EXPECT_TRUE(!zero_rate_recorder.start());
    zero_rate_recorder.stop();

    config.sample_rate_hz = 20;
    config.output_path.clear();
    flight_recorder::FlightRecorder empty_path_recorder(config);
    EXPECT_TRUE(!empty_path_recorder.start());
    empty_path_recorder.stop();

    remove_if_present("test_zero_rate.bin");
    remove_if_present(flight_recorder::RecoveryManager::journal_path_for_log("test_zero_rate.bin"));
}

void test_recorder_clean_shutdown() {
    const std::string path = "test_clean_shutdown.bin";
    remove_if_present(path);
    remove_if_present(flight_recorder::RecoveryManager::journal_path_for_log(path));

    flight_recorder::RecorderConfig config;
    config.output_path = path;
    config.buffer_size = 8;
    config.sample_rate_hz = 1000;
    flight_recorder::FlightRecorder recorder(config);
    EXPECT_TRUE(recorder.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    recorder.stop();
    recorder.stop();

    const auto stats = recorder.stats();
    EXPECT_TRUE(stats.total_records_generated > 0);
    EXPECT_EQ(stats.total_records_written, stats.total_records_generated - stats.dropped_records);
    EXPECT_EQ(stats.total_records_committed, stats.total_records_written);
    EXPECT_TRUE(!stats.writer_error);

    flight_recorder::RecoveryManager recovery_manager;
    const auto report = recovery_manager.validate(path);
    EXPECT_TRUE(report.healthy);
    EXPECT_EQ(report.valid_records, static_cast<std::size_t>(stats.total_records_written));

    remove_if_present(path);
    remove_if_present(flight_recorder::RecoveryManager::journal_path_for_log(path));
}

void test_timed_group_commit() {
    // At one sample per second the writer is idle between records. A very
    // large batch threshold ensures only the timer can commit while running.
    const std::string path = "test_timed_commit.bin";
    cleanup_log(path);
    flight_recorder::RecorderConfig config;
    config.output_path = path;
    config.sample_rate_hz = 1;
    config.sync_every_batches = 1'000'000;
    config.flush_interval_ms = 30;
    flight_recorder::FlightRecorder recorder(config);
    EXPECT_TRUE(recorder.start());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    while (recorder.stats().total_records_committed == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto live = recorder.stats();
    // Inspect the checkpoint before stop() can flush it. This verifies that the
    // live counter corresponds to an actual CRC-valid checkpoint on disk.
    std::array<flight_recorder::PersistedCheckpointSlot,
               flight_recorder::kCheckpointSlotCount> slots {};
    std::ifstream journal(path + ".journal", std::ios::binary);
    journal.read(reinterpret_cast<char*>(slots.data()), sizeof(slots));
    bool checkpoint_found = false;
    for (const auto& slot : slots) {
        checkpoint_found = checkpoint_found ||
            (flight_recorder::checkpoint_metadata_valid(slot) &&
             slot.record_count > 0 && slot.record_count >= live.total_records_committed);
    }
    recorder.stop();
    EXPECT_TRUE(live.total_records_committed > 0);
    EXPECT_TRUE(checkpoint_found);
    EXPECT_TRUE(live.total_records_written < config.sync_every_batches);
    EXPECT_TRUE(!live.writer_error);
    const auto recovered = flight_recorder::RecoveryManager {}.recover_startup_state(path);
    EXPECT_TRUE(recovered.healthy);
    EXPECT_EQ(recovered.valid_records, recorder.stats().total_records_committed);
    cleanup_log(path);
}

void test_group_commit_timer_disabled() {
    const std::string path = "test_commit_timer_disabled.bin";
    cleanup_log(path);
    flight_recorder::RecorderConfig config;
    config.output_path = path;
    config.sample_rate_hz = 1000;
    config.sync_every_batches = 1'000'000;
    EXPECT_EQ(config.flush_interval_ms, 0u);
    flight_recorder::FlightRecorder recorder(config);
    EXPECT_TRUE(recorder.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto live = recorder.stats();
    recorder.stop();
    EXPECT_TRUE(live.total_records_written > 0);
    EXPECT_EQ(live.total_records_committed, 0u);
    EXPECT_EQ(recorder.stats().total_records_committed, recorder.stats().total_records_written);
    EXPECT_TRUE(!recorder.stats().writer_error);
    cleanup_log(path);
}

void test_group_commit_timer_under_load() {
    const std::string path = "test_commit_timer_load.bin";
    cleanup_log(path);
    flight_recorder::RecorderConfig config;
    config.output_path = path;
    config.sample_rate_hz = 1000;
    config.sync_every_batches = 1'000'000;
    config.flush_interval_ms = 30;
    flight_recorder::FlightRecorder recorder(config);
    EXPECT_TRUE(recorder.start());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::uint64_t first_commit = 0;
    std::uint64_t later_commit = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        later_commit = recorder.stats().total_records_committed;
        if (first_commit == 0) first_commit = later_commit;
        if (later_commit > first_commit && first_commit > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    recorder.stop();
    EXPECT_TRUE(first_commit > 0);
    EXPECT_TRUE(later_commit > first_commit);
    EXPECT_TRUE(recorder.stats().total_records_written < config.sync_every_batches);
    EXPECT_EQ(recorder.stats().total_records_written, recorder.stats().total_records_committed);
    EXPECT_TRUE(!recorder.stats().writer_error);
    cleanup_log(path);
}

void test_record_serialization_deserialization() {
    const auto original = make_record(42, 3200.25);
    const auto payload = flight_recorder::make_persisted_payload(original);
    const flight_recorder::PersistedRecordHeader header {
        flight_recorder::kLogRecordMagic,
        flight_recorder::kLogFormatVersion,
        static_cast<std::uint16_t>(sizeof(flight_recorder::PersistedRecordHeader)),
        static_cast<std::uint32_t>(sizeof(flight_recorder::PersistedFlightPayload)),
        7,
        original.timestamp_us
    };

    const auto restored = flight_recorder::make_runtime_record(header, payload);
    EXPECT_EQ(restored.timestamp_us, original.timestamp_us);
    EXPECT_EQ(restored.system_status, original.system_status);
    EXPECT_EQ(restored.altitude_m, original.altitude_m);
    EXPECT_EQ(restored.airspeed_kts, original.airspeed_kts);
    EXPECT_EQ(restored.heading_deg, original.heading_deg);
    EXPECT_EQ(restored.vertical_speed_fpm, original.vertical_speed_fpm);
    EXPECT_EQ(restored.engine_temperature_c, original.engine_temperature_c);
    EXPECT_EQ(restored.engine_rpm, original.engine_rpm);
}

void test_checksum_validation() {
    constexpr char canonical[] = "123456789";
    EXPECT_EQ(flight_recorder::compute_crc32(canonical, sizeof(canonical) - 1), 0xCBF43926u);
    const auto record = make_record(100);
    const auto payload = flight_recorder::make_persisted_payload(record);
    flight_recorder::PersistedRecordHeader header {
        flight_recorder::kLogRecordMagic,
        flight_recorder::kLogFormatVersion,
        static_cast<std::uint16_t>(sizeof(flight_recorder::PersistedRecordHeader)),
        static_cast<std::uint32_t>(sizeof(flight_recorder::PersistedFlightPayload)),
        1,
        record.timestamp_us
    };

    const auto crc_before = flight_recorder::compute_record_crc(header, payload);
    header.timestamp_us += 1;
    const auto crc_after = flight_recorder::compute_record_crc(header, payload);
    EXPECT_TRUE(crc_before != crc_after);

    const auto raw_crc = flight_recorder::compute_crc32(&payload, sizeof(payload));
    EXPECT_TRUE(raw_crc != 0u);
}

void test_replay_parses_valid_file() {
    const std::string path = "test_valid_replay.bin";
    remove_if_present(path);
    remove_if_present(flight_recorder::RecoveryManager::journal_path_for_log(path));

    flight_recorder::BinaryLogWriter writer(path);
    EXPECT_TRUE(writer.open());
    EXPECT_TRUE(writer.append(make_record(100, 2000.0), 1));
    EXPECT_TRUE(writer.append(make_record(200, 2100.0), 2));
    EXPECT_TRUE(writer.flush());
    writer.close();

    flight_recorder::RecoveryManager recovery_manager;
    flight_recorder::ReplayLog replay_log;
    std::string error;
    EXPECT_TRUE(recovery_manager.read_log(path, replay_log, error));
    EXPECT_EQ(replay_log.metadata.format_version, flight_recorder::kLogFormatVersion);
    EXPECT_EQ(replay_log.metadata.record_size, static_cast<std::uint32_t>(flight_recorder::kPersistedRecordSize));
    EXPECT_EQ(replay_log.entries.size(), 2u);
    EXPECT_EQ(replay_log.entries.front().sequence, 1u);
    EXPECT_EQ(replay_log.entries.back().sequence, 2u);

    remove_if_present(path);
    remove_if_present(flight_recorder::RecoveryManager::journal_path_for_log(path));
}

void test_detection_of_truncated_records() {
    const std::string path = "test_runtime_truncated.bin";
    remove_if_present(path);
    remove_if_present(flight_recorder::RecoveryManager::journal_path_for_log(path));

    flight_recorder::BinaryLogWriter writer(path);
    EXPECT_TRUE(writer.open());
    EXPECT_TRUE(writer.append(make_record(11), 1));
    EXPECT_TRUE(writer.flush());
    writer.close();

    {
        std::ofstream output(path, std::ios::binary | std::ios::app);
        const char partial[] = {0x01, 0x02, 0x03};
        output.write(partial, sizeof(partial));
    }

    flight_recorder::RecoveryManager recovery_manager;
    const auto report = recovery_manager.validate(path);
    EXPECT_TRUE(!report.healthy);
    EXPECT_TRUE(report.corruption_detected);
    EXPECT_EQ(report.valid_records, 1u);
    EXPECT_EQ(report.first_bad_record_index, 2u);

    const auto recovered = recovery_manager.recover(path, true);
    EXPECT_TRUE(recovered.truncated);

    remove_if_present(path);
    remove_if_present(flight_recorder::RecoveryManager::journal_path_for_log(path));
}

void test_detection_of_corrupt_records() {
    const std::string path = "test_corrupt.bin";
    remove_if_present(path);
    remove_if_present(flight_recorder::RecoveryManager::journal_path_for_log(path));

    flight_recorder::BinaryLogWriter writer(path);
    EXPECT_TRUE(writer.open());
    EXPECT_TRUE(writer.append(make_record(88, 123.0), 1));
    EXPECT_TRUE(writer.flush());
    writer.close();

    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        EXPECT_TRUE(file.good());
        file.seekp(static_cast<std::streamoff>(sizeof(flight_recorder::PersistedFileHeader) +
                                              sizeof(flight_recorder::PersistedRecordHeader) + 4));
        const char byte = static_cast<char>(0xFF);
        file.write(&byte, 1);
    }

    flight_recorder::RecoveryManager recovery_manager;
    const auto report = recovery_manager.validate(path);
    EXPECT_TRUE(!report.healthy);
    EXPECT_TRUE(report.corruption_detected);
    EXPECT_EQ(report.valid_records, 0u);
    EXPECT_EQ(report.first_bad_record_index, 1u);
    EXPECT_TRUE(report.message.find("CRC mismatch") != std::string::npos);

    remove_if_present(path);
    remove_if_present(flight_recorder::RecoveryManager::journal_path_for_log(path));
}

void test_file_and_record_header_corruption_detection() {
    const std::string path = "test_header_corruption.bin";
    flight_recorder::RecoveryManager recovery_manager;

    create_valid_log(path, 1);
    flip_file_byte(path, offsetof(flight_recorder::PersistedFileHeader, record_size));
    auto report = recovery_manager.validate(path);
    EXPECT_TRUE(!report.healthy);
    EXPECT_TRUE(report.corruption_detected);
    EXPECT_EQ(report.corruption_offset, 0u);
    EXPECT_TRUE(report.message.find("file header metadata") != std::string::npos);

    create_valid_log(path, 1);
    flip_file_byte(path, offsetof(flight_recorder::PersistedFileHeader, header_crc32));
    report = recovery_manager.validate(path);
    EXPECT_TRUE(report.message.find("file header CRC") != std::string::npos);

    create_valid_log(path, 1);
    flip_file_byte(path, sizeof(flight_recorder::PersistedFileHeader) +
                             offsetof(flight_recorder::PersistedRecordHeader, magic));
    report = recovery_manager.validate(path);
    EXPECT_TRUE(report.corruption_detected);
    EXPECT_TRUE(report.message.find("record metadata") != std::string::npos);
    cleanup_log(path);
}

void test_timestamp_payload_and_crc_corruption_detection() {
    const std::string path = "test_region_corruption.bin";
    flight_recorder::RecoveryManager recovery_manager;
    const auto record_base = sizeof(flight_recorder::PersistedFileHeader);
    const auto payload_base = record_base + sizeof(flight_recorder::PersistedRecordHeader);

    create_valid_log(path, 1);
    flip_file_byte(path, record_base + offsetof(flight_recorder::PersistedRecordHeader, sequence));
    auto report = recovery_manager.validate(path);
    EXPECT_EQ(report.checksum_failures, 1u);

    create_valid_log(path, 1);
    flip_file_byte(path, record_base + offsetof(flight_recorder::PersistedRecordHeader, timestamp_us));
    report = recovery_manager.validate(path);
    EXPECT_EQ(report.checksum_failures, 1u);

    const std::array<std::size_t, 7> payload_offsets {
        offsetof(flight_recorder::PersistedFlightPayload, altitude_m),
        offsetof(flight_recorder::PersistedFlightPayload, airspeed_kts),
        offsetof(flight_recorder::PersistedFlightPayload, heading_deg),
        offsetof(flight_recorder::PersistedFlightPayload, vertical_speed_fpm),
        offsetof(flight_recorder::PersistedFlightPayload, engine_temperature_c),
        offsetof(flight_recorder::PersistedFlightPayload, engine_rpm),
        offsetof(flight_recorder::PersistedFlightPayload, system_status)
    };
    for (const auto payload_offset : payload_offsets) {
        create_valid_log(path, 1);
        flip_file_byte(path, payload_base + payload_offset);
        report = recovery_manager.validate(path);
        EXPECT_TRUE(report.corruption_detected);
        EXPECT_EQ(report.checksum_failures, 1u);
        EXPECT_TRUE(report.message.find("CRC mismatch") != std::string::npos);
    }

    create_valid_log(path, 1);
    flip_file_byte(path, record_base + sizeof(flight_recorder::PersistedRecordHeader) +
                             sizeof(flight_recorder::PersistedFlightPayload));
    report = recovery_manager.validate(path);
    EXPECT_EQ(report.checksum_failures, 1u);
    cleanup_log(path);
}

void test_truncated_header_payload_and_crc_detection() {
    const std::string path = "test_precise_truncation.bin";
    flight_recorder::RecoveryManager recovery_manager;

    create_valid_log(path, 1);
    EXPECT_EQ(::truncate(path.c_str(), 10), 0);
    auto report = recovery_manager.validate(path);
    EXPECT_TRUE(report.message.find("file header") != std::string::npos);
    EXPECT_EQ(report.bytes_scanned, 10u);

    create_valid_log(path, 1);
    const auto partial_record_header = sizeof(flight_recorder::PersistedFileHeader) + 10u;
    EXPECT_EQ(::truncate(path.c_str(), static_cast<off_t>(partial_record_header)), 0);
    report = recovery_manager.validate(path);
    EXPECT_TRUE(report.message.find("partial record header") != std::string::npos);
    EXPECT_EQ(report.valid_bytes, sizeof(flight_recorder::PersistedFileHeader));
    EXPECT_EQ(report.bytes_scanned, partial_record_header);

    create_valid_log(path, 1);
    const auto partial_payload = sizeof(flight_recorder::PersistedFileHeader) +
                                 sizeof(flight_recorder::PersistedRecordHeader) + 10u;
    EXPECT_EQ(::truncate(path.c_str(), static_cast<off_t>(partial_payload)), 0);
    report = recovery_manager.validate(path);
    EXPECT_TRUE(report.message.find("record payload") != std::string::npos);
    EXPECT_EQ(report.bytes_scanned, partial_payload);

    create_valid_log(path, 1);
    const auto partial_crc = sizeof(flight_recorder::PersistedFileHeader) +
                             sizeof(flight_recorder::PersistedRecordHeader) +
                             sizeof(flight_recorder::PersistedFlightPayload) + 2u;
    EXPECT_EQ(::truncate(path.c_str(), static_cast<off_t>(partial_crc)), 0);
    report = recovery_manager.validate(path);
    EXPECT_TRUE(report.message.find("record CRC") != std::string::npos);
    EXPECT_EQ(report.bytes_scanned, partial_crc);
    cleanup_log(path);
}

void test_duplicate_rewind_and_gap_sequences() {
    const std::string path = "test_sequence_continuity.bin";
    flight_recorder::RecoveryManager recovery_manager;
    for (const std::uint64_t observed : {1u, 0u, 3u}) {
        create_valid_log(path, 2);
        rewrite_record_sequence(path, 1, observed);
        flight_recorder::ReplayLog replay_log;
        flight_recorder::RecoveryReport report;
        EXPECT_TRUE(recovery_manager.scan_replayable_log(path, replay_log, report));
        EXPECT_TRUE(!report.healthy);
        EXPECT_TRUE(report.corruption_detected);
        EXPECT_EQ(report.valid_records, 1u);
        EXPECT_EQ(replay_log.entries.size(), 1u);
        EXPECT_EQ(report.valid_bytes,
                  sizeof(flight_recorder::PersistedFileHeader) + flight_recorder::kPersistedRecordSize);
        EXPECT_EQ(report.first_bad_record_index, 2u);
        EXPECT_EQ(report.first_bad_record_sequence, observed);
        EXPECT_EQ(report.expected_sequence, 2u);
        EXPECT_TRUE(report.message.find("expected 2, observed") != std::string::npos);
    }
    cleanup_log(path);
}

void test_unknown_format_version_rejected() {
    const std::string path = "test_unknown_version.bin";
    create_valid_log(path, 1);
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    flight_recorder::PersistedFileHeader header {};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    EXPECT_TRUE(file.good());
    header.version = static_cast<std::uint16_t>(flight_recorder::kLogFormatVersion + 1);
    header.header_crc32 = flight_recorder::compute_file_header_crc(header);
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.close();

    flight_recorder::RecoveryManager recovery_manager;
    const auto report = recovery_manager.validate(path);
    EXPECT_TRUE(!report.healthy);
    EXPECT_TRUE(report.message.find("file header metadata") != std::string::npos);
    cleanup_log(path);
}

std::vector<flight_recorder::SequencedRecord> make_batch(std::size_t count) {
    std::vector<flight_recorder::SequencedRecord> records;
    for (std::size_t index = 0; index < count; ++index) {
        records.push_back({make_record(100 + index, 2000.0 + index), index + 1});
    }
    return records;
}

std::vector<flight_recorder::SequencedRecord> make_batch_from(std::uint64_t first_sequence,
                                                               std::size_t count) {
    std::vector<flight_recorder::SequencedRecord> records;
    for (std::size_t index = 0; index < count; ++index) {
        records.push_back({make_record(100 + first_sequence + index, 3000.0 + index),
                           first_sequence + index});
    }
    return records;
}

std::array<flight_recorder::PersistedCheckpointSlot, flight_recorder::kCheckpointSlotCount>
read_checkpoint_slots(const std::string& path) {
    std::array<flight_recorder::PersistedCheckpointSlot,
               flight_recorder::kCheckpointSlotCount> slots {};
    std::ifstream input(
        flight_recorder::RecoveryManager::journal_path_for_log(path), std::ios::binary);
    EXPECT_TRUE(input.good());
    input.read(reinterpret_cast<char*>(slots.data()),
               static_cast<std::streamsize>(sizeof(slots)));
    EXPECT_TRUE(input.good());
    return slots;
}

void test_aligned_full_batch_uses_one_write() {
    const std::string path = "test_full_batch.bin";
    cleanup_log(path);
    auto io = std::make_shared<ScriptedWriterIo>(
        ScriptedWriterIo::MainWriteMode::Normal, true, false);
    flight_recorder::BinaryLogWriter writer(
        path, {}, flight_recorder::WriterOptions {4, 1, 4096}, io);
    EXPECT_TRUE(writer.open());
    EXPECT_EQ(writer.serialization_buffer_address() % 4096u, 0u);
    EXPECT_TRUE(writer.append_batch(make_batch(4)));
    EXPECT_EQ(writer.stats().records_written, 4u);
    EXPECT_EQ(writer.stats().batches_written, 1u);
    EXPECT_EQ(writer.stats().bytes_written, 4u * flight_recorder::kPersistedRecordSize);
    EXPECT_EQ(writer.stats().write_calls, 1u);
    EXPECT_EQ(writer.stats().preallocation_calls, 1u);
    EXPECT_EQ(io->preallocation_attempts, 1u);
    writer.close();

    flight_recorder::RecoveryManager recovery_manager;
    const auto report = recovery_manager.validate(path);
    EXPECT_TRUE(report.healthy);
    EXPECT_EQ(report.valid_records, 4u);
    cleanup_log(path);
}

void test_short_write_and_eintr_retries() {
    const std::string path = "test_io_retries.bin";
    for (const auto mode : {ScriptedWriterIo::MainWriteMode::InterruptOnce,
                            ScriptedWriterIo::MainWriteMode::ShortOnce}) {
        cleanup_log(path);
        auto io = std::make_shared<ScriptedWriterIo>(mode);
        flight_recorder::BinaryLogWriter writer(
            path, {}, flight_recorder::WriterOptions {3, 1, 4096}, io);
        EXPECT_TRUE(writer.open());
        EXPECT_TRUE(writer.append_batch(make_batch(3)));
        EXPECT_EQ(writer.stats().records_written, 3u);
        EXPECT_EQ(writer.stats().write_calls, 2u);
        writer.close();

        flight_recorder::RecoveryManager recovery_manager;
        const auto report = recovery_manager.validate(path);
        EXPECT_TRUE(report.healthy);
        EXPECT_EQ(report.valid_records, 3u);
    }
    cleanup_log(path);
}

void test_partial_batch_group_flush_and_io_failures() {
    const std::string path = "test_partial_batch.bin";
    cleanup_log(path);
    {
        flight_recorder::BinaryLogWriter writer(
            path, {}, flight_recorder::WriterOptions {8, 10, 4096});
        EXPECT_TRUE(writer.open());
        EXPECT_TRUE(writer.append_batch(make_batch(3)));
        EXPECT_EQ(writer.stats().sync_calls, 1u);
        EXPECT_EQ(writer.stats().checkpoint_writes, 1u);
        EXPECT_EQ(writer.stats().records_committed, 0u);
        writer.close();
        EXPECT_EQ(writer.stats().sync_calls, 2u);
        EXPECT_EQ(writer.stats().checkpoint_writes, 2u);
        EXPECT_EQ(writer.stats().records_committed, 3u);
    }
    flight_recorder::RecoveryManager recovery_manager;
    EXPECT_EQ(recovery_manager.validate(path).valid_records, 3u);
    cleanup_log(path);

    auto preallocation_failure = std::make_shared<ScriptedWriterIo>(
        ScriptedWriterIo::MainWriteMode::Normal, true, true);
    flight_recorder::BinaryLogWriter no_space_writer(
        path, {}, flight_recorder::WriterOptions {2, 1, 4096}, preallocation_failure);
    EXPECT_TRUE(no_space_writer.open());
    EXPECT_TRUE(!no_space_writer.append_batch(make_batch(2)));
    EXPECT_TRUE(no_space_writer.stats().failures > 0);
    no_space_writer.close();
    cleanup_log(path);

    auto disk_error = std::make_shared<ScriptedWriterIo>(ScriptedWriterIo::MainWriteMode::Fail);
    flight_recorder::BinaryLogWriter failing_writer(
        path, {}, flight_recorder::WriterOptions {2, 1, 4096}, disk_error);
    EXPECT_TRUE(failing_writer.open());
    EXPECT_TRUE(!failing_writer.append_batch(make_batch(2)));
    EXPECT_EQ(failing_writer.stats().records_written, 0u);
    EXPECT_TRUE(failing_writer.stats().failures > 0);
    failing_writer.close();
    cleanup_log(path);
}

void test_checkpoint_generations_commit_counters_and_idempotence() {
    const std::string path = "test_checkpoint_generations.bin";
    cleanup_log(path);
    flight_recorder::BinaryLogWriter writer(
        path, {}, flight_recorder::WriterOptions {2, 1, 4096});
    EXPECT_TRUE(writer.open());
    EXPECT_TRUE(writer.append_batch(make_batch_from(1, 2)));
    EXPECT_TRUE(writer.append_batch(make_batch_from(3, 2)));
    EXPECT_EQ(writer.stats().records_written, 4u);
    EXPECT_EQ(writer.stats().records_committed, 4u);
    EXPECT_EQ(writer.stats().checkpoint_writes, 3u);
    EXPECT_EQ(writer.stats().journal_sync_calls, 3u);
    EXPECT_EQ(writer.stats().commits, 3u);
    writer.close();

    const auto slots = read_checkpoint_slots(path);
    EXPECT_TRUE(flight_recorder::checkpoint_metadata_valid(slots[0]));
    EXPECT_TRUE(flight_recorder::checkpoint_metadata_valid(slots[1]));
    EXPECT_TRUE(slots[0].generation != slots[1].generation);

    flight_recorder::RecoveryManager recovery_manager;
    const auto first = recovery_manager.recover_startup_state(path);
    EXPECT_TRUE(first.healthy);
    EXPECT_EQ(first.valid_records, 4u);
    EXPECT_EQ(first.checkpoint_generation, 3u);
    EXPECT_TRUE(!first.log_truncated);
    const auto second = recovery_manager.recover_startup_state(path);
    EXPECT_TRUE(second.healthy);
    EXPECT_EQ(second.valid_records, first.valid_records);
    EXPECT_EQ(second.checkpoint_generation, first.checkpoint_generation);
    EXPECT_TRUE(!second.log_truncated);
    cleanup_log(path);
}

void test_checkpoint_falls_back_from_torn_newest_slot() {
    const std::string path = "test_checkpoint_fallback.bin";
    cleanup_log(path);
    {
        flight_recorder::BinaryLogWriter writer(
            path, {}, flight_recorder::WriterOptions {1, 1, 4096});
        EXPECT_TRUE(writer.open());
        EXPECT_TRUE(writer.append_batch(make_batch_from(1, 1)));
        EXPECT_TRUE(writer.append_batch(make_batch_from(2, 1)));
    }
    auto slots = read_checkpoint_slots(path);
    const std::size_t newest = slots[0].generation > slots[1].generation ? 0u : 1u;
    const auto journal_path = flight_recorder::RecoveryManager::journal_path_for_log(path);
    flip_file_byte(journal_path,
                   newest * sizeof(flight_recorder::PersistedCheckpointSlot) +
                       offsetof(flight_recorder::PersistedCheckpointSlot, checkpoint_crc32));

    flight_recorder::RecoveryManager recovery_manager;
    const auto recovered = recovery_manager.recover_startup_state(path);
    EXPECT_TRUE(recovered.healthy);
    EXPECT_TRUE(recovered.log_truncated);
    EXPECT_EQ(recovered.valid_records, 1u);
    EXPECT_EQ(recovered.last_sequence, 1u);
    EXPECT_EQ(recovered.checkpoint_generation, 2u);
    EXPECT_TRUE(recovery_manager.validate(path).healthy);
    const auto again = recovery_manager.recover_startup_state(path);
    EXPECT_TRUE(again.healthy);
    EXPECT_EQ(again.valid_records, 1u);
    cleanup_log(path);
}

void test_checkpoint_rejects_two_corrupt_slots_and_mismatch() {
    const std::string path = "test_checkpoint_reject.bin";
    cleanup_log(path);
    {
        flight_recorder::BinaryLogWriter writer(path);
        EXPECT_TRUE(writer.open());
        EXPECT_TRUE(writer.append(make_record(1), 1));
    }
    const auto journal_path = flight_recorder::RecoveryManager::journal_path_for_log(path);
    for (std::size_t index = 0; index < flight_recorder::kCheckpointSlotCount; ++index) {
        flip_file_byte(journal_path,
                       index * sizeof(flight_recorder::PersistedCheckpointSlot) +
                           offsetof(flight_recorder::PersistedCheckpointSlot, checkpoint_crc32));
    }
    flight_recorder::RecoveryManager recovery_manager;
    auto report = recovery_manager.recover_startup_state(path);
    EXPECT_TRUE(!report.healthy);
    EXPECT_TRUE(report.message.find("no valid checkpoint") != std::string::npos);

    cleanup_log(path);
    {
        flight_recorder::BinaryLogWriter writer(path);
        EXPECT_TRUE(writer.open());
        EXPECT_TRUE(writer.append(make_record(1), 1));
    }
    auto mismatched = read_checkpoint_slots(path);
    for (auto& slot : mismatched) {
        if (flight_recorder::checkpoint_metadata_valid(slot)) {
            ++slot.recorder_start_time_us;
            slot.checkpoint_crc32 = flight_recorder::compute_checkpoint_crc(slot);
        }
    }
    std::ofstream output(journal_path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(mismatched.data()), sizeof(mismatched));
    output.close();
    report = recovery_manager.recover_startup_state(path);
    EXPECT_TRUE(!report.healthy);
    EXPECT_TRUE(report.message.find("no valid checkpoint") != std::string::npos);
    cleanup_log(path);
}

void test_empty_legacy_journal_migration() {
    const std::string path = "test_empty_legacy_migration.bin";
    cleanup_log(path);
    {
        flight_recorder::BinaryLogWriter writer(path);
        EXPECT_TRUE(writer.open());
        EXPECT_TRUE(writer.append(make_record(1), 1));
    }
    const auto journal_path = flight_recorder::RecoveryManager::journal_path_for_log(path);
    EXPECT_EQ(::truncate(journal_path.c_str(), 0), 0);
    flight_recorder::RecoveryManager recovery_manager;
    const auto report = recovery_manager.recover_startup_state(path);
    EXPECT_TRUE(report.healthy);
    EXPECT_TRUE(report.journal_migrated);
    EXPECT_EQ(report.valid_records, 1u);
    struct stat journal_stat {};
    EXPECT_EQ(::stat(journal_path.c_str(), &journal_stat), 0);
    EXPECT_EQ(static_cast<std::size_t>(journal_stat.st_size), flight_recorder::kCheckpointJournalSize);
    cleanup_log(path);
}

void test_missing_checkpoint_journal_fails_closed() {
    const std::string path = "test_missing_checkpoint.bin";
    cleanup_log(path);
    {
        flight_recorder::BinaryLogWriter writer(path);
        EXPECT_TRUE(writer.open());
        EXPECT_TRUE(writer.append(make_record(1), 1));
    }
    remove_if_present(flight_recorder::RecoveryManager::journal_path_for_log(path));
    flight_recorder::RecoveryManager recovery_manager;
    const auto report = recovery_manager.recover_startup_state(path);
    EXPECT_TRUE(!report.healthy);
    EXPECT_TRUE(report.message.find("journal missing") != std::string::npos);
    cleanup_log(path);
}

void test_recovery_after_interrupted_journaled_write() {
    const std::string path = "test_interrupted_commit.bin";
    const std::string journal_path = flight_recorder::RecoveryManager::journal_path_for_log(path);
    remove_if_present(path);
    remove_if_present(journal_path);

    flight_recorder::BinaryLogWriter writer(
        path,
        flight_recorder::RuntimeFaultConfig {
            flight_recorder::RuntimeFaultMode::DropCommit,
            2
        });
    EXPECT_TRUE(writer.open());
    EXPECT_TRUE(writer.append(make_record(100, 1000.0), 1));
    EXPECT_TRUE(!writer.append(make_record(200, 3456.0), 2));
    writer.close();

    flight_recorder::RecoveryManager recovery_manager;
    const auto pre_recovery_report = recovery_manager.validate(path);
    EXPECT_TRUE(pre_recovery_report.healthy);
    EXPECT_EQ(pre_recovery_report.valid_records, 2u);

    const auto startup = recovery_manager.recover_startup_state(path);
    EXPECT_TRUE(startup.healthy);
    EXPECT_TRUE(startup.journal_found);
    EXPECT_TRUE(startup.log_truncated);
    EXPECT_TRUE(!startup.journal_replayed);
    EXPECT_EQ(startup.last_sequence, 1u);

    flight_recorder::ReplayLog replay_log;
    std::string error;
    EXPECT_TRUE(recovery_manager.read_log(path, replay_log, error));
    EXPECT_EQ(replay_log.entries.size(), 1u);
    EXPECT_EQ(replay_log.entries.back().sequence, 1u);

    remove_if_present(path);
    remove_if_present(journal_path);
}

void test_startup_recovery_rolls_forward_pending_journal() {
    const std::string path = "test_startup_rollforward.bin";
    const std::string journal_path = flight_recorder::RecoveryManager::journal_path_for_log(path);
    remove_if_present(path);
    remove_if_present(journal_path);

    flight_recorder::BinaryLogWriter writer(path);
    EXPECT_TRUE(writer.open());
    EXPECT_TRUE(writer.append(make_record(100, 1500.0), 1));
    EXPECT_TRUE(writer.flush());
    writer.close();

    write_journal_file(path, make_journal_entry(2, 200, 2500.0));

    flight_recorder::RecoveryManager recovery_manager;
    const auto startup = recovery_manager.recover_startup_state(path);
    EXPECT_TRUE(startup.healthy);
    EXPECT_TRUE(startup.journal_replayed);
    EXPECT_EQ(startup.last_sequence, 2u);

    flight_recorder::ReplayLog replay_log;
    std::string error;
    EXPECT_TRUE(recovery_manager.read_log(path, replay_log, error));
    EXPECT_EQ(replay_log.entries.size(), 2u);
    EXPECT_EQ(replay_log.entries.back().record.altitude_m, 2500.0);

    struct stat journal_stat {};
    EXPECT_EQ(::stat(journal_path.c_str(), &journal_stat), 0);
    EXPECT_EQ(static_cast<std::size_t>(journal_stat.st_size), flight_recorder::kCheckpointJournalSize);

    remove_if_present(path);
    remove_if_present(journal_path);
}

void test_startup_recovery_clears_stale_journal() {
    const std::string path = "test_startup_stale.bin";
    const std::string journal_path = flight_recorder::RecoveryManager::journal_path_for_log(path);
    remove_if_present(path);
    remove_if_present(journal_path);

    flight_recorder::BinaryLogWriter writer(path);
    EXPECT_TRUE(writer.open());
    EXPECT_TRUE(writer.append(make_record(100, 1000.0), 1));
    EXPECT_TRUE(writer.append(make_record(200, 2000.0), 2));
    EXPECT_TRUE(writer.flush());
    writer.close();

    write_journal_file(path, make_journal_entry(2, 200, 2200.0));

    flight_recorder::RecoveryManager recovery_manager;
    const auto startup = recovery_manager.recover_startup_state(path);
    EXPECT_TRUE(startup.healthy);
    EXPECT_TRUE(startup.journal_cleared);
    EXPECT_TRUE(!startup.journal_replayed);
    EXPECT_EQ(startup.last_sequence, 2u);

    struct stat journal_stat {};
    EXPECT_EQ(::stat(journal_path.c_str(), &journal_stat), 0);
    EXPECT_EQ(static_cast<std::size_t>(journal_stat.st_size), flight_recorder::kCheckpointJournalSize);

    remove_if_present(path);
    remove_if_present(journal_path);
}

}  // namespace

int main() {
    const std::vector<TestCase> tests {
        {"CircularBuffer push/pop correctness", test_circular_buffer_push_pop_correctness},
        {"SPSC wraparound, full/empty, and batch drain", test_spsc_wraparound_full_empty_and_batch_drain},
        {"SPSC close wakes waiter", test_spsc_close_wakes_waiter},
        {"SPSC full-to-nonfull wakes producer", test_spsc_full_to_nonfull_wakes_producer},
        {"SPSC randomized model and concurrent ordering", test_spsc_randomized_model_and_concurrent_ordering},
        {"Recorder rejects zero capacity", test_recorder_rejects_zero_capacity},
        {"Recorder rejects invalid configuration", test_recorder_rejects_invalid_configuration},
        {"Recorder clean shutdown", test_recorder_clean_shutdown},
        {"Timed group commit while idle", test_timed_group_commit},
        {"Disabled timer preserves batch-only commits", test_group_commit_timer_disabled},
        {"Timed group commit under continuing load", test_group_commit_timer_under_load},
        {"Record serialization/deserialization", test_record_serialization_deserialization},
        {"Checksum validation", test_checksum_validation},
        {"Replay parsing of valid files", test_replay_parses_valid_file},
        {"Detection of truncated records", test_detection_of_truncated_records},
        {"Detection of corrupt records", test_detection_of_corrupt_records},
        {"File and record header corruption detection", test_file_and_record_header_corruption_detection},
        {"Timestamp, payload, and CRC corruption detection", test_timestamp_payload_and_crc_corruption_detection},
        {"Truncated header, payload, and CRC detection", test_truncated_header_payload_and_crc_detection},
        {"Duplicate, rewind, and gap sequences", test_duplicate_rewind_and_gap_sequences},
        {"Unknown format version rejection", test_unknown_format_version_rejected},
        {"Aligned full batch uses one write", test_aligned_full_batch_uses_one_write},
        {"Short write and EINTR retries", test_short_write_and_eintr_retries},
        {"Partial batch group flush and I/O failures", test_partial_batch_group_flush_and_io_failures},
        {"Checkpoint generations, counters, and idempotence", test_checkpoint_generations_commit_counters_and_idempotence},
        {"Checkpoint fallback from torn newest slot", test_checkpoint_falls_back_from_torn_newest_slot},
        {"Checkpoint rejects corrupt slots and mismatch", test_checkpoint_rejects_two_corrupt_slots_and_mismatch},
        {"Empty legacy journal migration", test_empty_legacy_journal_migration},
        {"Missing checkpoint journal fails closed", test_missing_checkpoint_journal_fails_closed},
        {"Recovery after interrupted journaled write", test_recovery_after_interrupted_journaled_write},
        {"Startup recovery rolls forward pending journal", test_startup_recovery_rolls_forward_pending_journal},
        {"Startup recovery clears stale journal", test_startup_recovery_clears_stale_journal},
    };

    std::size_t passed = 0;
    for (const auto& test : tests) {
        try {
            std::cout << "[RUN ] " << test.name << std::endl;
            test.run();
            ++passed;
            std::cout << "[PASS] " << test.name << std::endl;
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }

    std::cout << "Passed " << passed << " tests\n";
    return 0;
}
