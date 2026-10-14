#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace peripheral {

namespace detail {

consteval std::array<uint32_t, 256> generate_crc32_table() noexcept {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            if (c & 1) {
                c = 0xEDB88320 ^ (c >> 1);
            } else {
                c >>= 1;
            }
        }
        t[i] = c;
    }
    return t;
}

inline constexpr auto crc32_table = generate_crc32_table();

} // namespace detail

/**
 * @brief Standard IEEE 802.3 CRC-32 (polynomial 0xEDB88320).
 * Matches zlib crc32() starting from 0.
 */
class Crc32 {
public:
    static uint32_t calculate(std::span<const uint8_t> data, uint32_t initial_crc = 0) noexcept {
        uint32_t crc = ~initial_crc;
        for (uint8_t byte : data) {
            crc = detail::crc32_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
        }
        return ~crc;
    }
};

inline uint32_t crc32(std::span<const uint8_t> data, uint32_t initial = 0) noexcept {
    return Crc32::calculate(data, initial);
}

} // namespace peripheral
