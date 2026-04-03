#include "flight_recorder/recovery_manager.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flight_recorder {

namespace {

struct JournalInspection {
    enum class Status {
        Empty,
        Pending,
        Corrupt
    };

    Status status {Status::Empty};
    PersistedJournalEntry entry {};
    std::string message;
};

bool read_exact(std::ifstream& stream, void* buffer, std::size_t length) {
    stream.read(static_cast<char*>(buffer), static_cast<std::streamsize>(length));
    return static_cast<std::size_t>(stream.gcount()) == length;
}

bool write_full(int fd, const void* data, std::size_t length) {
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

int durable_sync(int fd) {
#if defined(__APPLE__)
    return ::fsync(fd);
#else
    return ::fdatasync(fd);
#endif
}

bool path_exists(const std::string& path) {
    struct stat file_stat {};
    return ::stat(path.c_str(), &file_stat) == 0;
}

JournalInspection inspect_journal(const std::string& journal_path) {
    JournalInspection inspection;
    if (!path_exists(journal_path)) {
        inspection.message = "journal not present";
        return inspection;
    }

    struct stat file_stat {};
    if (::stat(journal_path.c_str(), &file_stat) != 0) {
        inspection.status = JournalInspection::Status::Corrupt;
        inspection.message = "failed to stat journal";
        return inspection;
    }
    if (file_stat.st_size == 0) {
        inspection.message = "journal empty";
        return inspection;
    }
    if (static_cast<std::size_t>(file_stat.st_size) != sizeof(PersistedJournalEntry)) {
        inspection.status = JournalInspection::Status::Corrupt;
        inspection.message = "journal size mismatch";
        return inspection;
    }

    std::ifstream input(journal_path, std::ios::binary);
    if (!input || !read_exact(input, &inspection.entry, sizeof(inspection.entry))) {
        inspection.status = JournalInspection::Status::Corrupt;
        inspection.message = "journal read failed";
        return inspection;
    }

    const auto& entry = inspection.entry;
    if (entry.magic != kJournalMagic ||
        entry.version != kLogFormatVersion ||
        entry.header_size != sizeof(PersistedJournalEntry) ||
        entry.entry_size != sizeof(PersistedJournalEntry) ||
        entry.state != kJournalStatePending) {
        inspection.status = JournalInspection::Status::Corrupt;
        inspection.message = "journal metadata invalid";
        return inspection;
    }
    if (compute_journal_crc(entry) != entry.journal_crc32) {
        inspection.status = JournalInspection::Status::Corrupt;
        inspection.message = "journal CRC invalid";
        return inspection;
    }
    if (entry.record_header.magic != kLogRecordMagic ||
        entry.record_header.version != kLogFormatVersion ||
        entry.record_header.header_size != sizeof(PersistedRecordHeader) ||
        entry.record_header.payload_size != sizeof(PersistedFlightPayload) ||
        entry.record_header.sequence != entry.sequence ||
        compute_record_crc(entry.record_header, entry.payload) != entry.record_crc32) {
        inspection.status = JournalInspection::Status::Corrupt;
        inspection.message = "journal payload invalid";
        return inspection;
    }

    inspection.status = JournalInspection::Status::Pending;
    inspection.message = "pending journal entry found";
    return inspection;
}

bool clear_journal_file(const std::string& journal_path) {
    const int fd = ::open(journal_path.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return false;
    }
    const bool ok = (::ftruncate(fd, 0) == 0) && (::lseek(fd, 0, SEEK_SET) >= 0) && (durable_sync(fd) == 0);
    ::close(fd);
    return ok;
}

bool append_recovered_record(const std::string& path, const PersistedJournalEntry& entry) {
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) {
        return false;
    }

    const bool ok =
        write_full(fd, &entry.record_header, sizeof(entry.record_header)) &&
        write_full(fd, &entry.payload, sizeof(entry.payload)) &&
        write_full(fd, &entry.record_crc32, sizeof(entry.record_crc32)) &&
        durable_sync(fd) == 0;
    ::close(fd);
    return ok;
}

RecoveryReport scan_file(const std::string& path,
                         LogFileMetadata* metadata,
                         std::vector<ReplayEntry>* replay_entries,
                         bool truncate_invalid_tail) {
    RecoveryReport report;
    if (!path_exists(path)) {
        report.healthy = true;
        report.message = "Log file not present";
        return report;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        report.message = "Failed to open log file";
        return report;
    }

    PersistedFileHeader file_header {};
    if (!read_exact(input, &file_header, sizeof(file_header))) {
        report.message = "Detected missing or partial file header";
        return report;
    }

    if (file_header.magic != kLogFileMagic ||
        file_header.version != kLogFormatVersion ||
        file_header.header_size != sizeof(PersistedFileHeader) ||
        file_header.record_size != kPersistedRecordSize) {
        report.message = "Detected invalid file header metadata";
        return report;
    }

    if (compute_file_header_crc(file_header) != file_header.header_crc32) {
        report.message = "Detected corrupted file header CRC";
        return report;
    }

    if (metadata != nullptr) {
        metadata->format_version = file_header.version;
        metadata->recorder_start_time_us = file_header.recorder_start_time_us;
        metadata->record_size = file_header.record_size;
    }

    std::size_t valid_bytes = sizeof(file_header);
    std::uint64_t previous_sequence = 0;
    bool seen_record = false;

    while (true) {
        PersistedRecordHeader header {};
        const auto record_offset = valid_bytes;
        report.first_bad_record_index = report.valid_records + 1;
        report.corruption_offset = record_offset;

        if (!read_exact(input, &header, sizeof(header))) {
            if (input.eof() && input.gcount() == 0) {
                report.healthy = true;
                report.valid_bytes = valid_bytes;
                report.message = "Log validated successfully";
            } else {
                report.corruption_detected = true;
                report.valid_bytes = valid_bytes;
                report.message = "Detected partial record header at end of file";
            }
            break;
        }

        report.first_bad_record_sequence = header.sequence;

        if (header.magic != kLogRecordMagic ||
            header.version != kLogFormatVersion ||
            header.header_size != sizeof(PersistedRecordHeader) ||
            header.payload_size != sizeof(PersistedFlightPayload)) {
            report.corruption_detected = true;
            report.valid_bytes = valid_bytes;
            report.message = "Detected invalid record metadata";
            break;
        }

        PersistedFlightPayload payload {};
        std::uint32_t stored_crc = 0;
        if (!read_exact(input, &payload, sizeof(payload)) ||
            !read_exact(input, &stored_crc, sizeof(stored_crc))) {
            report.corruption_detected = true;
            report.valid_bytes = valid_bytes;
            report.message = "Detected truncated record payload";
            break;
        }

        const auto computed_crc = compute_record_crc(header, payload);
        if (stored_crc != computed_crc) {
            report.corruption_detected = true;
            report.valid_bytes = valid_bytes;
            ++report.checksum_failures;
            report.message = "Detected CRC mismatch";
            break;
        }

        if (seen_record && header.sequence <= previous_sequence) {
            report.corruption_detected = true;
            report.valid_bytes = valid_bytes;
            report.message = "Detected non-monotonic record sequence";
            break;
        }

        const FlightRecord record = make_runtime_record(header, payload);
        ++report.valid_records;
        report.last_sequence = header.sequence;
        previous_sequence = header.sequence;
        seen_record = true;
        valid_bytes = record_offset + sizeof(header) + sizeof(payload) + sizeof(stored_crc);

        if (replay_entries != nullptr) {
            replay_entries->push_back(ReplayEntry {header.sequence, record});
        }
    }

    if (!report.healthy &&
        truncate_invalid_tail &&
        report.valid_bytes >= sizeof(PersistedFileHeader) &&
        ::truncate(path.c_str(), static_cast<off_t>(report.valid_bytes)) == 0) {
        report.truncated = true;
        report.message += "; truncated invalid tail";
    }

    return report;
}

RecoveryReport scan_healthy_prefix(const std::string& path, bool truncate_invalid_tail) {
    RecoveryReport report = scan_file(path, nullptr, nullptr, truncate_invalid_tail);
    if (!report.healthy && report.truncated) {
        report = scan_file(path, nullptr, nullptr, false);
        report.truncated = true;
        report.message += "; log prefix recovered";
    }
    return report;
}

}  // namespace

std::string RecoveryManager::journal_path_for_log(const std::string& path) {
    return path + ".journal";
}

RecoveryReport RecoveryManager::validate(const std::string& path) const {
    return scan_file(path, nullptr, nullptr, false);
}

RecoveryReport RecoveryManager::recover(const std::string& path, bool truncate_invalid_tail) const {
    return scan_healthy_prefix(path, truncate_invalid_tail);
}

bool RecoveryManager::scan_replayable_log(const std::string& path,
                                          ReplayLog& replay_log,
                                          RecoveryReport& report) const {
    replay_log = ReplayLog {};
    report = scan_file(path, &replay_log.metadata, &replay_log.entries, false);
    return true;
}

StartupRecoveryReport RecoveryManager::recover_startup_state(const std::string& path) const {
    StartupRecoveryReport startup_report;
    const std::string journal_path = journal_path_for_log(path);

    RecoveryReport log_report = scan_healthy_prefix(path, true);
    startup_report.log_truncated = log_report.truncated;
    startup_report.valid_records = log_report.valid_records;
    startup_report.last_sequence = log_report.last_sequence;
    startup_report.checksum_failures = log_report.checksum_failures;

    const JournalInspection journal = inspect_journal(journal_path);
    startup_report.journal_found = (journal.status != JournalInspection::Status::Empty);

    if (journal.status == JournalInspection::Status::Empty) {
        startup_report.healthy = log_report.healthy;
        startup_report.message = log_report.message + "; " + journal.message;
        return startup_report;
    }

    if (journal.status == JournalInspection::Status::Corrupt) {
        startup_report.journal_discarded = clear_journal_file(journal_path);
        startup_report.healthy = log_report.healthy && startup_report.journal_discarded;
        startup_report.message = log_report.message + "; discarded corrupt journal: " + journal.message;
        return startup_report;
    }

    if (!log_report.healthy) {
        startup_report.healthy = false;
        startup_report.message = "main log is not recoverable before journal replay";
        return startup_report;
    }

    if (startup_report.last_sequence >= journal.entry.sequence) {
        startup_report.journal_cleared = clear_journal_file(journal_path);
        startup_report.healthy = startup_report.journal_cleared;
        startup_report.message = "journal described an already committed record; cleared stale journal";
        return startup_report;
    }

    if (startup_report.last_sequence + 1 != journal.entry.sequence) {
        startup_report.journal_discarded = clear_journal_file(journal_path);
        startup_report.healthy = startup_report.journal_discarded;
        startup_report.message = "journal sequence does not match log tail; discarded journal";
        return startup_report;
    }

    if (!append_recovered_record(path, journal.entry)) {
        startup_report.healthy = false;
        startup_report.message = "failed to roll journal entry forward into main log";
        return startup_report;
    }

    RecoveryReport replayed_log_report = scan_healthy_prefix(path, false);
    if (!replayed_log_report.healthy || replayed_log_report.last_sequence != journal.entry.sequence) {
        startup_report.healthy = false;
        startup_report.message = "journal replay did not produce a valid main log";
        return startup_report;
    }

    startup_report.journal_replayed = true;
    startup_report.journal_cleared = clear_journal_file(journal_path);
    startup_report.healthy = startup_report.journal_cleared;
    startup_report.valid_records = replayed_log_report.valid_records;
    startup_report.last_sequence = replayed_log_report.last_sequence;
    startup_report.checksum_failures = replayed_log_report.checksum_failures;
    startup_report.message = "rolled pending journal entry forward and cleared journal";
    return startup_report;
}

bool RecoveryManager::read_log(const std::string& path, ReplayLog& replay_log, std::string& error) const {
    RecoveryReport report;
    scan_replayable_log(path, replay_log, report);
    if (!report.healthy) {
        error = report.message;
        replay_log = ReplayLog {};
        return false;
    }

    error.clear();
    return true;
}

bool RecoveryManager::read_all(const std::string& path,
                               std::vector<ReplayEntry>& out_records,
                               std::string& error) const {
    ReplayLog replay_log;
    if (!read_log(path, replay_log, error)) {
        out_records.clear();
        return false;
    }

    out_records = std::move(replay_log.entries);
    return true;
}

}  // namespace flight_recorder
