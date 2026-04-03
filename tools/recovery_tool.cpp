#include "flight_recorder/recovery_manager.hpp"

#include <iostream>
#include <string>

namespace {

void print_usage() {
    std::cout << "Usage: recovery_tool <log_file> [--truncate]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        print_usage();
        return 1;
    }

    const std::string path = argv[1];
    const bool truncate = argc == 3 && std::string(argv[2]) == "--truncate";

    flight_recorder::RecoveryManager recovery_manager;
    const auto startup_report = recovery_manager.recover_startup_state(path);

    std::cout << "startup_recovery"
              << " healthy=" << (startup_report.healthy ? "true" : "false")
              << " log_truncated=" << (startup_report.log_truncated ? "true" : "false")
              << " journal_found=" << (startup_report.journal_found ? "true" : "false")
              << " journal_replayed=" << (startup_report.journal_replayed ? "true" : "false")
              << " journal_cleared=" << (startup_report.journal_cleared ? "true" : "false")
              << " journal_discarded=" << (startup_report.journal_discarded ? "true" : "false")
              << " valid_records=" << startup_report.valid_records
              << " last_sequence=" << startup_report.last_sequence
              << '\n';
    std::cout << startup_report.message << '\n';

    const auto report = recovery_manager.recover(path, truncate);

    std::cout << "healthy=" << (report.healthy ? "true" : "false")
              << " truncated=" << (report.truncated ? "true" : "false")
              << " valid_records=" << report.valid_records
              << " valid_bytes=" << report.valid_bytes
              << " last_sequence=" << report.last_sequence
              << " corruption_detected=" << (report.corruption_detected ? "true" : "false")
              << " first_bad_record_index=" << report.first_bad_record_index
              << " first_bad_record_sequence=" << report.first_bad_record_sequence
              << '\n';
    std::cout << report.message << '\n';

    return report.healthy ? 0 : 2;
}
