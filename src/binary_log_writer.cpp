#include "flight_recorder/binary_log_writer.hpp"

#include "flight_recorder/recovery_manager.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <fcntl.h>
#if defined(__linux__)
#include <linux/falloc.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

namespace flight_recorder {

namespace {

void observe_latency(std::array<std::uint64_t, WriterStats::kLatencyBinCount>& histogram,
                     std::uint64_t& samples,
                     std::uint64_t& maximum,
                     std::uint64_t elapsed_ns) {
    std::size_t bin = 0;
    const auto fine_limit = WriterStats::kLatencyFineBinCount * WriterStats::kLatencyBinWidthNs;
    if (elapsed_ns < fine_limit) {
        bin = static_cast<std::size_t>(elapsed_ns / WriterStats::kLatencyBinWidthNs);
    } else {
        bin = WriterStats::kLatencyFineBinCount +
              static_cast<std::size_t>((elapsed_ns - fine_limit) /
                                       WriterStats::kLatencyCoarseBinWidthNs);
        bin = std::min<std::size_t>(WriterStats::kLatencyBinCount - 1, bin);
    }
    ++histogram[bin];
    ++samples;
    maximum = std::max(maximum, elapsed_ns);
}

LatencySummary summarize_histogram(
    const std::array<std::uint64_t, WriterStats::kLatencyBinCount>& histogram,
    std::uint64_t samples,
    std::uint64_t maximum) {
    LatencySummary summary;
    summary.samples = samples;
    summary.max_ns = maximum;
    if (samples == 0) return summary;
    const auto percentile = [&](std::uint64_t numerator) {
        const std::uint64_t rank = (samples * numerator + 99u) / 100u;
        std::uint64_t cumulative = 0;
        for (std::size_t index = 0; index < histogram.size(); ++index) {
            cumulative += histogram[index];
            if (cumulative >= rank) {
                if (index + 1 == histogram.size()) return maximum;
                if (index < WriterStats::kLatencyFineBinCount) {
                    return static_cast<std::uint64_t>(index + 1) *
                           WriterStats::kLatencyBinWidthNs;
                }
                return WriterStats::kLatencyFineBinCount * WriterStats::kLatencyBinWidthNs +
                       static_cast<std::uint64_t>(
                           index - WriterStats::kLatencyFineBinCount + 1) *
                           WriterStats::kLatencyCoarseBinWidthNs;
            }
        }
        return maximum;
    };
    summary.p50_ns = percentile(50);
    summary.p95_ns = percentile(95);
    summary.p99_ns = percentile(99);
    return summary;
}

bool sync_parent_directory(const std::string& path) {
    const auto separator = path.find_last_of('/');
    const std::string directory = separator == std::string::npos ? "." :
        (separator == 0 ? "/" : path.substr(0, separator));
    const int directory_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory_fd < 0) {
        return false;
    }
    const bool ok = ::fsync(directory_fd) == 0;
    ::close(directory_fd);
    return ok;
}

}  // namespace

ssize_t WriterIo::pwrite_data(int fd, const void* data, std::size_t length, off_t offset) {
    return ::pwrite(fd, data, length, offset);
}

LatencySummary summarize_write_latency(const WriterStats& stats) {
    return summarize_histogram(stats.write_latency_histogram,
                               stats.write_latency_samples,
                               stats.max_write_latency_ns);
}

LatencySummary summarize_sync_latency(const WriterStats& stats) {
    return summarize_histogram(stats.sync_latency_histogram,
                               stats.sync_latency_samples,
                               stats.max_sync_latency_ns);
}

int WriterIo::sync_data(int fd) {
#if defined(__APPLE__)
    return ::fsync(fd);
#else
    return ::fdatasync(fd);
#endif
}

bool WriterIo::supports_keep_size_preallocation() const {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

int WriterIo::preallocate_keep_size(int fd, off_t offset, off_t length) {
#if defined(__linux__)
    return ::fallocate(fd, FALLOC_FL_KEEP_SIZE, offset, length);
#else
    (void)fd;
    (void)offset;
    (void)length;
    errno = ENOTSUP;
    return -1;
#endif
}

BinaryLogWriter::BinaryLogWriter(std::string path,
                                 RuntimeFaultConfig fault_config,
                                 WriterOptions options,
                                 std::shared_ptr<WriterIo> io)
    : path_(std::move(path)),
      journal_path_(RecoveryManager::journal_path_for_log(path_)),
      fault_config_(fault_config),
      options_(options),
      io_(io ? std::move(io) : std::make_shared<WriterIo>()) {}

BinaryLogWriter::~BinaryLogWriter() {
    close();
    std::free(serialization_buffer_);
}

bool BinaryLogWriter::allocate_serialization_buffer() {
    if (serialization_buffer_ != nullptr) {
        return true;
    }
    if (options_.batch_capacity_records == 0 ||
        options_.serialization_buffer_alignment < sizeof(void*) ||
        (options_.serialization_buffer_alignment &
         (options_.serialization_buffer_alignment - 1u)) != 0 ||
        options_.batch_capacity_records >
            std::numeric_limits<std::size_t>::max() / kPersistedRecordSize) {
        record_failure();
        return false;
    }
    const auto bytes = options_.batch_capacity_records * kPersistedRecordSize;
    if (::posix_memalign(&serialization_buffer_,
                         options_.serialization_buffer_alignment,
                         bytes) != 0) {
        serialization_buffer_ = nullptr;
        record_failure();
        return false;
    }
    return true;
}

bool BinaryLogWriter::open() {
    if (fd_ >= 0) {
        return true;
    }
    if (path_.empty() || options_.sync_every_batches == 0 || !allocate_serialization_buffer()) {
        record_failure();
        return false;
    }

    const bool file_existed = ::access(path_.c_str(), F_OK) == 0;
    const bool journal_existed = ::access(journal_path_.c_str(), F_OK) == 0;
    fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd_ < 0) {
        record_failure();
        return false;
    }

    struct stat file_stat {};
    if (::fstat(fd_, &file_stat) != 0) {
        record_failure();
        close();
        return false;
    }
    logical_end_ = file_stat.st_size;
    preallocated_end_ = logical_end_;

    if (file_stat.st_size == 0) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        recorder_start_time_us_ = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        if (!write_file_header() || !sync_main_file()) {
            close();
            return false;
        }
    } else {
        PersistedFileHeader header {};
        if (::pread(fd_, &header, sizeof(header), 0) != static_cast<ssize_t>(sizeof(header)) ||
            header.magic != kLogFileMagic || header.version != kLogFormatVersion ||
            compute_file_header_crc(header) != header.header_crc32 ||
            (logical_end_ - static_cast<off_t>(sizeof(header))) %
                    static_cast<off_t>(kPersistedRecordSize) != 0) {
            record_failure();
            close();
            return false;
        }
        recorder_start_time_us_ = header.recorder_start_time_us;
    }

    if (!open_journal() || !load_or_initialize_checkpoint()) {
        record_failure();
        close();
        return false;
    }
    if ((!file_existed || !journal_existed) && !sync_parent_directory(path_)) {
        record_failure();
        close();
        return false;
    }
    return true;
}

bool BinaryLogWriter::append(const FlightRecord& record, std::uint64_t sequence) {
    return append_batch(std::vector<SequencedRecord> {{record, sequence}});
}

bool BinaryLogWriter::append_batch(const std::vector<SequencedRecord>& records) {
    if (records.empty() || records.size() > options_.batch_capacity_records) {
        record_failure();
        return false;
    }
    if (fd_ < 0 && !open()) {
        return false;
    }
    for (std::size_t index = 1; index < records.size(); ++index) {
        if (records[index].sequence != records[index - 1].sequence + 1) {
            record_failure();
            return false;
        }
    }
    if (file_record_count_ != 0 && records.front().sequence != last_written_sequence_ + 1) {
        record_failure();
        return false;
    }

    auto* destination = static_cast<std::uint8_t*>(serialization_buffer_);
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& input = records[index];
        const PersistedFlightPayload payload = make_persisted_payload(input.record);
        const PersistedRecordHeader header {
            kLogRecordMagic,
            kLogFormatVersion,
            static_cast<std::uint16_t>(sizeof(PersistedRecordHeader)),
            static_cast<std::uint32_t>(sizeof(PersistedFlightPayload)),
            input.sequence,
            input.record.timestamp_us
        };
        const std::uint32_t crc = compute_record_crc(header, payload);
        std::memcpy(destination, &header, sizeof(header));
        destination += sizeof(header);
        std::memcpy(destination, &payload, sizeof(payload));
        destination += sizeof(payload);
        std::memcpy(destination, &crc, sizeof(crc));
        destination += sizeof(crc);

    }

    const bool fault_armed = !fault_injected_ &&
        fault_config_.mode != RuntimeFaultMode::None &&
        fault_config_.trigger_sequence >= records.front().sequence &&
        fault_config_.trigger_sequence <= records.back().sequence;

    if (fault_armed && fault_config_.mode == RuntimeFaultMode::CrashBeforeMainLogWrite) {
        fault_injected_ = true;
        ::_exit(81);
    }

    const std::size_t batch_bytes = records.size() * kPersistedRecordSize;
    if (!ensure_preallocated(batch_bytes)) {
        record_failure();
        return false;
    }

    if (fault_armed && fault_config_.mode == RuntimeFaultMode::CrashDuringMainLogWrite) {
        fault_injected_ = true;
        const auto requested = fault_config_.partial_write_bytes == 0
            ? batch_bytes / 2u : fault_config_.partial_write_bytes;
        const auto partial_size = std::min<std::size_t>(batch_bytes - 1u, requested);
        (void)write_at_full(fd_, serialization_buffer_, partial_size, logical_end_, true);
        (void)sync_main_file();
        ::_exit(92);
    }

    if (!write_at_full(fd_, serialization_buffer_, batch_bytes, logical_end_, true)) {
        record_failure();
        return false;
    }
    logical_end_ += static_cast<off_t>(batch_bytes);
    file_record_count_ += records.size();
    last_written_sequence_ = records.back().sequence;
    stats_.records_written += records.size();
    ++stats_.batches_written;
    ++unsynced_batches_;

    if (fault_armed && fault_config_.mode == RuntimeFaultMode::CrashAfterMainLogWrite) {
        fault_injected_ = true;
        ::_exit(83);
    }

    if (fault_armed && fault_config_.mode == RuntimeFaultMode::DropCommit) {
        fault_injected_ = true;
        record_failure();
        return false;
    }

    if (fault_armed) {
        pending_commit_fault_ = fault_config_.mode;
    }

    if (unsynced_batches_ >= options_.sync_every_batches && !flush()) {
        return false;
    }
    return true;
}

bool BinaryLogWriter::flush() {
    if (fd_ < 0 || failed_) {
        record_failure();
        return false;
    }
    if (unsynced_batches_ == 0) {
        return true;
    }
    if (pending_commit_fault_ == RuntimeFaultMode::CrashBeforeMainLogSync) {
        ::_exit(84);
    }
    if (!sync_main_file() || !commit_checkpoint()) {
        record_failure();
        return false;
    }
    if (pending_commit_fault_ == RuntimeFaultMode::CrashAfterJournalSync) {
        if (fault_config_.acknowledgement_fd >= 0) {
            const auto acknowledged = last_written_sequence_;
            if (::pwrite(fault_config_.acknowledgement_fd,
                         &acknowledged,
                         sizeof(acknowledged),
                         0) != static_cast<ssize_t>(sizeof(acknowledged)) ||
                ::fsync(fault_config_.acknowledgement_fd) != 0) {
                ::_exit(93);
            }
        }
        ::_exit(90);
    }
    if (pending_commit_fault_ != RuntimeFaultMode::None) {
        fault_injected_ = true;
    }
    pending_commit_fault_ = RuntimeFaultMode::None;
    unsynced_batches_ = 0;
    return true;
}

void BinaryLogWriter::close() {
    if (fd_ >= 0 && unsynced_batches_ != 0 && !failed_) {
        (void)flush();
    }
    if (journal_fd_ >= 0) {
        ::close(journal_fd_);
        journal_fd_ = -1;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool BinaryLogWriter::open_journal() {
    if (journal_fd_ >= 0) {
        return true;
    }
    journal_fd_ = ::open(journal_path_.c_str(), O_CREAT | O_RDWR, 0644);
    return journal_fd_ >= 0;
}

bool BinaryLogWriter::load_or_initialize_checkpoint() {
    struct stat journal_stat {};
    if (::fstat(journal_fd_, &journal_stat) != 0) {
        return false;
    }

    if (journal_stat.st_size == 0) {
        file_record_count_ = static_cast<std::uint64_t>(
            (logical_end_ - static_cast<off_t>(sizeof(PersistedFileHeader))) /
            static_cast<off_t>(kPersistedRecordSize));
        if (file_record_count_ != 0) {
            PersistedRecordHeader last_header {};
            const off_t offset = logical_end_ - static_cast<off_t>(kPersistedRecordSize);
            if (::pread(fd_, &last_header, sizeof(last_header), offset) !=
                static_cast<ssize_t>(sizeof(last_header))) {
                return false;
            }
            last_written_sequence_ = last_header.sequence;
        }
        checkpoint_generation_ = 0;
        active_checkpoint_slot_ = 1;
        return commit_checkpoint();
    }

    if (static_cast<std::size_t>(journal_stat.st_size) != kCheckpointJournalSize) {
        return false;
    }
    PersistedCheckpointSlot slots[kCheckpointSlotCount] {};
    if (::pread(journal_fd_, slots, sizeof(slots), 0) != static_cast<ssize_t>(sizeof(slots))) {
        return false;
    }
    const PersistedCheckpointSlot* selected = nullptr;
    for (std::size_t index = 0; index < kCheckpointSlotCount; ++index) {
        const auto& slot = slots[index];
        if (!checkpoint_metadata_valid(slot) ||
            slot.recorder_start_time_us != recorder_start_time_us_ ||
            slot.committed_length < sizeof(PersistedFileHeader) ||
            slot.committed_length != static_cast<std::uint64_t>(logical_end_) ||
            slot.record_count !=
                (slot.committed_length - sizeof(PersistedFileHeader)) / kPersistedRecordSize) {
            continue;
        }
        if (selected == nullptr || slot.generation > selected->generation) {
            selected = &slot;
            active_checkpoint_slot_ = index;
        }
    }
    if (selected == nullptr) {
        return false;
    }
    checkpoint_generation_ = selected->generation;
    file_record_count_ = selected->record_count;
    last_written_sequence_ = selected->last_sequence;
    return true;
}

bool BinaryLogWriter::commit_checkpoint() {
    if (checkpoint_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    const std::size_t slot_index = (active_checkpoint_slot_ + 1) % kCheckpointSlotCount;
    PersistedCheckpointSlot slot {
        kJournalMagic,
        kCheckpointJournalVersion,
        static_cast<std::uint16_t>(sizeof(PersistedCheckpointSlot)),
        checkpoint_generation_ + 1,
        recorder_start_time_us_,
        static_cast<std::uint64_t>(logical_end_),
        file_record_count_,
        last_written_sequence_,
        0
    };
    slot.checkpoint_crc32 = compute_checkpoint_crc(slot);
    const off_t offset = static_cast<off_t>(slot_index) * static_cast<off_t>(sizeof(slot));
    if (pending_commit_fault_ == RuntimeFaultMode::CrashAfterMainLogSync) {
        ::_exit(85);
    }
    if (pending_commit_fault_ == RuntimeFaultMode::CrashBeforeCheckpointWrite) {
        ::_exit(86);
    }
    if (pending_commit_fault_ == RuntimeFaultMode::CrashDuringCheckpointWrite) {
        const auto requested = fault_config_.partial_write_bytes == 0
            ? sizeof(slot) / 2u : fault_config_.partial_write_bytes;
        const auto partial_size = std::min<std::size_t>(sizeof(slot) - 1u, requested);
        (void)io_->pwrite_data(journal_fd_, &slot, partial_size, offset);
        ::_exit(87);
    }
    if (!write_at_full(journal_fd_, &slot, sizeof(slot), offset, false) ||
        ::ftruncate(journal_fd_, static_cast<off_t>(kCheckpointJournalSize)) != 0) {
        return false;
    }
    ++stats_.checkpoint_writes;
    if (pending_commit_fault_ == RuntimeFaultMode::CrashAfterCheckpointWrite) {
        ::_exit(88);
    }
    if (pending_commit_fault_ == RuntimeFaultMode::CrashBeforeJournalSync) {
        ::_exit(89);
    }
    ++stats_.journal_sync_calls;
    if (io_->sync_data(journal_fd_) != 0) {
        return false;
    }
    checkpoint_generation_ = slot.generation;
    active_checkpoint_slot_ = slot_index;
    stats_.records_committed = stats_.records_written;
    ++stats_.commits;
    return true;
}

bool BinaryLogWriter::write_file_header() {
    PersistedFileHeader header {
        kLogFileMagic,
        kLogFormatVersion,
        static_cast<std::uint16_t>(sizeof(PersistedFileHeader)),
        recorder_start_time_us_,
        static_cast<std::uint32_t>(kPersistedRecordSize),
        0
    };
    header.header_crc32 = compute_file_header_crc(header);
    if (!write_at_full(fd_, &header, sizeof(header), 0, false)) {
        record_failure();
        return false;
    }
    logical_end_ = sizeof(header);
    return true;
}

bool BinaryLogWriter::ensure_preallocated(std::size_t upcoming_bytes) {
    if (options_.preallocation_chunk_bytes == 0 || !io_->supports_keep_size_preallocation()) {
        return true;
    }
    const off_t required = logical_end_ + static_cast<off_t>(upcoming_bytes);
    if (required <= preallocated_end_) {
        return true;
    }
    const auto chunk = static_cast<off_t>(options_.preallocation_chunk_bytes);
    const off_t target = ((required + chunk - 1) / chunk) * chunk;
    ++stats_.preallocation_calls;
    if (io_->preallocate_keep_size(fd_, preallocated_end_, target - preallocated_end_) != 0) {
        return false;
    }
    preallocated_end_ = target;
    return true;
}

bool BinaryLogWriter::write_at_full(int fd,
                                    const void* data,
                                    std::size_t length,
                                    off_t offset,
                                    bool count_main_write) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t total_written = 0;
    while (total_written < length) {
        if (count_main_write) {
            ++stats_.write_calls;
        }
        const auto start = std::chrono::steady_clock::now();
        const ssize_t written = io_->pwrite_data(
            fd, bytes + total_written, length - total_written,
            offset + static_cast<off_t>(total_written));
        const auto end = std::chrono::steady_clock::now();
        if (count_main_write) {
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            observe_latency(stats_.write_latency_histogram,
                            stats_.write_latency_samples,
                            stats_.max_write_latency_ns,
                            elapsed);
        }
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        total_written += static_cast<std::size_t>(written);
        if (count_main_write) {
            stats_.bytes_written += static_cast<std::size_t>(written);
        }
    }
    return true;
}

bool BinaryLogWriter::sync_main_file() {
    ++stats_.sync_calls;
    const auto start = std::chrono::steady_clock::now();
    const int result = io_->sync_data(fd_);
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    observe_latency(stats_.sync_latency_histogram,
                    stats_.sync_latency_samples,
                    stats_.max_sync_latency_ns,
                    elapsed);
    if (result != 0) {
        record_failure();
        return false;
    }
    return true;
}

void BinaryLogWriter::record_failure() {
    failed_ = true;
    ++stats_.failures;
}

}  // namespace flight_recorder
