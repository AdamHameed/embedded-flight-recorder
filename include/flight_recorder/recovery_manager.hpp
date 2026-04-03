#pragma once

#include "flight_recorder/log_format.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flight_recorder {

struct LogFileMetadata {
    std::uint16_t format_version {0};
    std::uint64_t recorder_start_time_us {0};
    std::uint32_t record_size {0};
};

struct RecoveryReport {
    bool healthy {false};
    bool truncated {false};
    std::size_t valid_records {0};
    std::uint64_t last_sequence {0};
    std::size_t valid_bytes {0};
    std::size_t checksum_failures {0};
    bool corruption_detected {false};
    std::size_t first_bad_record_index {0};
    std::uint64_t first_bad_record_sequence {0};
    std::size_t corruption_offset {0};
    std::string message;
};

struct StartupRecoveryReport {
    bool healthy {false};
    bool log_truncated {false};
    bool journal_found {false};
    bool journal_cleared {false};
    bool journal_replayed {false};
    bool journal_discarded {false};
    std::size_t valid_records {0};
    std::uint64_t last_sequence {0};
    std::size_t checksum_failures {0};
    std::string message;
};

struct ReplayEntry {
    std::uint64_t sequence {0};
    FlightRecord record;
};

struct ReplayLog {
    LogFileMetadata metadata;
    std::vector<ReplayEntry> entries;
};

class RecoveryManager {
public:
    static std::string journal_path_for_log(const std::string& path);

    RecoveryReport validate(const std::string& path) const;
    RecoveryReport recover(const std::string& path, bool truncate_invalid_tail) const;
    StartupRecoveryReport recover_startup_state(const std::string& path) const;
    bool scan_replayable_log(const std::string& path, ReplayLog& replay_log, RecoveryReport& report) const;
    bool read_log(const std::string& path, ReplayLog& replay_log, std::string& error) const;
    bool read_all(const std::string& path, std::vector<ReplayEntry>& out_records, std::string& error) const;
};

}  // namespace flight_recorder
