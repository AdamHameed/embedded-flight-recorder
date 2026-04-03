#include "flight_recorder/recovery_manager.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void print_usage() {
    std::cout << "Usage: replay_tool <log_file>\n";
}

std::string status_to_string(std::uint32_t status) {
    std::ostringstream output;
    output << "0x" << std::hex << status << std::dec;
    return output.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        print_usage();
        return 1;
    }

    const std::string path = argv[1];
    flight_recorder::RecoveryManager recovery_manager;
    flight_recorder::ReplayLog replay_log;
    std::string error;

    if (!recovery_manager.read_log(path, replay_log, error)) {
        std::cerr << "Failed to replay log: " << error << '\n';
        return 1;
    }

    std::cout << "file_header"
              << " version=" << replay_log.metadata.format_version
              << " recorder_start_us=" << replay_log.metadata.recorder_start_time_us
              << " record_size=" << replay_log.metadata.record_size
              << " records=" << replay_log.entries.size()
              << '\n';

    for (const auto& entry : replay_log.entries) {
        const auto& record = entry.record;
        std::cout << "seq=" << entry.sequence
                  << " ts_us=" << record.timestamp_us
                  << " alt_m=" << std::fixed << std::setprecision(2) << record.altitude_m
                  << " airspeed_kts=" << record.airspeed_kts
                  << " heading_deg=" << record.heading_deg
                  << " vs_fpm=" << record.vertical_speed_fpm
                  << " eng_temp_c=" << record.engine_temperature_c
                  << " eng_rpm=" << record.engine_rpm
                  << " status=" << status_to_string(record.system_status)
                  << '\n';
    }

    return 0;
}
