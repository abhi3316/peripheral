#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace peripheral {

/**
 * @brief High-performance, zero-allocation streaming SHA-256 implementation.
 * Standard FIPS 180-4 compliant.
 */
class Sha256 {
public:
    Sha256() noexcept { reset(); }

    void reset() noexcept {
        state_[0] = 0x6a09e667;
        state_[1] = 0xbb67ae85;
        state_[2] = 0x3c6ef372;
        state_[3] = 0xa54ff53a;
        state_[4] = 0x510e527f;
        state_[5] = 0x9b05688c;
        state_[6] = 0x1f83d9ab;
        state_[7] = 0x5be0cd19;
        count_ = 0;
        buffer_len_ = 0;
    }

    void update(const uint8_t* data, size_t len) noexcept {
        count_ += static_cast<uint64_t>(len) * 8;

        if (buffer_len_ > 0) {
            size_t to_copy = 64 - buffer_len_;
            if (len < to_copy) {
                for (size_t i = 0; i < len; ++i) {
                    buffer_[buffer_len_ + i] = data[i];
                }
                buffer_len_ += len;
                return;
            }
            for (size_t i = 0; i < to_copy; ++i) {
                buffer_[buffer_len_ + i] = data[i];
            }
            transform(buffer_.data());
            data += to_copy;
            len -= to_copy;
            buffer_len_ = 0;
        }

        while (len >= 64) {
            transform(data);
            data += 64;
            len -= 64;
        }

        for (size_t i = 0; i < len; ++i) {
            buffer_[i] = data[i];
        }
        buffer_len_ = len;
    }

    void update(std::string_view sv) noexcept {
        update(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
    }

    [[nodiscard]] std::array<uint8_t, 32> digest() const noexcept {
        Sha256 temp = *this;
        temp.pad();
        std::array<uint8_t, 32> hash{};
        for (size_t i = 0; i < 8; ++i) {
            hash[i * 4 + 0] = static_cast<uint8_t>((temp.state_[i] >> 24) & 0xFF);
            hash[i * 4 + 1] = static_cast<uint8_t>((temp.state_[i] >> 16) & 0xFF);
            hash[i * 4 + 2] = static_cast<uint8_t>((temp.state_[i] >> 8) & 0xFF);
            hash[i * 4 + 3] = static_cast<uint8_t>((temp.state_[i] >> 0) & 0xFF);
        }
        return hash;
    }

    [[nodiscard]] std::string digest_hex() const {
        auto d = digest();
        static constexpr char hex_digits[] = "0123456789abcdef";
        std::string hex;
        hex.resize(64);
        for (size_t i = 0; i < 32; ++i) {
            hex[i * 2 + 0] = hex_digits[(d[i] >> 4) & 0x0F];
            hex[i * 2 + 1] = hex_digits[d[i] & 0x0F];
        }
        return hex;
    }

    /// Convenience static method to compute SHA-256 of any string/buffer
    static std::string hash_string(std::string_view sv) {
        Sha256 ctx;
        ctx.update(sv);
        return ctx.digest_hex();
    }

    /// Streaming SHA-256 calculation for a file without loading entire file in RAM
    static std::string hash_file(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return "";
        }
        Sha256 ctx;
        std::array<uint8_t, 65536> buffer{};
        while (file) {
            file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
            std::streamsize bytes = file.gcount();
            if (bytes > 0) {
                ctx.update(buffer.data(), static_cast<size_t>(bytes));
            }
        }
        return ctx.digest_hex();
    }

private:
    static constexpr uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    static constexpr uint32_t rotr(uint32_t x, uint32_t n) noexcept {
        return (x >> n) | (x << (32 - n));
    }
    static constexpr uint32_t ch(uint32_t x, uint32_t y, uint32_t z) noexcept {
        return (x & y) ^ (~x & z);
    }
    static constexpr uint32_t maj(uint32_t x, uint32_t y, uint32_t z) noexcept {
        return (x & y) ^ (x & z) ^ (y & z);
    }
    static constexpr uint32_t ep0(uint32_t x) noexcept {
        return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
    }
    static constexpr uint32_t ep1(uint32_t x) noexcept {
        return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
    }
    static constexpr uint32_t sig0(uint32_t x) noexcept {
        return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
    }
    static constexpr uint32_t sig1(uint32_t x) noexcept {
        return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
    }

    void transform(const uint8_t* chunk) noexcept {
        uint32_t m[64];
        for (size_t i = 0; i < 16; ++i) {
            m[i] = (static_cast<uint32_t>(chunk[i * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(chunk[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(chunk[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(chunk[i * 4 + 3]));
        }
        for (size_t i = 16; i < 64; ++i) {
            m[i] = sig1(m[i - 2]) + m[i - 7] + sig0(m[i - 15]) + m[i - 16];
        }

        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        uint32_t f = state_[5];
        uint32_t g = state_[6];
        uint32_t h = state_[7];

        for (size_t i = 0; i < 64; ++i) {
            uint32_t t1 = h + ep1(e) + ch(e, f, g) + K[i] + m[i];
            uint32_t t2 = ep0(a) + maj(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    void pad() noexcept {
        uint8_t count_bytes[8];
        for (size_t i = 0; i < 8; ++i) {
            count_bytes[i] = static_cast<uint8_t>((count_ >> ((7 - i) * 8)) & 0xFF);
        }

        uint8_t p = 0x80;
        update(&p, 1);
        while (buffer_len_ != 56) {
            uint8_t zero = 0x00;
            update(&zero, 1);
        }
        update(count_bytes, 8);
    }

    uint32_t state_[8]{};
    uint64_t count_{0};
    std::array<uint8_t, 64> buffer_{};
    size_t buffer_len_{0};
};

} // namespace peripheral
