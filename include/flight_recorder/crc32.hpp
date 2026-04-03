#pragma once

#include <cstddef>
#include <cstdint>

namespace flight_recorder {

inline std::uint32_t update_crc32(std::uint32_t crc, const std::uint8_t* data, std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            const bool lsb_set = (crc & 1u) != 0u;
            crc >>= 1u;
            if (lsb_set) {
                crc ^= 0xEDB88320u;
            }
        }
    }
    return crc;
}

inline std::uint32_t compute_crc32(const void* data, std::size_t length) {
    auto crc = 0xFFFFFFFFu;
    crc = update_crc32(crc, static_cast<const std::uint8_t*>(data), length);
    return crc ^ 0xFFFFFFFFu;
}

inline std::uint32_t compute_crc32_segments(const void* first,
                                            std::size_t first_length,
                                            const void* second,
                                            std::size_t second_length) {
    auto crc = 0xFFFFFFFFu;
    crc = update_crc32(crc, static_cast<const std::uint8_t*>(first), first_length);
    crc = update_crc32(crc, static_cast<const std::uint8_t*>(second), second_length);
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace flight_recorder
