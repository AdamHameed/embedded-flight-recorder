#pragma once

#include "flight_recorder/fault_injection.hpp"

#include <cstddef>
#include <string>

namespace flight_recorder {

struct RecorderConfig {
    std::string output_path {"flight_log.bin"};
    std::size_t buffer_size {256};
    unsigned int sample_rate_hz {20};
    std::uint32_t simulator_seed {42};
    RuntimeFaultConfig fault_config {};
};

}  // namespace flight_recorder
