#include "flight_recorder/binary_log_writer.hpp"
#include "flight_recorder/circular_buffer.hpp"
#include "flight_recorder/log_format.hpp"
#include "flight_recorder/recovery_manager.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace {

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
    assert(output);
    output.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
}

void test_circular_buffer_overwrites_oldest() {
    flight_recorder::CircularBuffer buffer(2);

    const auto first = buffer.push_wait_for(flight_recorder::FlightRecord {.timestamp_us = 1},
                                            std::chrono::milliseconds(1));
    const auto second = buffer.push_wait_for(flight_recorder::FlightRecord {.timestamp_us = 2},
                                             std::chrono::milliseconds(1));
    const auto third = buffer.push_wait_for(flight_recorder::FlightRecord {.timestamp_us = 3},
                                            std::chrono::milliseconds(1));

    assert(first == flight_recorder::CircularBuffer::PushStatus::Pushed);
    assert(second == flight_recorder::CircularBuffer::PushStatus::Pushed);
    assert(third == flight_recorder::CircularBuffer::PushStatus::Timeout);
    assert(buffer.stats().dropped_records == 1);

    flight_recorder::FlightRecord record {};
    assert(buffer.try_pop(record));
    assert(record.timestamp_us == 1);
    assert(buffer.try_pop(record));
    assert(record.timestamp_us == 2);
}

void test_log_write_and_replay() {
    const std::string path = "test_flight_log.bin";
    std::remove(path.c_str());

    flight_recorder::BinaryLogWriter writer(path);
    assert(writer.open());

    flight_recorder::FlightRecord record {};
    record.timestamp_us = 1234;
    record.altitude_m = 2000.0;
    record.airspeed_kts = 145.0;
    record.heading_deg = 87.0;
    record.vertical_speed_fpm = 300.0;
    record.engine_temperature_c = 640.0;
    record.engine_rpm = 2200.0;
    record.system_status = flight_recorder::StatusNominal;

    assert(writer.append(record, 1));
    assert(writer.flush());
    writer.close();

    flight_recorder::RecoveryManager recovery_manager;
    flight_recorder::ReplayLog replay_log;
    std::string error;
    assert(recovery_manager.read_log(path, replay_log, error));
    assert(replay_log.metadata.format_version == flight_recorder::kLogFormatVersion);
    assert(replay_log.metadata.record_size == flight_recorder::kPersistedRecordSize);
    assert(replay_log.entries.size() == 1);
    assert(replay_log.entries.front().sequence == 1);
    assert(replay_log.entries.front().record.timestamp_us == 1234);

    std::remove(path.c_str());
}

void test_recovery_detects_truncated_tail() {
    const std::string path = "test_corrupt_log.bin";
    std::remove(path.c_str());

    flight_recorder::BinaryLogWriter writer(path);
    assert(writer.open());
    assert(writer.append(flight_recorder::FlightRecord {.timestamp_us = 11}, 1));
    assert(writer.flush());
    writer.close();

    {
        std::ofstream output(path, std::ios::binary | std::ios::app);
        const char partial[] = {0x01, 0x02, 0x03};
        output.write(partial, sizeof(partial));
    }

    flight_recorder::RecoveryManager recovery_manager;
    const auto report = recovery_manager.validate(path);
    assert(!report.healthy);
    assert(report.valid_records == 1);

    const auto recovered = recovery_manager.recover(path, true);
    assert(recovered.truncated);

    std::remove(path.c_str());
}

void test_recovery_rejects_bad_file_header_crc() {
    const std::string path = "test_bad_header.bin";
    std::remove(path.c_str());

    flight_recorder::BinaryLogWriter writer(path);
    assert(writer.open());
    assert(writer.append(flight_recorder::FlightRecord {.timestamp_us = 77}, 1));
    assert(writer.flush());
    writer.close();

    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        assert(file);
        flight_recorder::PersistedFileHeader header {};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        assert(file);
        header.header_crc32 ^= 0xFFFFFFFFu;
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }

    flight_recorder::RecoveryManager recovery_manager;
    const auto report = recovery_manager.validate(path);
    assert(!report.healthy);
    assert(report.message.find("file header CRC") != std::string::npos);

    std::remove(path.c_str());
}

void test_recovery_rejects_bad_record_crc() {
    const std::string path = "test_bad_record_crc.bin";
    std::remove(path.c_str());

    flight_recorder::BinaryLogWriter writer(path);
    assert(writer.open());
    assert(writer.append(flight_recorder::FlightRecord {.timestamp_us = 88, .altitude_m = 123.0}, 1));
    assert(writer.flush());
    writer.close();

    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        assert(file);
        file.seekp(static_cast<std::streamoff>(sizeof(flight_recorder::PersistedFileHeader) +
                                              sizeof(flight_recorder::PersistedRecordHeader) + 4));
        const char byte = static_cast<char>(0xFF);
        file.write(&byte, 1);
    }

    flight_recorder::RecoveryManager recovery_manager;
    const auto report = recovery_manager.validate(path);
    assert(!report.healthy);
    assert(report.valid_records == 0);
    assert(report.message.find("CRC mismatch") != std::string::npos);

    std::remove(path.c_str());
}

void test_startup_recovery_rolls_forward_pending_journal() {
    const std::string path = "test_startup_rollforward.bin";
    const std::string journal_path = flight_recorder::RecoveryManager::journal_path_for_log(path);
    std::remove(path.c_str());
    std::remove(journal_path.c_str());

    flight_recorder::BinaryLogWriter writer(path);
    assert(writer.open());
    assert(writer.append(flight_recorder::FlightRecord {.timestamp_us = 100}, 1));
    assert(writer.flush());
    writer.close();

    write_journal_file(path, make_journal_entry(2, 200, 2500.0));

    flight_recorder::RecoveryManager recovery_manager;
    const auto startup = recovery_manager.recover_startup_state(path);
    assert(startup.healthy);
    assert(startup.journal_replayed);
    assert(startup.last_sequence == 2);

    flight_recorder::ReplayLog replay_log;
    std::string error;
    assert(recovery_manager.read_log(path, replay_log, error));
    assert(replay_log.entries.size() == 2);
    assert(replay_log.entries.back().sequence == 2);
    assert(replay_log.entries.back().record.altitude_m == 2500.0);

    struct stat journal_stat {};
    assert(::stat(journal_path.c_str(), &journal_stat) == 0);
    assert(journal_stat.st_size == 0);

    std::remove(path.c_str());
    std::remove(journal_path.c_str());
}

void test_startup_recovery_clears_stale_journal() {
    const std::string path = "test_startup_stale.bin";
    const std::string journal_path = flight_recorder::RecoveryManager::journal_path_for_log(path);
    std::remove(path.c_str());
    std::remove(journal_path.c_str());

    flight_recorder::BinaryLogWriter writer(path);
    assert(writer.open());
    assert(writer.append(flight_recorder::FlightRecord {.timestamp_us = 100}, 1));
    assert(writer.append(flight_recorder::FlightRecord {.timestamp_us = 200}, 2));
    assert(writer.flush());
    writer.close();

    write_journal_file(path, make_journal_entry(2, 200, 2200.0));

    flight_recorder::RecoveryManager recovery_manager;
    const auto startup = recovery_manager.recover_startup_state(path);
    assert(startup.healthy);
    assert(startup.journal_cleared);
    assert(!startup.journal_replayed);
    assert(startup.last_sequence == 2);

    struct stat journal_stat {};
    assert(::stat(journal_path.c_str(), &journal_stat) == 0);
    assert(journal_stat.st_size == 0);

    std::remove(path.c_str());
    std::remove(journal_path.c_str());
}

}  // namespace

int main() {
    test_circular_buffer_overwrites_oldest();
    test_log_write_and_replay();
    test_recovery_detects_truncated_tail();
    test_recovery_rejects_bad_file_header_crc();
    test_recovery_rejects_bad_record_crc();
    test_startup_recovery_rolls_forward_pending_journal();
    test_startup_recovery_clears_stale_journal();
    return 0;
}
