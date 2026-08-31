#include "flight_recorder/flight_recorder.hpp"
#include "flight_recorder/platform_info.hpp"
#include "flight_recorder/recovery_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

namespace {

struct Options {
    std::string output_path;
    std::string artifact_path;
    unsigned int warmup_seconds {2};
    unsigned int duration_seconds {30};
    std::size_t buffer_size {262'144};
    std::size_t batch_size {1024};
    std::size_t sync_every_batches {64};
    std::size_t preallocation_bytes {std::size_t {256} * 1024u * 1024u};
    std::size_t alignment {4096};
    std::uint32_t seed {42};
};

bool exists(const std::string& path) {
    struct stat details {};
    return ::stat(path.c_str(), &details) == 0;
}

bool parse_size(const char* text, std::size_t& value) {
    try {
        value = std::stoull(text);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_args(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (index + 1 >= argc) return false;
        const char* value = argv[++index];
        std::size_t parsed = 0;
        if (arg == "--output") options.output_path = value;
        else if (arg == "--artifact") options.artifact_path = value;
        else if (arg == "--warmup-seconds" && parse_size(value, parsed)) options.warmup_seconds = parsed;
        else if (arg == "--duration-seconds" && parse_size(value, parsed)) options.duration_seconds = parsed;
        else if (arg == "--buffer-size" && parse_size(value, parsed)) options.buffer_size = parsed;
        else if (arg == "--batch-size" && parse_size(value, parsed)) options.batch_size = parsed;
        else if (arg == "--sync-every-batches" && parse_size(value, parsed)) options.sync_every_batches = parsed;
        else if (arg == "--preallocation-bytes" && parse_size(value, parsed)) options.preallocation_bytes = parsed;
        else if (arg == "--alignment" && parse_size(value, parsed)) options.alignment = parsed;
        else if (arg == "--seed" && parse_size(value, parsed)) options.seed = static_cast<std::uint32_t>(parsed);
        else return false;
    }
    return !options.output_path.empty() && !options.artifact_path.empty() &&
           options.warmup_seconds > 0 && options.duration_seconds > 0 &&
           options.buffer_size > 0 && options.batch_size > 0 &&
           options.sync_every_batches > 0 && options.alignment >= sizeof(void*);
}

flight_recorder::RecorderConfig make_config(const Options& options, const std::string& path) {
    flight_recorder::RecorderConfig config;
    config.output_path = path;
    config.buffer_size = options.buffer_size;
    config.batch_size = options.batch_size;
    config.sync_every_batches = options.sync_every_batches;
    config.preallocation_chunk_bytes = options.preallocation_bytes;
    config.serialization_buffer_alignment = options.alignment;
    config.sample_rate_hz = 1;
    config.unpaced_producer = true;
    config.simulator_seed = options.seed;
    return config;
}

void write_latency_json(std::ostream& output,
                        const flight_recorder::LatencySummary& latency) {
    output << "{\"samples\":" << latency.samples
           << ",\"p50_ns\":" << latency.p50_ns
           << ",\"p95_ns\":" << latency.p95_ns
           << ",\"p99_ns\":" << latency.p99_ns
           << ",\"max_ns\":" << latency.max_ns << '}';
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        std::cerr << "Usage: recorder_benchmark --output PATH --artifact PATH "
                     "[--warmup-seconds N] [--duration-seconds N] [--buffer-size N] "
                     "[--batch-size N] [--sync-every-batches N] "
                     "[--preallocation-bytes N] [--alignment N] [--seed N]\n";
        return 1;
    }
    const auto output_parent = std::filesystem::path(options.output_path).parent_path();
    const auto artifact_parent = std::filesystem::path(options.artifact_path).parent_path();
    std::error_code filesystem_error;
    if (!output_parent.empty()) {
        std::filesystem::create_directories(output_parent, filesystem_error);
    }
    if (!filesystem_error && !artifact_parent.empty()) {
        std::filesystem::create_directories(artifact_parent, filesystem_error);
    }
    if (filesystem_error) {
        std::cerr << "failed to create benchmark directory: "
                  << filesystem_error.message() << '\n';
        return 1;
    }
    const auto platform = flight_recorder::collect_platform_info(
        output_parent.empty() ? "." : output_parent.string());
    if (platform.build_type != "Release") {
        std::cerr << "recorder_benchmark requires a Release build\n";
        return 1;
    }
    const std::string warmup_path = options.output_path + ".warmup";
    if (exists(options.output_path) || exists(options.output_path + ".journal") ||
        exists(warmup_path) || exists(warmup_path + ".journal")) {
        std::cerr << "benchmark output paths must not already exist\n";
        return 1;
    }
    auto warmup_config = make_config(options, warmup_path);
    flight_recorder::FlightRecorder warmup(warmup_config);
    if (!warmup.start()) {
        std::cerr << "failed to start warmup recorder\n";
        return 2;
    }
    std::this_thread::sleep_for(std::chrono::seconds(options.warmup_seconds));
    warmup.stop();
    const auto warmup_stats = warmup.stats();
    flight_recorder::RecoveryManager recovery_manager;
    const auto warmup_replay = recovery_manager.validate(warmup_path);
    if (!warmup_replay.healthy || warmup_stats.writer_error || warmup_stats.dropped_records != 0 ||
        warmup_stats.total_records_written != warmup_stats.total_records_committed) {
        std::cerr << "warmup correctness validation failed\n";
        return 2;
    }

    auto measured_config = make_config(options, options.output_path);
    flight_recorder::FlightRecorder recorder(measured_config);
    if (!recorder.start()) {
        std::cerr << "failed to start measured recorder\n";
        return 2;
    }
    std::vector<std::uint64_t> window_rates;
    window_rates.reserve(options.duration_seconds);
    const auto start = std::chrono::steady_clock::now();
    auto prior_time = start;
    std::uint64_t prior_written = 0;
    for (unsigned int second = 1; second <= options.duration_seconds; ++second) {
        std::this_thread::sleep_until(start + std::chrono::seconds(second));
        const auto now = std::chrono::steady_clock::now();
        const auto snapshot = recorder.stats();
        const double window_seconds = std::chrono::duration<double>(now - prior_time).count();
        window_rates.push_back(static_cast<std::uint64_t>(
            static_cast<double>(snapshot.total_records_written - prior_written) /
            window_seconds));
        prior_written = snapshot.total_records_written;
        prior_time = now;
    }
    recorder.stop();
    const auto end = std::chrono::steady_clock::now();
    const double elapsed_seconds = std::chrono::duration<double>(end - start).count();
    const auto recorder_stats = recorder.stats();
    const auto writer_stats = recorder.writer_stats();
    const auto replay = recovery_manager.validate(options.output_path);
    struct stat file_stat {};
    const bool stat_ok = ::stat(options.output_path.c_str(), &file_stat) == 0;
    const std::uint64_t expected_bytes = sizeof(flight_recorder::PersistedFileHeader) +
        recorder_stats.total_records_written * flight_recorder::kPersistedRecordSize;
    const bool exact_size = stat_ok && static_cast<std::uint64_t>(file_stat.st_size) == expected_bytes;
    const bool valid = replay.healthy && exact_size && !recorder_stats.writer_error &&
        recorder_stats.dropped_records == 0 &&
        recorder_stats.total_records_generated == recorder_stats.total_records_written &&
        recorder_stats.total_records_written == recorder_stats.total_records_committed &&
        replay.valid_records == recorder_stats.total_records_written &&
        writer_stats.failures == 0;
    const auto throughput = static_cast<std::uint64_t>(
        static_cast<double>(recorder_stats.total_records_written) / elapsed_seconds);
    const auto min_window = window_rates.empty() ? 0 :
        *std::min_element(window_rates.begin(), window_rates.end());
    const auto write_latency = flight_recorder::summarize_write_latency(writer_stats);
    const auto sync_latency = flight_recorder::summarize_sync_latency(writer_stats);

    std::ofstream artifact(options.artifact_path, std::ios::trunc);
    if (!artifact) {
        std::cerr << "failed to open benchmark artifact: " << options.artifact_path << '\n';
        return 1;
    }
    artifact << "{\n  \"benchmark_kind\": \"end_to_end_recorder\",\n"
             << "  \"metric_contract\": {\"throughput\":\"produced_consumed_serialized_written_per_steady_second\","
                "\"write_latency\":\"steady_clock_around_each_real_main_pwrite_syscall\","
                "\"sync_latency\":\"steady_clock_around_each_main_data_sync\"},\n"
             << "  \"platform\": " << flight_recorder::platform_info_json(platform) << ",\n"
             << "  \"configuration\": {\"warmup_seconds\":" << options.warmup_seconds
             << ",\"duration_seconds\":" << options.duration_seconds
             << ",\"buffer_size\":" << options.buffer_size
             << ",\"batch_size\":" << options.batch_size
             << ",\"sync_every_batches\":" << options.sync_every_batches
             << ",\"preallocation_bytes\":" << options.preallocation_bytes
             << ",\"alignment\":" << options.alignment
             << ",\"seed\":" << options.seed << "},\n"
             << "  \"warmup\": {\"written\":" << warmup_stats.total_records_written
             << ",\"committed\":" << warmup_stats.total_records_committed
             << ",\"dropped\":" << warmup_stats.dropped_records
             << ",\"replay_valid\":" << (warmup_replay.healthy ? "true" : "false") << "},\n"
             << "  \"elapsed_seconds\": " << elapsed_seconds << ",\n"
             << "  \"records_generated\": " << recorder_stats.total_records_generated << ",\n"
             << "  \"records_written\": " << recorder_stats.total_records_written << ",\n"
             << "  \"records_committed\": " << recorder_stats.total_records_committed << ",\n"
             << "  \"dropped_records\": " << recorder_stats.dropped_records << ",\n"
             << "  \"writer_error\": " << (recorder_stats.writer_error ? "true" : "false") << ",\n"
             << "  \"records_per_second\": " << throughput << ",\n"
             << "  \"one_second_window_min_records_per_second\": " << min_window << ",\n"
             << "  \"one_second_window_records_per_second\": [";
    for (std::size_t index = 0; index < window_rates.size(); ++index) {
        artifact << (index == 0 ? "" : ",") << window_rates[index];
    }
    artifact << "],\n  \"writer_counters\": {\"records\":" << writer_stats.records_written
             << ",\"batches\":" << writer_stats.batches_written
             << ",\"bytes\":" << writer_stats.bytes_written
             << ",\"write_calls\":" << writer_stats.write_calls
             << ",\"sync_calls\":" << writer_stats.sync_calls
             << ",\"checkpoint_writes\":" << writer_stats.checkpoint_writes
             << ",\"journal_sync_calls\":" << writer_stats.journal_sync_calls
             << ",\"preallocation_calls\":" << writer_stats.preallocation_calls
             << ",\"failures\":" << writer_stats.failures << "},\n"
             << "  \"batch_write_latency\": ";
    write_latency_json(artifact, write_latency);
    artifact << ",\n  \"main_sync_latency\": ";
    write_latency_json(artifact, sync_latency);
    artifact << ",\n  \"replay\": {\"valid\":" << (replay.healthy ? "true" : "false")
             << ",\"records\":" << replay.valid_records
             << ",\"checksum_failures\":" << replay.checksum_failures
             << ",\"valid_bytes\":" << replay.valid_bytes
             << ",\"exact_file_size\":" << (exact_size ? "true" : "false") << "},\n"
             << "  \"valid_run\": " << (valid ? "true" : "false") << "\n}\n";
    artifact.close();

    std::cout << "benchmark_complete records=" << recorder_stats.total_records_written
              << " elapsed_seconds=" << elapsed_seconds
              << " records_per_second=" << throughput
              << " one_second_min=" << min_window
              << " dropped=" << recorder_stats.dropped_records
              << " replay_valid=" << (replay.healthy ? "true" : "false")
              << " write_p99_ns=" << write_latency.p99_ns
              << " sync_p99_ns=" << sync_latency.p99_ns
              << " artifact=" << options.artifact_path << '\n';
    return valid ? 0 : 2;
}
