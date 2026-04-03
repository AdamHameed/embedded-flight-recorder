#include "flight_recorder/flight_recorder.hpp"
#include "flight_recorder/recovery_manager.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

void print_usage() {
    std::cout
        << "Usage: flight_recorder [--output path] [--duration-seconds N] "
        << "[--sample-rate-hz N] [--buffer-size N]\n";
}

}  // namespace

int main(int argc, char** argv) {
    flight_recorder::RecorderConfig config;
    int duration_seconds = 5;

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
        } else if (arg == "--help") {
            print_usage();
            return 0;
        } else {
            print_usage();
            return 1;
        }
    }

    if (config.sample_rate_hz == 0 || config.buffer_size == 0) {
        std::cerr << "sample rate and buffer size must be greater than zero\n";
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
              << duration_seconds << " seconds\n";
    std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
    recorder.stop();

    const auto stats = recorder.stats();
    std::cout << "Recording complete\n";
    std::cout << "generated=" << stats.total_records_generated
              << " written=" << stats.total_records_written
              << " dropped=" << stats.dropped_records
              << " buffer_high_watermark=" << stats.buffer_high_watermark
              << " writer_error=" << (stats.writer_error ? "true" : "false")
              << '\n';

    const auto recovery_report = recovery_manager.validate(config.output_path);
    std::cout << "replay_validation healthy=" << (recovery_report.healthy ? "true" : "false")
              << " checksum_failures=" << recovery_report.checksum_failures
              << " valid_records=" << recovery_report.valid_records
              << '\n';
    return 0;
}
