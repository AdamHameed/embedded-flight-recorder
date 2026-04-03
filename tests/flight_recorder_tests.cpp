#include "flight_recorder/binary_log_writer.hpp"
#include "flight_recorder/circular_buffer.hpp"
#include "flight_recorder/crc32.hpp"
#include "flight_recorder/fault_injection.hpp"
#include "flight_recorder/log_format.hpp"
#include "flight_recorder/recovery_manager.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
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
    const std::string path = "test_truncated.bin";
    remove_if_present(path);

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
    EXPECT_EQ(pre_recovery_report.valid_records, 1u);

    const auto startup = recovery_manager.recover_startup_state(path);
    EXPECT_TRUE(startup.healthy);
    EXPECT_TRUE(startup.journal_found);
    EXPECT_TRUE(startup.journal_replayed);
    EXPECT_EQ(startup.last_sequence, 2u);

    flight_recorder::ReplayLog replay_log;
    std::string error;
    EXPECT_TRUE(recovery_manager.read_log(path, replay_log, error));
    EXPECT_EQ(replay_log.entries.size(), 2u);
    EXPECT_EQ(replay_log.entries.back().sequence, 2u);
    EXPECT_EQ(replay_log.entries.back().record.altitude_m, 3456.0);

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
    EXPECT_EQ(static_cast<std::size_t>(journal_stat.st_size), 0u);

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
    EXPECT_EQ(static_cast<std::size_t>(journal_stat.st_size), 0u);

    remove_if_present(path);
    remove_if_present(journal_path);
}

}  // namespace

int main() {
    const std::vector<TestCase> tests {
        {"CircularBuffer push/pop correctness", test_circular_buffer_push_pop_correctness},
        {"Record serialization/deserialization", test_record_serialization_deserialization},
        {"Checksum validation", test_checksum_validation},
        {"Replay parsing of valid files", test_replay_parses_valid_file},
        {"Detection of truncated records", test_detection_of_truncated_records},
        {"Detection of corrupt records", test_detection_of_corrupt_records},
        {"Recovery after interrupted journaled write", test_recovery_after_interrupted_journaled_write},
        {"Startup recovery rolls forward pending journal", test_startup_recovery_rolls_forward_pending_journal},
        {"Startup recovery clears stale journal", test_startup_recovery_clears_stale_journal},
    };

    std::size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }

    std::cout << "Passed " << passed << " tests\n";
    return 0;
}
