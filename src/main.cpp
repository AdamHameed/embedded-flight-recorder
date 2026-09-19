#include "flight_recorder/flight_recorder.hpp"
#include "flight_recorder/recovery_manager.hpp"

#include <chrono>
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

flight_recorder::RuntimeFaultMode parse_fault_mode(const std::string& value) {
    if (value == "none") {
        return flight_recorder::RuntimeFaultMode::None;
    }
    if (value == "crash-after-journal") {
        return flight_recorder::RuntimeFaultMode::CrashAfterJournalSync;
    }
    if (value == "crash-before-write") {
        return flight_recorder::RuntimeFaultMode::CrashBeforeMainLogWrite;
    }
    if (value == "crash-during-write") {
        return flight_recorder::RuntimeFaultMode::CrashDuringMainLogWrite;
    }
    if (value == "crash-after-write") {
        return flight_recorder::RuntimeFaultMode::CrashAfterMainLogWrite;
    }
    if (value == "crash-before-data-sync") {
        return flight_recorder::RuntimeFaultMode::CrashBeforeMainLogSync;
    }
    if (value == "crash-after-data-sync") {
        return flight_recorder::RuntimeFaultMode::CrashAfterMainLogSync;
    }
    if (value == "crash-before-checkpoint") {
        return flight_recorder::RuntimeFaultMode::CrashBeforeCheckpointWrite;
    }
    if (value == "crash-during-checkpoint") {
        return flight_recorder::RuntimeFaultMode::CrashDuringCheckpointWrite;
    }
    if (value == "crash-after-checkpoint") {
        return flight_recorder::RuntimeFaultMode::CrashAfterCheckpointWrite;
    }
    if (value == "crash-before-journal-sync") {
        return flight_recorder::RuntimeFaultMode::CrashBeforeJournalSync;
    }
    if (value == "drop-commit") {
        return flight_recorder::RuntimeFaultMode::DropCommit;
    }
    throw std::invalid_argument("unknown fault mode");
}

void print_usage() {
    std::cout
        << "Usage: flight_recorder [--output path] [--duration-seconds N] "
        << "[--sample-rate-hz N] [--buffer-size N] "
        << "[--batch-size N] [--sync-every-batches N] [--flush-interval-ms N] "
        << "[--seed N] "
        << "[--unpaced] "
        << "[--fault none|crash-before-write|crash-during-write|crash-after-write|"
           "crash-before-data-sync|crash-after-data-sync|crash-before-checkpoint|"
           "crash-during-checkpoint|crash-after-checkpoint|crash-before-journal-sync|"
           "crash-after-journal|drop-commit] "
        << "[--fault-sequence N]\n"
        << "Flush interval: milliseconds before committing a pending group; 0 disables the timer.\n"
        << "Counters: generated=sensor samples; written=complete main-log records; "
           "committed=records covered by a durably acknowledged checkpoint; "
           "dropped=records evicted from the full bounded ring.\n";
}

}  // namespace

int main(int argc, char** argv) {
    flight_recorder::RecorderConfig config;
    int duration_seconds = 5;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--output" && i + 1 < argc) {
                config.output_path = argv[++i];
            } else if (arg == "--duration-seconds" && i + 1 < argc) {
                duration_seconds = std::stoi(argv[++i]);
            } else if (arg == "--sample-rate-hz" && i + 1 < argc) {
                config.sample_rate_hz = static_cast<unsigned int>(std::stoul(argv[++i]));
            } else if (arg == "--buffer-size" && i + 1 < argc) {
                config.buffer_size = static_cast<std::size_t>(std::stoull(argv[++i]));
            } else if (arg == "--batch-size" && i + 1 < argc) {
                config.batch_size = static_cast<std::size_t>(std::stoull(argv[++i]));
            } else if (arg == "--sync-every-batches" && i + 1 < argc) {
                config.sync_every_batches = static_cast<std::size_t>(std::stoull(argv[++i]));
            } else if (arg == "--flush-interval-ms" && i + 1 < argc) {
                const std::string value = argv[++i];
                const auto result = std::from_chars(
                    value.data(), value.data() + value.size(), config.flush_interval_ms);
                if (result.ec != std::errc {} || result.ptr != value.data() + value.size()) {
                    throw std::invalid_argument("flush interval must be an unsigned 32-bit integer");
                }
            } else if (arg == "--seed" && i + 1 < argc) {
                config.simulator_seed = static_cast<std::uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--unpaced") {
                config.unpaced_producer = true;
            } else if (arg == "--fault" && i + 1 < argc) {
                config.fault_config.mode = parse_fault_mode(argv[++i]);
            } else if (arg == "--fault-sequence" && i + 1 < argc) {
                config.fault_config.trigger_sequence =
                    static_cast<std::uint64_t>(std::stoull(argv[++i]));
            } else if (arg == "--help") {
                print_usage();
                return 0;
            } else {
                print_usage();
                return 1;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "invalid argument: " << error.what() << '\n';
        print_usage();
        return 1;
    }

    if (duration_seconds < 0) {
        std::cerr << "duration must not be negative\n";
        return 1;
    }

    if (config.sample_rate_hz == 0 || config.buffer_size == 0 || config.batch_size == 0 ||
        config.sync_every_batches == 0) {
        std::cerr << "sample rate, buffer size, batch size, and sync group must be greater than zero\n";
        return 1;
    }

    flight_recorder::RecoveryManager recovery_manager;
    const auto startup_recovery = recovery_manager.recover_startup_state(config.output_path);
    std::cout << "startup_recovery healthy=" << (startup_recovery.healthy ? "true" : "false")
              << " log_truncated=" << (startup_recovery.log_truncated ? "true" : "false")
              << " journal_found=" << (startup_recovery.journal_found ? "true" : "false")
              << " journal_replayed=" << (startup_recovery.journal_replayed ? "true" : "false")
              << " journal_cleared=" << (startup_recovery.journal_cleared ? "true" : "false")
              << " last_sequence=" << startup_recovery.last_sequence
              << '\n';
    std::cout << startup_recovery.message << '\n';
    if (!startup_recovery.healthy) {
        std::cerr << "startup recovery failed\n";
        return 1;
    }

    flight_recorder::FlightRecorder recorder(config);
    recorder.set_start_sequence(startup_recovery.last_sequence);
    if (!recorder.start()) {
        std::cerr << "failed to start recorder\n";
        return 1;
    }

    std::cout << "Recording to " << config.output_path << " for "
              << duration_seconds << " seconds"
              << " seed=" << config.simulator_seed
              << '\n';
    std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
    recorder.stop();

    const auto stats = recorder.stats();
    std::cout << "Recording complete\n";
    std::cout << "generated=" << stats.total_records_generated
              << " written=" << stats.total_records_written
              << " committed=" << stats.total_records_committed
              << " dropped=" << stats.dropped_records
              << " buffer_high_watermark=" << stats.buffer_high_watermark
              << " writer_error=" << (stats.writer_error ? "true" : "false")
              << '\n';

    const auto recovery_report = recovery_manager.validate(config.output_path);
    std::cout << "replay_validation healthy=" << (recovery_report.healthy ? "true" : "false")
              << " checksum_failures=" << recovery_report.checksum_failures
              << " valid_records=" << recovery_report.valid_records
              << '\n';
    return !stats.writer_error && recovery_report.healthy ? 0 : 2;
}
