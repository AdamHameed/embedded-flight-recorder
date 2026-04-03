#include "flight_recorder/binary_log_writer.hpp"

#include "flight_recorder/recovery_manager.hpp"

#include <cerrno>
#include <chrono>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flight_recorder {

namespace {

int durable_sync(int fd) {
#if defined(__APPLE__)
    return ::fsync(fd);
#else
    return ::fdatasync(fd);
#endif
}

}  // namespace

BinaryLogWriter::BinaryLogWriter(std::string path, RuntimeFaultConfig fault_config)
    : path_(std::move(path)),
      journal_path_(RecoveryManager::journal_path_for_log(path_)),
      fault_config_(fault_config) {}

BinaryLogWriter::~BinaryLogWriter() {
    close();
}

bool BinaryLogWriter::open() {
    if (fd_ >= 0) {
        return true;
    }

    fd_ = ::open(path_.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd_ < 0) {
        return false;
    }

    struct stat file_stat {};
    if (::fstat(fd_, &file_stat) != 0) {
        close();
        return false;
    }

    if (file_stat.st_size == 0) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        recorder_start_time_us_ = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now).count());

        if (!write_file_header() || durable_sync(fd_) != 0) {
            close();
            return false;
        }
    }

    if (!open_journal()) {
        close();
        return false;
    }

    return true;
}

bool BinaryLogWriter::append(const FlightRecord& record, std::uint64_t sequence) {
    if (fd_ < 0 && !open()) {
        return false;
    }

    const PersistedFlightPayload payload = make_persisted_payload(record);
    const PersistedRecordHeader header {
        kLogRecordMagic,
        kLogFormatVersion,
        static_cast<std::uint16_t>(sizeof(PersistedRecordHeader)),
        static_cast<std::uint32_t>(sizeof(PersistedFlightPayload)),
        sequence,
        record.timestamp_us
    };
    const std::uint32_t record_crc = compute_record_crc(header, payload);

    PersistedJournalEntry journal_entry {
        kJournalMagic,
        kLogFormatVersion,
        static_cast<std::uint16_t>(sizeof(PersistedJournalEntry)),
        kJournalStatePending,
        static_cast<std::uint32_t>(sizeof(PersistedJournalEntry)),
        sequence,
        header,
        payload,
        record_crc,
        0
    };
    journal_entry.journal_crc32 = compute_journal_crc(journal_entry);

    // This is a small write-ahead log:
    // 1. Persist the intent into the sidecar journal.
    // 2. Append the real record to the main log.
    // 3. Clear the journal after the main log is durable.
    //
    // That protects against crashes between intent creation and record commit. It does not
    // protect against media corruption that affects both files or directory metadata loss.
    if (!write_journal_intent(journal_entry)) {
        return false;
    }

    const bool fault_armed =
        !fault_injected_ &&
        fault_config_.mode != RuntimeFaultMode::None &&
        sequence == fault_config_.trigger_sequence;

    if (fault_armed && fault_config_.mode == RuntimeFaultMode::CrashAfterJournalSync) {
        fault_injected_ = true;
        ::_exit(91);
    }
    if (fault_armed && fault_config_.mode == RuntimeFaultMode::DropCommit) {
        fault_injected_ = true;
        return false;
    }

    if (fault_armed && fault_config_.mode == RuntimeFaultMode::CrashDuringMainLogWrite) {
        fault_injected_ = true;
        if (!write_full(fd_, &header, sizeof(header))) {
            return false;
        }
        const auto partial_payload_size = sizeof(payload) / 2u;
        if (!write_full(fd_, &payload, partial_payload_size) || durable_sync(fd_) != 0) {
            return false;
        }
        ::_exit(92);
    }

    if (!write_full(fd_, &header, sizeof(header)) ||
        !write_full(fd_, &payload, sizeof(payload)) ||
        !write_full(fd_, &record_crc, sizeof(record_crc)) ||
        durable_sync(fd_) != 0) {
        return false;
    }
    if (!clear_journal()) {
        return false;
    }

    return true;
}

bool BinaryLogWriter::flush() {
    if (fd_ < 0) {
        return false;
    }
    return durable_sync(fd_) == 0;
}

void BinaryLogWriter::close() {
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

bool BinaryLogWriter::write_file_header() {
    const PersistedFileHeader header {
        kLogFileMagic,
        kLogFormatVersion,
        static_cast<std::uint16_t>(sizeof(PersistedFileHeader)),
        recorder_start_time_us_,
        static_cast<std::uint32_t>(kPersistedRecordSize),
        0
    };

    PersistedFileHeader header_with_crc = header;
    header_with_crc.header_crc32 = compute_file_header_crc(header_with_crc);

    return write_full(fd_, &header_with_crc, sizeof(header_with_crc));
}

bool BinaryLogWriter::write_journal_intent(const PersistedJournalEntry& entry) {
    if (!open_journal()) {
        return false;
    }
    if (::ftruncate(journal_fd_, 0) != 0) {
        return false;
    }
    if (::lseek(journal_fd_, 0, SEEK_SET) < 0) {
        return false;
    }
    if (!write_full(journal_fd_, &entry, sizeof(entry))) {
        return false;
    }
    if (::ftruncate(journal_fd_, static_cast<off_t>(sizeof(entry))) != 0) {
        return false;
    }
    return durable_sync(journal_fd_) == 0;
}

bool BinaryLogWriter::clear_journal() {
    if (!open_journal()) {
        return false;
    }
    if (::ftruncate(journal_fd_, 0) != 0) {
        return false;
    }
    if (::lseek(journal_fd_, 0, SEEK_SET) < 0) {
        return false;
    }
    return durable_sync(journal_fd_) == 0;
}

bool BinaryLogWriter::write_full(int fd, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t total_written = 0;

    while (total_written < length) {
        const ssize_t bytes_written = ::write(fd, bytes + total_written, length - total_written);
        if (bytes_written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (bytes_written == 0) {
            return false;
        }
        total_written += static_cast<std::size_t>(bytes_written);
    }

    return true;
}

}  // namespace flight_recorder
