#include "flight_recorder/fault_injection.hpp"
#include "flight_recorder/recovery_manager.hpp"

#include <iostream>
#include <string>

namespace {

void print_usage() {
    std::cout
        << "Usage:\n"
        << "  fault_injector truncate <log_file> <bytes>\n"
        << "  fault_injector corrupt <log_file> <sequence> <record_byte_offset>\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const std::string mode = argv[1];
    std::string error;

    if (mode == "truncate" && argc == 4) {
        const std::string path = argv[2];
        const auto bytes = static_cast<std::size_t>(std::stoull(argv[3]));
        if (!flight_recorder::FaultInjector::truncate_log_tail(path, bytes, error)) {
            std::cerr << "truncate failed: " << error << '\n';
            return 1;
        }
        std::cout << "truncate_complete path=" << path << " bytes_removed=" << bytes << '\n';
        return 0;
    }

    if (mode == "corrupt" && argc == 5) {
        const std::string path = argv[2];
        const auto sequence = static_cast<std::uint64_t>(std::stoull(argv[3]));
        const auto offset = static_cast<std::size_t>(std::stoull(argv[4]));
        if (!flight_recorder::FaultInjector::corrupt_record_byte(path, sequence, offset, error)) {
            std::cerr << "corrupt failed: " << error << '\n';
            return 1;
        }
        std::cout << "corruption_complete path=" << path
                  << " sequence=" << sequence
                  << " offset=" << offset
                  << '\n';
        return 0;
    }

    print_usage();
    return 1;
}
