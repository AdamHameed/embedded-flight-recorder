#pragma once

#include <cstddef>
#include <string>

namespace flight_recorder {

struct RecorderConfig {
    std::string output_path {"flight_log.bin"};
    std::size_t buffer_size {256};
    unsigned int sample_rate_hz {20};
};

}  // namespace flight_recorder
