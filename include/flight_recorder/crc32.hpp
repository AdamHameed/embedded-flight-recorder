#pragma once

#include <cstddef>
#include <cstdint>
#include <array>

namespace flight_recorder {

inline const std::array<std::uint32_t, 256>& crc32_table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values {};
        for (std::uint32_t index = 0; index < values.size(); ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value >> 1u) ^ ((value & 1u) != 0u ? 0xEDB88320u : 0u);
            }
            values[index] = value;
        }
        return values;
    }();
    return table;
}

inline std::uint32_t update_crc32(std::uint32_t crc, const std::uint8_t* data, std::size_t length) {
    const auto& table = crc32_table();
    for (std::size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8u);
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
