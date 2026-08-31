#pragma once

#include "flight_recorder/fault_injection.hpp"
#include "flight_recorder/log_format.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace flight_recorder {

struct SequencedRecord {
    FlightRecord record;
    std::uint64_t sequence {0};
};

struct WriterOptions {
    std::size_t batch_capacity_records {64};
    std::size_t sync_every_batches {1};
    std::size_t preallocation_chunk_bytes {std::size_t {4} * 1024u * 1024u};
    std::size_t serialization_buffer_alignment {4096};
};

struct WriterStats {
    static constexpr std::uint64_t kLatencyBinWidthNs = 100;
    static constexpr std::size_t kLatencyFineBinCount = 10'000;
    static constexpr std::uint64_t kLatencyCoarseBinWidthNs = 10'000;
    static constexpr std::size_t kLatencyCoarseBinCount = 9'900;
    static constexpr std::size_t kLatencyBinCount =
        kLatencyFineBinCount + kLatencyCoarseBinCount + 1;
    std::uint64_t records_written {0};
    std::uint64_t batches_written {0};
    std::uint64_t bytes_written {0};
    std::uint64_t write_calls {0};
    std::uint64_t sync_calls {0};
    std::uint64_t preallocation_calls {0};
    std::uint64_t checkpoint_writes {0};
    std::uint64_t journal_sync_calls {0};
    std::uint64_t records_committed {0};
    std::uint64_t commits {0};
    std::uint64_t failures {0};
    std::uint64_t write_latency_samples {0};
    std::uint64_t max_write_latency_ns {0};
    std::array<std::uint64_t, kLatencyBinCount> write_latency_histogram {};
    std::uint64_t sync_latency_samples {0};
    std::uint64_t max_sync_latency_ns {0};
    std::array<std::uint64_t, kLatencyBinCount> sync_latency_histogram {};
};

struct LatencySummary {
    std::uint64_t samples {0};
    std::uint64_t p50_ns {0};
    std::uint64_t p95_ns {0};
    std::uint64_t p99_ns {0};
    std::uint64_t max_ns {0};
};

LatencySummary summarize_write_latency(const WriterStats& stats);
LatencySummary summarize_sync_latency(const WriterStats& stats);

class WriterIo {
public:
    virtual ~WriterIo() = default;
    virtual ssize_t pwrite_data(int fd, const void* data, std::size_t length, off_t offset);
    virtual int sync_data(int fd);
    virtual bool supports_keep_size_preallocation() const;
    virtual int preallocate_keep_size(int fd, off_t offset, off_t length);
};

class BinaryLogWriter {
public:
    explicit BinaryLogWriter(std::string path,
                             RuntimeFaultConfig fault_config = {},
                             WriterOptions options = {},
                             std::shared_ptr<WriterIo> io = {});
    ~BinaryLogWriter();

    BinaryLogWriter(const BinaryLogWriter&) = delete;
    BinaryLogWriter& operator=(const BinaryLogWriter&) = delete;

    bool open();
    bool append(const FlightRecord& record, std::uint64_t sequence);
    bool append_batch(const std::vector<SequencedRecord>& records);
    bool flush();
    void close();
    void reset_measurement_counters() { stats_ = WriterStats {}; }

    const std::string& path() const noexcept { return path_; }
    const std::string& journal_path() const noexcept { return journal_path_; }
    const WriterStats& stats() const noexcept { return stats_; }
    std::uintptr_t serialization_buffer_address() const noexcept {
        return reinterpret_cast<std::uintptr_t>(serialization_buffer_);
    }
    std::size_t serialization_buffer_alignment() const noexcept {
        return options_.serialization_buffer_alignment;
    }

private:
    bool allocate_serialization_buffer();
    bool open_journal();
    bool write_file_header();
    bool load_or_initialize_checkpoint();
    bool commit_checkpoint();
    bool ensure_preallocated(std::size_t upcoming_bytes);
    bool write_at_full(int fd,
                       const void* data,
                       std::size_t length,
                       off_t offset,
                       bool count_main_write);
    bool sync_main_file();
    void record_failure();

    std::string path_;
    std::string journal_path_;
    int fd_ {-1};
    int journal_fd_ {-1};
    std::uint64_t recorder_start_time_us_ {0};
    RuntimeFaultConfig fault_config_ {};
    WriterOptions options_ {};
    std::shared_ptr<WriterIo> io_;
    void* serialization_buffer_ {nullptr};
    off_t logical_end_ {0};
    off_t preallocated_end_ {0};
    std::size_t unsynced_batches_ {0};
    std::uint64_t checkpoint_generation_ {0};
    std::size_t active_checkpoint_slot_ {1};
    std::uint64_t file_record_count_ {0};
    std::uint64_t last_written_sequence_ {0};
    bool fault_injected_ {false};
    RuntimeFaultMode pending_commit_fault_ {RuntimeFaultMode::None};
    bool failed_ {false};
    WriterStats stats_ {};
};

}  // namespace flight_recorder
