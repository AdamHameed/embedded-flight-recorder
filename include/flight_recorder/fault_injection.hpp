#pragma once

#include "flight_recorder/log_format.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace flight_recorder {

enum class RuntimeFaultMode {
    None,
    CrashBeforeMainLogWrite,
    CrashAfterJournalSync,
    CrashDuringMainLogWrite,
    CrashAfterMainLogWrite,
    CrashBeforeMainLogSync,
    CrashAfterMainLogSync,
    CrashBeforeCheckpointWrite,
    CrashDuringCheckpointWrite,
    CrashAfterCheckpointWrite,
    CrashBeforeJournalSync,
    DropCommit
};

struct RuntimeFaultConfig {
    RuntimeFaultMode mode {RuntimeFaultMode::None};
    std::uint64_t trigger_sequence {1};
    std::size_t partial_write_bytes {0};
    int acknowledgement_fd {-1};
};

class FaultInjector {
public:
    static bool truncate_log_tail(const std::string& path, std::size_t bytes_to_remove, std::string& error);
    static bool corrupt_record_byte(const std::string& path,
                                    std::uint64_t sequence,
                                    std::size_t byte_offset_in_record,
                                    std::string& error);
};

}  // namespace flight_recorder
