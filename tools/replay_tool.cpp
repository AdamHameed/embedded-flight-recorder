#include "flight_recorder/recovery_manager.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string log_path;
    bool summary_only {false};
    bool show_invalid {false};
    std::uint64_t from_seq {0};
    std::uint64_t to_seq {std::numeric_limits<std::uint64_t>::max()};
    std::size_t max_records {std::numeric_limits<std::size_t>::max()};
    std::string csv_path;
};

struct SummaryStats {
    std::size_t total_valid_records {0};
    std::size_t invalid_records {0};
    std::uint64_t first_timestamp_us {0};
    std::uint64_t last_timestamp_us {0};
    double min_altitude_m {0.0};
    double max_altitude_m {0.0};
    double min_airspeed_kts {0.0};
    double max_airspeed_kts {0.0};
    double min_engine_temperature_c {0.0};
    double max_engine_temperature_c {0.0};
};

void print_usage() {
    std::cout
        << "Usage: replay_tool <log_file> [--summary] [--from-seq N] [--to-seq N] "
        << "[--max N] [--show-invalid] [--csv output.csv]\n";
}

std::string status_to_string(std::uint32_t status) {
    std::ostringstream output;
    output << "0x" << std::hex << status << std::dec;
    return output.str();
}

bool parse_unsigned_64(const std::string& text, std::uint64_t& value) {
    try {
        value = std::stoull(text);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_size(const std::string& text, std::size_t& value) {
    try {
        value = static_cast<std::size_t>(std::stoull(text));
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_args(int argc, char** argv, Options& options) {
    if (argc < 2) {
        return false;
    }

    options.log_path = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--summary") {
            options.summary_only = true;
        } else if (arg == "--show-invalid") {
            options.show_invalid = true;
        } else if (arg == "--from-seq" && i + 1 < argc) {
            if (!parse_unsigned_64(argv[++i], options.from_seq)) {
                return false;
            }
        } else if (arg == "--to-seq" && i + 1 < argc) {
            if (!parse_unsigned_64(argv[++i], options.to_seq)) {
                return false;
            }
        } else if (arg == "--max" && i + 1 < argc) {
            if (!parse_size(argv[++i], options.max_records)) {
                return false;
            }
        } else if (arg == "--csv" && i + 1 < argc) {
            options.csv_path = argv[++i];
        } else {
            return false;
        }
    }

    return options.from_seq <= options.to_seq;
}

bool matches_filter(const flight_recorder::ReplayEntry& entry, const Options& options) {
    return entry.sequence >= options.from_seq && entry.sequence <= options.to_seq;
}

std::vector<flight_recorder::ReplayEntry> filter_entries(const std::vector<flight_recorder::ReplayEntry>& entries,
                                                         const Options& options) {
    std::vector<flight_recorder::ReplayEntry> filtered;
    filtered.reserve(entries.size());

    for (const auto& entry : entries) {
        if (!matches_filter(entry, options)) {
            continue;
        }
        filtered.push_back(entry);
        if (filtered.size() >= options.max_records) {
            break;
        }
    }

    return filtered;
}

SummaryStats compute_summary(const std::vector<flight_recorder::ReplayEntry>& entries,
                             const flight_recorder::RecoveryReport& report) {
    SummaryStats stats;
    stats.total_valid_records = entries.size();
    stats.invalid_records = report.corruption_detected ? 1u : 0u;

    if (entries.empty()) {
        return stats;
    }

    const auto& first_record = entries.front().record;
    stats.first_timestamp_us = first_record.timestamp_us;
    stats.last_timestamp_us = entries.back().record.timestamp_us;
    stats.min_altitude_m = first_record.altitude_m;
    stats.max_altitude_m = first_record.altitude_m;
    stats.min_airspeed_kts = first_record.airspeed_kts;
    stats.max_airspeed_kts = first_record.airspeed_kts;
    stats.min_engine_temperature_c = first_record.engine_temperature_c;
    stats.max_engine_temperature_c = first_record.engine_temperature_c;

    for (const auto& entry : entries) {
        const auto& record = entry.record;
        stats.min_altitude_m = std::min(stats.min_altitude_m, record.altitude_m);
        stats.max_altitude_m = std::max(stats.max_altitude_m, record.altitude_m);
        stats.min_airspeed_kts = std::min(stats.min_airspeed_kts, record.airspeed_kts);
        stats.max_airspeed_kts = std::max(stats.max_airspeed_kts, record.airspeed_kts);
        stats.min_engine_temperature_c =
            std::min(stats.min_engine_temperature_c, record.engine_temperature_c);
        stats.max_engine_temperature_c =
            std::max(stats.max_engine_temperature_c, record.engine_temperature_c);
    }

    return stats;
}

void print_table(const std::vector<flight_recorder::ReplayEntry>& entries) {
    std::cout << std::left
              << std::setw(8) << "SEQ"
              << std::setw(18) << "TIMESTAMP_US"
              << std::setw(12) << "ALT_M"
              << std::setw(14) << "AIRSPEED_KTS"
              << std::setw(13) << "HEADING_DEG"
              << std::setw(12) << "VS_FPM"
              << std::setw(14) << "ENG_TEMP_C"
              << std::setw(12) << "ENG_RPM"
              << "STATUS"
              << '\n';

    for (const auto& entry : entries) {
        const auto& record = entry.record;
        std::cout << std::left
                  << std::setw(8) << entry.sequence
                  << std::setw(18) << record.timestamp_us
                  << std::setw(12) << std::fixed << std::setprecision(2) << record.altitude_m
                  << std::setw(14) << record.airspeed_kts
                  << std::setw(13) << record.heading_deg
                  << std::setw(12) << record.vertical_speed_fpm
                  << std::setw(14) << record.engine_temperature_c
                  << std::setw(12) << record.engine_rpm
                  << status_to_string(record.system_status)
                  << '\n';
    }
}

bool write_csv(const std::string& path, const std::vector<flight_recorder::ReplayEntry>& entries) {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        return false;
    }

    output << "sequence,timestamp_us,altitude_m,airspeed_kts,heading_deg,vertical_speed_fpm,"
              "engine_temperature_c,engine_rpm,system_status\n";

    for (const auto& entry : entries) {
        const auto& record = entry.record;
        output << entry.sequence << ','
               << record.timestamp_us << ','
               << record.altitude_m << ','
               << record.airspeed_kts << ','
               << record.heading_deg << ','
               << record.vertical_speed_fpm << ','
               << record.engine_temperature_c << ','
               << record.engine_rpm << ','
               << record.system_status << '\n';
    }

    return static_cast<bool>(output);
}

void print_summary(const SummaryStats& stats, const flight_recorder::RecoveryReport& report) {
    std::cout << "summary"
              << " total_valid_records=" << stats.total_valid_records
              << " invalid_corrupt_records=" << stats.invalid_records
              << " checksum_failures=" << report.checksum_failures
              << " first_timestamp_us=" << stats.first_timestamp_us
              << " last_timestamp_us=" << stats.last_timestamp_us
              << " min_altitude_m=" << std::fixed << std::setprecision(2) << stats.min_altitude_m
              << " max_altitude_m=" << stats.max_altitude_m
              << " min_airspeed_kts=" << stats.min_airspeed_kts
              << " max_airspeed_kts=" << stats.max_airspeed_kts
              << " min_engine_temp_c=" << stats.min_engine_temperature_c
              << " max_engine_temp_c=" << stats.max_engine_temperature_c
              << '\n';
}

void print_invalid_boundary(const flight_recorder::RecoveryReport& report) {
    std::cerr << "invalid_record"
              << " first_bad_record_index=" << report.first_bad_record_index
              << " first_bad_record_sequence=" << report.first_bad_record_sequence
              << " corruption_offset=" << report.corruption_offset
              << " valid_bytes=" << report.valid_bytes
              << " bytes_scanned=" << report.bytes_scanned
              << " expected_sequence=" << report.expected_sequence
              << " reason=\"" << report.message << "\"\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        print_usage();
        return 1;
    }

    flight_recorder::RecoveryManager recovery_manager;
    flight_recorder::ReplayLog replay_log;
    flight_recorder::RecoveryReport report;
    recovery_manager.scan_replayable_log(options.log_path, replay_log, report);

    std::cout << "file_header"
              << " version=" << replay_log.metadata.format_version
              << " recorder_start_us=" << replay_log.metadata.recorder_start_time_us
              << " record_size=" << replay_log.metadata.record_size
              << " total_records_in_valid_prefix=" << replay_log.entries.size()
              << " healthy=" << (report.healthy ? "true" : "false")
              << '\n';

    const auto filtered_entries = filter_entries(replay_log.entries, options);
    const auto summary = compute_summary(filtered_entries, report);

    if (!options.summary_only) {
        print_table(filtered_entries);
    }

    print_summary(summary, report);

    if (!options.csv_path.empty()) {
        if (!write_csv(options.csv_path, filtered_entries)) {
            std::cerr << "failed to write CSV to " << options.csv_path << '\n';
            return 1;
        }
        std::cout << "csv_export path=" << options.csv_path
                  << " records=" << filtered_entries.size()
                  << '\n';
    }

    if (report.corruption_detected && options.show_invalid) {
        print_invalid_boundary(report);
    }

    return report.healthy ? 0 : 2;
}
