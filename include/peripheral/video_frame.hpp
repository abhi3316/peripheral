#pragma once

#include "protocol.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace peripheral {

/**
 * @brief High-level domain representation of a captured video frame.
 */
struct VideoFrame {
    VideoFrameHeader header{};
    std::vector<uint8_t> image{};
    TruthStamp truth_stamp{};
    FrameFate fate{FrameFate::Sent};
    uint64_t rx_monotonic_us{0}; // Server CLOCK_MONOTONIC timestamp in µs

    // Estimated camera pose interpolated at mid-exposure
    double estimated_pan_deg{0.0};
    double estimated_tilt_deg{0.0};
    bool pose_valid{false};

    [[nodiscard]] bool is_valid() const noexcept {
        return fate == FrameFate::Sent;
    }

    [[nodiscard]] uint32_t seq() const noexcept {
        return header.frame_seq;
    }

    [[nodiscard]] uint16_t width() const noexcept {
        return header.width;
    }

    [[nodiscard]] uint16_t height() const noexcept {
        return header.height;
    }

    [[nodiscard]] std::span<const uint8_t> pixels() const noexcept {
        return image;
    }
};

} // namespace peripheral
