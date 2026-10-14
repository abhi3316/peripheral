#pragma once

#include "peripheral/protocol/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>

namespace peripheral {

/**
 * =========================================================================================
 * ARCHITECTURAL NOTE ON PRODUCTION DESIGN & PTP (IEEE 1588) ALTERNATIVE:
 *
 * In an ideal production environment where cameras and host network interfaces (NICs)
 * support hardware PTP (IEEE 1588 / 802.1AS), clock synchronization is performed directly
 * at the Ethernet MAC/PHY layer using hardware timestamping and standard system daemons 
 * (e.g. linuxptp / ptp4l / phc2sys).
 *
 * Under that architecture:
 *  1. The camera's hardware clock and the host's system clock are continuously disciplined
 *     to sub-microsecond alignment.
 *  2. Camera timestamps on video frames and telemetry directly correlate to system
 *     CLOCK_REALTIME / CLOCK_MONOTONIC.
 *  3. This entire software synchronization loop (4 Hz TIME_REQ polling, offset tracking,
 *     and software filtering) would NOT be present—the server would simply read the
 *     camera timestamp directly as a valid system time.
 *
 * However, because:
 *  - The target deployment is constrained to 1 CPU core, 250 MB RAM, and userland permissions.
 *  - The camera communicates via a standard application-layer TCP socket without hardware PTP.
 *  - The camera's 32-bit clock counter drifts by up to ±100 ppm, resets on reboot, and
 *    rolls over to 0 every 71.58 minutes.
 *
 * We implement this userland software TimeSyncFilter. It applies PTP's 4-timestamp formula
 * (t1, t2, t3, t4) over TCP to compute round-trip delay, reject network jitter, handle
 * 32-bit counter unwrapping, and maintain a smooth clock offset with zero runtime heap allocations.
 * =========================================================================================
 */
class TimeSyncFilter {
public:
    TimeSyncFilter() = default;

    /**
     * @brief Process a TIME_RESP message from the camera.
     * @param t1 Server send timestamp in microseconds (echoed from TIME_REQ)
     * @param t2 Camera clock timestamp when request arrived (32-bit µs)
     * @param t3 Camera clock timestamp when reply was sent (32-bit µs)
     * @param t4 Server receive timestamp in microseconds (CLOCK_MONOTONIC)
     * @param boot_id Current camera boot sequence ID
     */
    void on_time_resp(uint64_t t1, uint32_t t2, uint32_t t3, uint64_t t4, uint16_t boot_id) {
        std::scoped_lock lock(mutex_);

        // Reset if camera rebooted
        if (boot_id != current_boot_id_) {
            reset_locked(boot_id);
        }

        // Unwrap 32-bit camera timestamps to continuous 64-bit space
        uint64_t u_t2 = update_and_unwrap(t2);
        uint64_t u_t3 = update_and_unwrap(t3);

        // Calculate Round-Trip Time (RTT) and Camera Dwell Time
        int64_t round_trip = static_cast<int64_t>(t4 - t1);
        int64_t cam_dwell = static_cast<int64_t>(u_t3 - u_t2);
        int64_t net_rtt = std::max<int64_t>(0, round_trip - cam_dwell);

        // PTP Clock Offset formula:
        // offset = ((t1 - t2) + (t4 - t3)) / 2
        // where server_time = camera_time + offset
        int64_t raw_offset = (static_cast<int64_t>(t1 - u_t2) + static_cast<int64_t>(t4 - u_t3)) / 2;

        last_rtt_us_ = static_cast<uint64_t>(net_rtt);

        if (sample_count_ == 0) {
            filtered_offset_ = raw_offset;
            min_rtt_us_ = static_cast<uint64_t>(net_rtt);
            sample_count_ = 1;
            synchronized_ = true;
            return;
        }

        // Jitter filter: Track minimum RTT with slow decay
        if (static_cast<uint64_t>(net_rtt) < min_rtt_us_) {
            min_rtt_us_ = static_cast<uint64_t>(net_rtt);
        } else {
            min_rtt_us_ += 1; // slow decay to adapt to network baseline shifts
        }

        // Discard samples where RTT is an outlier (spike > 2x min_rtt + 1000 µs)
        uint64_t max_acceptable_rtt = min_rtt_us_ * 2 + 1000;
        if (static_cast<uint64_t>(net_rtt) > max_acceptable_rtt && sample_count_ >= 1) {
            return; // reject jitter outlier
        }

        // Exponential Moving Average (EMA) to smooth clock drift
        // ALPHA = 0.25 provides smooth filtering while responding to 100 ppm drift in ~1s
        constexpr double ALPHA = 0.25;
        filtered_offset_ = static_cast<int64_t>((1.0 - ALPHA) * filtered_offset_ + ALPHA * raw_offset);
        sample_count_++;
    }

    /**
     * @brief Translate a 32-bit camera clock timestamp to server monotonic microseconds.
     */
    [[nodiscard]] uint64_t camera_to_server_us(uint32_t cam_time_32) const {
        std::scoped_lock lock(mutex_);
        if (!synchronized_) {
            return 0;
        }
        uint64_t unwrapped_cam = unwrap_query(cam_time_32);
        int64_t server_time = static_cast<int64_t>(unwrapped_cam) + filtered_offset_;
        return (server_time > 0) ? static_cast<uint64_t>(server_time) : 0;
    }

    /**
     * @brief Translate server monotonic microseconds back to camera 32-bit clock microseconds.
     */
    [[nodiscard]] uint32_t server_to_camera_us(uint64_t server_mono_us) const {
        std::scoped_lock lock(mutex_);
        if (!synchronized_) {
            return 0;
        }
        int64_t cam_time = static_cast<int64_t>(server_mono_us) - filtered_offset_;
        return static_cast<uint32_t>(cam_time & 0xFFFFFFFFULL);
    }

    void on_boot_change(uint16_t new_boot_id) {
        std::scoped_lock lock(mutex_);
        if (new_boot_id != current_boot_id_) {
            reset_locked(new_boot_id);
        }
    }

    void reset() {
        std::scoped_lock lock(mutex_);
        reset_locked(0);
    }

    [[nodiscard]] bool is_synchronized() const noexcept {
        std::scoped_lock lock(mutex_);
        return synchronized_;
    }

    [[nodiscard]] int64_t current_offset_us() const noexcept {
        std::scoped_lock lock(mutex_);
        return filtered_offset_;
    }

    [[nodiscard]] uint64_t last_rtt_us() const noexcept {
        std::scoped_lock lock(mutex_);
        return last_rtt_us_;
    }

    [[nodiscard]] uint64_t sample_count() const noexcept {
        std::scoped_lock lock(mutex_);
        return sample_count_;
    }

private:
    void reset_locked(uint16_t boot_id) {
        current_boot_id_ = boot_id;
        filtered_offset_ = 0;
        min_rtt_us_ = 0;
        last_rtt_us_ = 0;
        sample_count_ = 0;
        synchronized_ = false;
        ref_cam_32_ = 0;
        ref_cam_unwrapped_ = 0;
        first_time_seen_ = false;
    }

    // Updates reference camera time and returns unwrapped 64-bit value
    uint64_t update_and_unwrap(uint32_t c32) {
        if (!first_time_seen_) {
            first_time_seen_ = true;
            ref_cam_unwrapped_ = c32;
            ref_cam_32_ = c32;
            return c32;
        }
        int32_t diff = static_cast<int32_t>(c32 - ref_cam_32_);
        ref_cam_unwrapped_ += diff;
        ref_cam_32_ = c32;
        return ref_cam_unwrapped_;
    }

    // Pure query: unwraps any 32-bit camera timestamp relative to the reference without mutating state
    uint64_t unwrap_query(uint32_t c32) const {
        if (!first_time_seen_) {
            return c32;
        }
        int32_t diff = static_cast<int32_t>(c32 - ref_cam_32_);
        return ref_cam_unwrapped_ + diff;
    }

    mutable std::mutex mutex_;
    uint16_t current_boot_id_{0};
    int64_t filtered_offset_{0};
    uint64_t min_rtt_us_{0};
    uint64_t last_rtt_us_{0};
    uint64_t sample_count_{0};
    bool synchronized_{false};

    // 32-bit unwrapper state
    bool first_time_seen_{false};
    uint32_t ref_cam_32_{0};
    uint64_t ref_cam_unwrapped_{0};
};

} // namespace peripheral
