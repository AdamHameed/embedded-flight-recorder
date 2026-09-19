#include "flight_recorder/flight_recorder.hpp"
#include "flight_recorder/recovery_manager.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

// A process signal may be delivered to any recorder thread. Only lock-free
// atomic operations are used in the handler; all I/O stays on the main thread.
static_assert(std::atomic<int>::is_always_lock_free);
std::atomic<int> shutdown_signal {0};

void request_shutdown(int signal) {
    shutdown_signal.store(signal, std::memory_order_relaxed);
}

std::uint32_t parse_unsigned(const std::string& value) {
    std::uint32_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc {} || result.ptr != value.data() + value.size()) {
        throw std::invalid_argument("expected an unsigned 32-bit integer");
    }
    return parsed;
}

void print_stats(const flight_recorder::RecorderStats& stats) {
    std::cout << "generated=" << stats.total_records_generated
              << " written=" << stats.total_records_written
              << " committed=" << stats.total_records_committed
              << " dropped=" << stats.dropped_records
              << " buffer_high_watermark=" << stats.buffer_high_watermark
              << " writer_error=" << (stats.writer_error ? "true" : "false")
              << std::endl;
}

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
        << "Usage: flight_recorder [--output path] [--duration-seconds N | --run-until-signal] "
        << "[--stats-interval-ms N] "
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
        << "SIGINT/SIGTERM stop acquisition, drain the queue, and commit pending records.\n"
        << "Stats interval: periodic live counters; 0 disables reporting (default).\n"
        << "Counters: generated=sensor samples; written=complete main-log records; "
           "committed=records covered by a durably acknowledged checkpoint; "
           "dropped=samples rejected when the bounded ring stays full.\n";
}

}  // namespace

int main(int argc, char** argv) {
    flight_recorder::RecorderConfig config;
    int duration_seconds = 5;
    bool duration_set = false;
    bool run_until_signal = false;
    std::uint32_t stats_interval_ms = 0;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--output" && i + 1 < argc) {
                config.output_path = argv[++i];
            } else if (arg == "--duration-seconds" && i + 1 < argc) {
                const std::string value = argv[++i];
                const auto result = std::from_chars(
                    value.data(), value.data() + value.size(), duration_seconds);
                if (result.ec != std::errc {} || result.ptr != value.data() + value.size()) {
                    throw std::invalid_argument("duration must be an integer");
                }
                duration_set = true;
            } else if (arg == "--run-until-signal") {
                run_until_signal = true;
            } else if (arg == "--stats-interval-ms" && i + 1 < argc) {
                stats_interval_ms = parse_unsigned(argv[++i]);
            } else if (arg == "--sample-rate-hz" && i + 1 < argc) {
                config.sample_rate_hz = static_cast<unsigned int>(std::stoul(argv[++i]));
            } else if (arg == "--buffer-size" && i + 1 < argc) {
                config.buffer_size = static_cast<std::size_t>(std::stoull(argv[++i]));
            } else if (arg == "--batch-size" && i + 1 < argc) {
                config.batch_size = static_cast<std::size_t>(std::stoull(argv[++i]));
            } else if (arg == "--sync-every-batches" && i + 1 < argc) {
                config.sync_every_batches = static_cast<std::size_t>(std::stoull(argv[++i]));
            } else if (arg == "--flush-interval-ms" && i + 1 < argc) {
                config.flush_interval_ms = parse_unsigned(argv[++i]);
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

    if (duration_set && run_until_signal) {
        std::cerr << "--duration-seconds and --run-until-signal are mutually exclusive\n";
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

    struct sigaction action {};
    action.sa_handler = request_shutdown;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) != 0 ||
        sigaction(SIGTERM, &action, nullptr) != 0) {
        std::cerr << "failed to install shutdown signal handlers\n";
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

    std::cout << "Recording to " << config.output_path
              << (run_until_signal ? " until SIGINT/SIGTERM" :
                  " for " + std::to_string(duration_seconds) + " seconds")
              << " seed=" << config.simulator_seed
              << std::endl;
    using Clock = std::chrono::steady_clock;
    const auto deadline = run_until_signal ? Clock::time_point::max() :
        Clock::now() + std::chrono::seconds(duration_seconds);
    const auto stats_interval = std::chrono::milliseconds(stats_interval_ms);
    auto next_stats = stats_interval_ms == 0 ? Clock::time_point::max() :
        Clock::now() + stats_interval;
    while (shutdown_signal.load(std::memory_order_relaxed) == 0 && Clock::now() < deadline) {
        const auto stats = recorder.stats();
        if (stats.writer_error) {
            break;
        }
        const auto now = Clock::now();
        if (now >= next_stats) {
            std::cout << "status ";
            print_stats(stats);
            next_stats = now + stats_interval;
        }
        std::this_thread::sleep_until(std::min({deadline, next_stats,
                                             now + std::chrono::milliseconds(20)}));
    }
    recorder.stop();

    const int received_signal = shutdown_signal.load(std::memory_order_relaxed);
    if (received_signal != 0) {
        std::cout << "shutdown_signal=" << received_signal << '\n';
    }

    const auto stats = recorder.stats();
    std::cout << "Recording complete\n";
    print_stats(stats);

    const auto recovery_report = recovery_manager.validate(config.output_path);
    std::cout << "replay_validation healthy=" << (recovery_report.healthy ? "true" : "false")
              << " checksum_failures=" << recovery_report.checksum_failures
              << " valid_records=" << recovery_report.valid_records
              << '\n';
    return !stats.writer_error && recovery_report.healthy ? 0 : 2;
}
