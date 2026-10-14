#pragma once

#include "peripheral/protocol/protocol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>

namespace peripheral {

struct InterpolatedPose {
    double pan_deg{0.0};
    double tilt_deg{0.0};
    bool homed{false};
    bool valid{false};
};

/**
 * @brief High-frequency 200 Hz camera pose ring buffer and mid-exposure interpolator.
 *
 * Keeps a fixed-size circular history of recent camera pan/tilt samples.
 * When a video frame arrives, calculates the exact pan and tilt angles at the middle
 * of the exposure (t_mid = t_start + 0.5 * t_exposure) using linear interpolation.
 */
class PoseInterpolator {
public:
    static constexpr size_t CAPACITY = 1024; // ~5.12 seconds at 200 Hz

    struct PoseRecord {
        uint32_t cam_time_us{0};
        double pan_deg{0.0};
        double tilt_deg{0.0};
        bool homed{false};
    };

    PoseInterpolator() = default;

    /**
     * @brief Record an incoming 200 Hz POSE message from the camera.
     */
    void push_pose(const PosePayload& pose) {
        std::scoped_lock lock(mutex_);
        buffer_[head_] = PoseRecord{
            .cam_time_us = pose.counter,
            .pan_deg = pose.pan_deg(),
            .tilt_deg = pose.tilt_deg(),
            .homed = (pose.homed != 0)
        };
        head_ = (head_ + 1) % CAPACITY;
        if (count_ < CAPACITY) {
            count_++;
        }
    }

    /**
     * @brief Interpolate camera pan and tilt at a target camera clock microsecond timestamp.
     * @param target_cam_us Target camera clock timestamp in µs (e.g. mid-exposure time)
     * @return InterpolatedPose with estimated angles and validity flag
     */
    [[nodiscard]] InterpolatedPose interpolate(uint32_t target_cam_us) const {
        std::scoped_lock lock(mutex_);
        if (count_ == 0) {
            return InterpolatedPose{.valid = false};
        }

        // Walk backwards from newest sample to oldest
        // Video frames arrive typically 20-50 ms after exposure, which is only 4-10 samples back!
        for (size_t i = 0; i < count_ - 1; ++i) {
            size_t idx_b = (head_ + CAPACITY - 1 - i) % CAPACITY;
            size_t idx_a = (head_ + CAPACITY - 2 - i) % CAPACITY;

            const auto& b = buffer_[idx_b];
            const auto& a = buffer_[idx_a];

            // Account for normal ordering or 32-bit roll-over
            uint32_t dt_total = b.cam_time_us - a.cam_time_us;
            uint32_t dt_target = target_cam_us - a.cam_time_us;

            // Check if target is inside interval [a, b]
            if (dt_target <= dt_total && dt_total > 0 && dt_total < 100000) {
                double factor = static_cast<double>(dt_target) / static_cast<double>(dt_total);
                return InterpolatedPose{
                    .pan_deg = a.pan_deg + factor * (b.pan_deg - a.pan_deg),
                    .tilt_deg = a.tilt_deg + factor * (b.tilt_deg - a.tilt_deg),
                    .homed = a.homed && b.homed,
                    .valid = true
                };
            }
        }

        // If target is newer than newest sample, fallback to newest sample
        size_t newest_idx = (head_ + CAPACITY - 1) % CAPACITY;
        const auto& newest = buffer_[newest_idx];
        return InterpolatedPose{
            .pan_deg = newest.pan_deg,
            .tilt_deg = newest.tilt_deg,
            .homed = newest.homed,
            .valid = true
        };
    }

    void reset() {
        std::scoped_lock lock(mutex_);
        head_ = 0;
        count_ = 0;
    }

    [[nodiscard]] size_t size() const noexcept {
        std::scoped_lock lock(mutex_);
        return count_;
    }

private:
    mutable std::mutex mutex_;
    std::array<PoseRecord, CAPACITY> buffer_{};
    size_t head_{0};
    size_t count_{0};
};

} // namespace peripheral
