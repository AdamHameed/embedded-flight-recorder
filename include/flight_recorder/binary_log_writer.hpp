#pragma once

#include "flight_recorder/fault_injection.hpp"
#include "flight_recorder/log_format.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace flight_recorder {

class BinaryLogWriter {
public:
    explicit BinaryLogWriter(std::string path, RuntimeFaultConfig fault_config = {});
    ~BinaryLogWriter();

    BinaryLogWriter(const BinaryLogWriter&) = delete;
    BinaryLogWriter& operator=(const BinaryLogWriter&) = delete;

    bool open();
    bool append(const FlightRecord& record, std::uint64_t sequence);
    bool flush();
    void close();

    const std::string& path() const noexcept { return path_; }
    const std::string& journal_path() const noexcept { return journal_path_; }

private:
    bool open_journal();
    bool write_file_header();
    bool write_journal_intent(const PersistedJournalEntry& entry);
    bool clear_journal();
    bool write_full(int fd, const void* data, std::size_t length);

    std::string path_;
    std::string journal_path_;
    int fd_ {-1};
    int journal_fd_ {-1};
    std::uint64_t recorder_start_time_us_ {0};
    RuntimeFaultConfig fault_config_ {};
    bool fault_injected_ {false};
};

}  // namespace flight_recorder
