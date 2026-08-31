#include "flight_recorder/platform_info.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: platform_info [filesystem_path]\n";
        return 1;
    }
    const std::string path = argc == 2 ? argv[1] : ".";
    std::cout << flight_recorder::platform_info_json(
                     flight_recorder::collect_platform_info(path))
              << '\n';
    return 0;
}
