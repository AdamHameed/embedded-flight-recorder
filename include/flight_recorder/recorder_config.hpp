#pragma once

#include "flight_recorder/fault_injection.hpp"

#include <cstddef>
#include <string>

namespace flight_recorder {

struct RecorderConfig {
    std::string output_path {"flight_log.bin"};
    std::size_t buffer_size {256};
    std::size_t batch_size {64};
    std::size_t sync_every_batches {1};
    // Zero disables the timer. Otherwise measure from the first pending write.
    std::uint32_t flush_interval_ms {0};
    std::size_t preallocation_chunk_bytes {std::size_t {4} * 1024u * 1024u};
    std::size_t serialization_buffer_alignment {4096};
    unsigned int sample_rate_hz {20};
    bool unpaced_producer {false};
    std::uint32_t simulator_seed {42};
    RuntimeFaultConfig fault_config {};
};

}  // namespace flight_recorder
