#include "flight_recorder/fault_injection.hpp"

#include "flight_recorder/recovery_manager.hpp"

#include <fstream>

#include <sys/stat.h>
#include <unistd.h>

namespace flight_recorder {

namespace {

bool path_exists(const std::string& path) {
    struct stat file_stat {};
    return ::stat(path.c_str(), &file_stat) == 0;
}

}  // namespace

bool FaultInjector::truncate_log_tail(const std::string& path,
                                      std::size_t bytes_to_remove,
                                      std::string& error) {
    if (!path_exists(path)) {
        error = "log file does not exist";
        return false;
    }

    struct stat file_stat {};
    if (::stat(path.c_str(), &file_stat) != 0) {
        error = "failed to stat log file";
        return false;
    }

    const auto current_size = static_cast<std::size_t>(file_stat.st_size);
    if (bytes_to_remove >= current_size) {
        error = "refusing to truncate entire file";
        return false;
    }

    if (::truncate(path.c_str(), static_cast<off_t>(current_size - bytes_to_remove)) != 0) {
        error = "truncate failed";
        return false;
    }

    error.clear();
    return true;
}

bool FaultInjector::corrupt_record_byte(const std::string& path,
                                        std::uint64_t sequence,
                                        std::size_t byte_offset_in_record,
                                        std::string& error) {
    if (byte_offset_in_record >= kPersistedRecordSize) {
        error = "record byte offset is out of range";
        return false;
    }

    RecoveryManager recovery_manager;
    ReplayLog replay_log;
    RecoveryReport report;
    if (!recovery_manager.scan_replayable_log(path, replay_log, report)) {
        error = report.message;
        return false;
    }

    bool found = false;
    std::size_t record_index = 0;
    for (std::size_t i = 0; i < replay_log.entries.size(); ++i) {
        if (replay_log.entries[i].sequence == sequence) {
            found = true;
            record_index = i;
            break;
        }
    }
    if (!found) {
        error = "sequence not found in valid log prefix";
        return false;
    }

    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        error = "failed to open log file for corruption injection";
        return false;
    }

    const auto record_offset =
        sizeof(PersistedFileHeader) + (record_index * kPersistedRecordSize) + byte_offset_in_record;
    file.seekg(static_cast<std::streamoff>(record_offset));
    char current = 0;
    file.read(&current, 1);
    if (!file) {
        error = "failed to read target byte";
        return false;
    }

    current ^= static_cast<char>(0x5A);
    file.seekp(static_cast<std::streamoff>(record_offset));
    file.write(&current, 1);
    if (!file) {
        error = "failed to write corrupted byte";
        return false;
    }

    error.clear();
    return true;
}

}  // namespace flight_recorder
