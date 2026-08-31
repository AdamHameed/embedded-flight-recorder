#include "flight_recorder/binary_log_writer.hpp"
#include "flight_recorder/circular_buffer.hpp"
#include "flight_recorder/platform_info.hpp"
#include "flight_recorder/recovery_manager.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct Site {
    const char* name;
    flight_recorder::RuntimeFaultMode mode;
    int expected_exit;
    bool acknowledges_new_batch;
};

constexpr std::array<Site, 6> kSites {{
    {"before_main_write", flight_recorder::RuntimeFaultMode::CrashBeforeMainLogWrite, 81, false},
    {"during_main_write", flight_recorder::RuntimeFaultMode::CrashDuringMainLogWrite, 92, false},
    {"after_main_write_before_sync", flight_recorder::RuntimeFaultMode::CrashAfterMainLogWrite, 83, false},
    {"after_main_sync_before_checkpoint", flight_recorder::RuntimeFaultMode::CrashAfterMainLogSync, 85, false},
    {"during_checkpoint_write", flight_recorder::RuntimeFaultMode::CrashDuringCheckpointWrite, 87, false},
    {"after_journal_sync", flight_recorder::RuntimeFaultMode::CrashAfterJournalSync, 90, true}
}};

struct RecoveryResult {
    std::uint64_t recovered_sequence {0};
    std::uint64_t recovered_records {0};
    std::uint64_t recovered_bytes {0};
    std::uint64_t first_generation {0};
    std::uint64_t second_generation {0};
    std::uint8_t first_healthy {0};
    std::uint8_t second_healthy {0};
    std::uint8_t contiguous {0};
    std::uint8_t exact_size {0};
    std::uint8_t idempotent {0};
};

struct CaseResult {
    std::size_t case_index {0};
    std::uint32_t seed {0};
    std::string site;
    std::size_t batch_size {0};
    std::size_t partial_cut {0};
    std::size_t ring_capacity {0};
    std::size_t ring_wrap_cycles {0};
    std::uint64_t preexisting_sequence {0};
    std::uint64_t expected_sequence {0};
    std::uint64_t acknowledged_sequence {0};
    RecoveryResult recovery;
    int child_exit {-1};
    bool passed {false};
};

flight_recorder::FlightRecord make_record(std::uint64_t sequence) {
    flight_recorder::FlightRecord record;
    record.timestamp_us = 1'000'000 + sequence;
    record.altitude_m = static_cast<double>(sequence);
    record.airspeed_kts = 120.0;
    return record;
}

bool write_all(int fd, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t written = 0;
    while (written < length) {
        const auto result = ::write(fd, bytes + written, length - written);
        if (result <= 0) return false;
        written += static_cast<std::size_t>(result);
    }
    return true;
}

bool read_all(int fd, void* data, std::size_t length) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t read_bytes = 0;
    while (read_bytes < length) {
        const auto result = ::read(fd, bytes + read_bytes, length - read_bytes);
        if (result <= 0) return false;
        read_bytes += static_cast<std::size_t>(result);
    }
    return true;
}

std::vector<flight_recorder::SequencedRecord> make_records(std::uint64_t first,
                                                            std::size_t count) {
    std::vector<flight_recorder::SequencedRecord> records;
    records.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto sequence = first + index;
        records.push_back({make_record(sequence), sequence});
    }
    return records;
}

bool initialize_committed_log(const std::string& path, std::uint64_t record_count) {
    flight_recorder::BinaryLogWriter writer(
        path, {}, flight_recorder::WriterOptions {static_cast<std::size_t>(record_count), 1, 4096});
    return writer.open() && writer.append_batch(make_records(1, record_count)) && writer.flush();
}

std::vector<flight_recorder::SequencedRecord> make_wrapped_batch(std::uint64_t first,
                                                                 std::size_t count,
                                                                 std::size_t capacity,
                                                                 std::size_t wrap_cycles) {
    flight_recorder::CircularBuffer ring(capacity);
    flight_recorder::FlightRecord temporary;
    for (std::size_t index = 0; index < capacity * wrap_cycles; ++index) {
        temporary.timestamp_us = index;
        if (ring.try_push(temporary) != flight_recorder::CircularBuffer::PushStatus::Pushed ||
            !ring.try_pop(temporary)) {
            return {};
        }
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (ring.try_push(make_record(first + index)) !=
            flight_recorder::CircularBuffer::PushStatus::Pushed) {
            return {};
        }
    }
    auto drained = std::make_unique<flight_recorder::FlightRecord[]>(count);
    if (ring.try_pop_batch(drained.get(), count) != count) {
        return {};
    }
    std::vector<flight_recorder::SequencedRecord> records;
    records.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        records.push_back({drained[index], first + index});
    }
    return records;
}

RecoveryResult recover_in_fresh_process(const std::string& path) {
    int pipe_fds[2] {-1, -1};
    RecoveryResult result;
    if (::pipe(pipe_fds) != 0) return result;
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(pipe_fds[0]);
        flight_recorder::RecoveryManager manager;
        const auto first = manager.recover_startup_state(path);
        const auto second = manager.recover_startup_state(path);
        flight_recorder::ReplayLog replay;
        flight_recorder::RecoveryReport scan;
        manager.scan_replayable_log(path, replay, scan);
        RecoveryResult child_result;
        child_result.first_healthy = first.healthy;
        child_result.second_healthy = second.healthy;
        child_result.recovered_sequence = second.last_sequence;
        child_result.recovered_records = replay.entries.size();
        child_result.recovered_bytes = scan.valid_bytes;
        child_result.first_generation = first.checkpoint_generation;
        child_result.second_generation = second.checkpoint_generation;
        child_result.contiguous = scan.healthy;
        for (std::size_t index = 0; index < replay.entries.size(); ++index) {
            child_result.contiguous = child_result.contiguous &&
                                      replay.entries[index].sequence == index + 1;
        }
        child_result.exact_size = scan.valid_bytes ==
            sizeof(flight_recorder::PersistedFileHeader) +
                replay.entries.size() * flight_recorder::kPersistedRecordSize;
        child_result.idempotent = first.healthy && second.healthy &&
            first.last_sequence == second.last_sequence &&
            first.valid_records == second.valid_records &&
            first.checkpoint_generation == second.checkpoint_generation &&
            !second.log_truncated;
        (void)write_all(pipe_fds[1], &child_result, sizeof(child_result));
        ::close(pipe_fds[1]);
        ::_exit(0);
    }
    ::close(pipe_fds[1]);
    (void)read_all(pipe_fds[0], &result, sizeof(result));
    ::close(pipe_fds[0]);
    int status = 0;
    (void)::waitpid(child, &status, 0);
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: crash_matrix <artifact.json>\n";
        return 1;
    }
    const std::filesystem::path artifact_path = argv[1];
    const auto artifact_parent = artifact_path.parent_path().empty()
        ? std::filesystem::path(".") : artifact_path.parent_path();
    std::error_code filesystem_error;
    std::filesystem::create_directories(artifact_parent, filesystem_error);
    if (filesystem_error) {
        std::cerr << "failed to create artifact directory: " << filesystem_error.message() << '\n';
        return 1;
    }
    const auto work_dir = artifact_parent /
        (artifact_path.stem().string() + "-work-" + std::to_string(::getpid()));
    std::filesystem::create_directories(work_dir, filesystem_error);
    if (filesystem_error) {
        std::cerr << "failed to create crash-matrix workspace: "
                  << filesystem_error.message() << '\n';
        return 1;
    }

    std::vector<CaseResult> results;
    results.reserve(180);
    std::size_t case_index = 0;
    for (const auto& site : kSites) {
        for (std::size_t iteration = 0; iteration < 30; ++iteration) {
            ++case_index;
            CaseResult result;
            result.case_index = case_index;
            result.seed = static_cast<std::uint32_t>(0xC0FFEEu + case_index * 7919u);
            result.site = site.name;
            std::mt19937 random(result.seed);
            result.preexisting_sequence = 1u + (random() % 23u);
            result.batch_size = 1u + (random() % 8u);
            result.ring_capacity = result.batch_size + 1u + (random() % 5u);
            result.ring_wrap_cycles = 1u + (random() % 4u);
            const auto batch_bytes = result.batch_size * flight_recorder::kPersistedRecordSize;
            result.partial_cut = std::string(site.name) == "during_main_write"
                ? 1u + (random() % (batch_bytes - 1u))
                : (std::string(site.name) == "during_checkpoint_write"
                    ? 1u + (random() % (sizeof(flight_recorder::PersistedCheckpointSlot) - 1u)) : 0u);
            result.expected_sequence = site.acknowledges_new_batch
                ? result.preexisting_sequence + result.batch_size : result.preexisting_sequence;

            const auto case_prefix = work_dir / ("case-" + std::to_string(case_index));
            const std::string log_path = case_prefix.string() + ".bin";
            const std::string ack_path = case_prefix.string() + ".ack";
            if (!initialize_committed_log(log_path, result.preexisting_sequence)) {
                results.push_back(result);
                continue;
            }
            const int ack_fd = ::open(ack_path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
            if (ack_fd < 0 ||
                ::pwrite(ack_fd, &result.preexisting_sequence,
                         sizeof(result.preexisting_sequence), 0) !=
                    static_cast<ssize_t>(sizeof(result.preexisting_sequence)) ||
                ::fsync(ack_fd) != 0) {
                if (ack_fd >= 0) ::close(ack_fd);
                results.push_back(result);
                continue;
            }

            const pid_t child = ::fork();
            if (child == 0) {
                const auto records = make_wrapped_batch(
                    result.preexisting_sequence + 1,
                    result.batch_size,
                    result.ring_capacity,
                    result.ring_wrap_cycles);
                flight_recorder::RuntimeFaultConfig fault {
                    site.mode,
                    result.preexisting_sequence + result.batch_size,
                    result.partial_cut,
                    ack_fd
                };
                flight_recorder::BinaryLogWriter writer(
                    log_path, fault,
                    flight_recorder::WriterOptions {result.batch_size, 1, 4096});
                if (!writer.open() || records.size() != result.batch_size) ::_exit(71);
                (void)writer.append_batch(records);
                ::_exit(72);
            }
            int child_status = 0;
            (void)::waitpid(child, &child_status, 0);
            result.child_exit = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1;
            if (::pread(ack_fd, &result.acknowledged_sequence,
                        sizeof(result.acknowledged_sequence), 0) !=
                static_cast<ssize_t>(sizeof(result.acknowledged_sequence))) {
                result.acknowledged_sequence = 0;
            }
            ::close(ack_fd);
            result.recovery = recover_in_fresh_process(log_path);
            result.passed = result.child_exit == site.expected_exit &&
                result.acknowledged_sequence == result.expected_sequence &&
                result.recovery.first_healthy && result.recovery.second_healthy &&
                result.recovery.recovered_sequence == result.expected_sequence &&
                result.recovery.recovered_records == result.expected_sequence &&
                result.recovery.contiguous && result.recovery.exact_size &&
                result.recovery.idempotent;
            results.push_back(result);
        }
    }

    std::size_t passed = 0;
    for (const auto& result : results) passed += result.passed ? 1u : 0u;
    std::ofstream artifact(artifact_path, std::ios::trunc);
    if (!artifact) {
        std::cerr << "failed to open artifact file: " << artifact_path << '\n';
        return 1;
    }
    artifact << "{\n  \"crash_cases\": " << results.size()
             << ",\n  \"passed\": " << passed
             << ",\n  \"failed\": " << (results.size() - passed)
             << ",\n  \"platform\": "
             << flight_recorder::platform_info_json(flight_recorder::collect_platform_info("."))
             << ",\n  \"cases\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& item = results[index];
        artifact << "    {\"case\":" << item.case_index
                 << ",\"seed\":" << item.seed
                 << ",\"crash_site\":\"" << item.site << "\""
                 << ",\"batch_size\":" << item.batch_size
                 << ",\"partial_cut\":" << item.partial_cut
                 << ",\"ring_capacity\":" << item.ring_capacity
                 << ",\"ring_wrap_cycles\":" << item.ring_wrap_cycles
                 << ",\"preexisting_sequence\":" << item.preexisting_sequence
                 << ",\"expected_committed_sequence\":" << item.expected_sequence
                 << ",\"acknowledged_sequence\":" << item.acknowledged_sequence
                 << ",\"recovered_sequence\":" << item.recovery.recovered_sequence
                 << ",\"recovered_records\":" << item.recovery.recovered_records
                 << ",\"child_exit\":" << item.child_exit
                 << ",\"healthy\":" << (item.recovery.first_healthy ? "true" : "false")
                 << ",\"contiguous\":" << (item.recovery.contiguous ? "true" : "false")
                 << ",\"exact_size\":" << (item.recovery.exact_size ? "true" : "false")
                 << ",\"idempotent\":" << (item.recovery.idempotent ? "true" : "false")
                 << ",\"passed\":" << (item.passed ? "true" : "false") << '}';
        artifact << (index + 1 == results.size() ? "\n" : ",\n");
    }
    artifact << "  ]\n}\n";
    artifact.close();

    std::cout << "crash_cases=" << results.size()
              << " passed=" << passed
              << " failed=" << (results.size() - passed)
              << " artifact=" << artifact_path.string() << '\n';
    return results.size() == 180 && passed == 180 ? 0 : 2;
}
