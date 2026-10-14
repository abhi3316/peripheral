#pragma once

#include "peripheral/events/atomic_event_ring.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace peripheral {

struct Alarm {
    std::string code;
    std::string severity; // "warning" or "alarm"
    std::string camera_id;
    std::string message;
    int64_t since_us{0};

    [[nodiscard]] std::string to_json() const {
        std::ostringstream oss;
        oss << "{"
            << "\"code\":\"" << code << "\","
            << "\"severity\":\"" << severity << "\",";
        if (camera_id.empty() || camera_id == "system") {
            oss << "\"cameraId\":null,";
        } else {
            oss << "\"cameraId\":\"" << camera_id << "\",";
        }
        oss << "\"message\":\"" << escape_json(message) << "\","
            << "\"sinceUs\":" << since_us
            << "}";
        return oss.str();
    }

private:
    static std::string escape_json(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 16);
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else out += c;
        }
        return out;
    }
};

/**
 * @brief High-performance Event & Alarm Logging Subsystem.
 *
 * Couples a wait-free AtomicEventRingBuffer for sub-microsecond notification
 * streaming with an Alarm state machine that tracks active system faults and
 * automatically normalizes alarms when conditions recover.
 */
class EventLogger {
public:
    static EventLogger& instance() noexcept {
        static EventLogger logger;
        return logger;
    }

    explicit EventLogger(LogLevel default_level = LogLevel::Info)
        : min_level_(default_level) {}

    void set_log_level(LogLevel level) noexcept {
        min_level_.store(level, std::memory_order_relaxed);
    }

    [[nodiscard]] LogLevel get_log_level() const noexcept {
        return min_level_.load(std::memory_order_relaxed);
    }

    // --- Core Atomic Logging Methods ---

    uint64_t log(LogLevel level,
                 std::string_view camera_id,
                 std::string_view code,
                 std::string_view message,
                 int64_t at_us = 0) noexcept {
        if (level < min_level_.load(std::memory_order_relaxed)) {
            return 0; // Filtered out by classified level flag
        }
        return ring_.push(level, camera_id, code, message, at_us);
    }

    uint64_t debug(std::string_view camera_id, std::string_view code, std::string_view message) noexcept {
        return log(LogLevel::Debug, camera_id, code, message);
    }

    uint64_t info(std::string_view camera_id, std::string_view code, std::string_view message) noexcept {
        return log(LogLevel::Info, camera_id, code, message);
    }

    uint64_t warning(std::string_view camera_id, std::string_view code, std::string_view message) noexcept {
        return log(LogLevel::Warning, camera_id, code, message);
    }

    uint64_t alarm(std::string_view camera_id, std::string_view code, std::string_view message) noexcept {
        return log(LogLevel::Alarm, camera_id, code, message);
    }

    uint64_t error(std::string_view camera_id, std::string_view code, std::string_view message) noexcept {
        return log(LogLevel::Error, camera_id, code, message);
    }

    // --- Alarm Management State Machine ---

    bool raise_alarm(std::string_view code,
                     std::string_view severity,
                     std::string_view camera_id,
                     std::string_view message) {
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        std::string key = std::string(code) + "::" + std::string(camera_id);
        {
            std::scoped_lock lock(alarm_mutex_);
            auto it = active_alarms_.find(key);
            if (it != active_alarms_.end()) {
                // Alarm already active; update message if changed
                it->second.message = std::string(message);
                return false;
            }
            active_alarms_[key] = Alarm{std::string(code), std::string(severity), std::string(camera_id), std::string(message), now_us};
        }

        // Push event notification into atomic ring buffer
        LogLevel lvl = (severity == "alarm") ? LogLevel::Alarm : LogLevel::Warning;
        ring_.push(lvl, camera_id, code, message, now_us);
        return true;
    }

    bool clear_alarm(std::string_view code, std::string_view camera_id) {
        std::string key = std::string(code) + "::" + std::string(camera_id);
        std::string msg;
        {
            std::scoped_lock lock(alarm_mutex_);
            auto it = active_alarms_.find(key);
            if (it == active_alarms_.end()) {
                return false;
            }
            msg = "Alarm cleared: " + it->second.code + " on " + std::string(camera_id);
            active_alarms_.erase(it);
        }

        // Push recovery notification into atomic ring buffer
        ring_.push(LogLevel::Info, camera_id, std::string(code) + "_cleared", msg);
        return true;
    }

    [[nodiscard]] std::vector<Alarm> get_active_alarms() const {
        std::scoped_lock lock(alarm_mutex_);
        std::vector<Alarm> out;
        out.reserve(active_alarms_.size());
        for (const auto& [_, alarm] : active_alarms_) {
            out.push_back(alarm);
        }
        return out;
    }

    [[nodiscard]] size_t active_alarms_count() const {
        std::scoped_lock lock(alarm_mutex_);
        return active_alarms_.size();
    }

    // --- Subsystem Monitors ---

    void update_camera_link(std::string_view camera_id, bool ctrl_connected, bool vid_connected) {
        if (!ctrl_connected || !vid_connected) {
            std::string reason = (!ctrl_connected && !vid_connected) ? "Control and video links down" :
                                 (!ctrl_connected) ? "Control link down" : "Video link down";
            raise_alarm("camera_link_down", "warning", camera_id,
                        "Camera " + std::string(camera_id) + " " + reason);
        } else {
            clear_alarm("camera_link_down", camera_id);
        }
    }

    void update_motor_status(std::string_view camera_id, bool motor_stalled, std::string_view fault_msg = "") {
        if (motor_stalled) {
            std::string msg = fault_msg.empty() ? "Pan/tilt motor stall detected" : std::string(fault_msg);
            raise_alarm("motor_stalled", "alarm", camera_id, msg);
        } else {
            clear_alarm("motor_stalled", camera_id);
        }
    }

    void update_clock_sync(std::string_view camera_id, bool synchronized, double drift_ppm) {
        if (!synchronized || std::abs(drift_ppm) > 100.0) {
            std::ostringstream oss;
            oss << "Clock sync lost (drift=" << drift_ppm << " ppm)";
            raise_alarm("clock_sync_lost", "warning", camera_id, oss.str());
        } else {
            clear_alarm("clock_sync_lost", camera_id);
        }
    }

    void update_disk_usage(uint64_t used_bytes, uint64_t quota_bytes) {
        if (quota_bytes == 0) return;
        double pct = static_cast<double>(used_bytes) / static_cast<double>(quota_bytes);
        if (pct >= 0.85) {
            std::string sev = (pct >= 0.95) ? "alarm" : "warning";
            std::ostringstream oss;
            oss << "Local disk near capacity: " << (used_bytes / (1024 * 1024))
                << " MB / " << (quota_bytes / (1024 * 1024)) << " MB ("
                << static_cast<int>(pct * 100) << "%)";
            raise_alarm("disk_near_full", sev, "system", oss.str());
        } else if (pct < 0.80) {
            clear_alarm("disk_near_full", "system");
        }
    }

    // --- Querying & JSON Serialization ---

    [[nodiscard]] std::vector<EventRecord> get_recent_events(size_t max_count = 50,
                                                             LogLevel min_level = LogLevel::Debug) const {
        return ring_.get_recent_events(max_count, min_level);
    }

    [[nodiscard]] std::vector<EventRecord> get_events_since(uint64_t since_id,
                                                           size_t max_count = 100,
                                                           LogLevel min_level = LogLevel::Debug) const {
        return ring_.get_events_since(since_id, max_count, min_level);
    }

    [[nodiscard]] std::string to_events_json(const std::vector<EventRecord>& events) const {
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < events.size(); ++i) {
            if (i > 0) oss << ",";
            oss << events[i].to_json();
        }
        oss << "]";
        return oss.str();
    }

    [[nodiscard]] std::string to_alarms_json() const {
        auto alarms = get_active_alarms();
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < alarms.size(); ++i) {
            if (i > 0) oss << ",";
            oss << alarms[i].to_json();
        }
        oss << "]";
        return oss.str();
    }

    [[nodiscard]] uint64_t total_events() const noexcept {
        return ring_.total_pushed();
    }

    [[nodiscard]] size_t ring_capacity() const noexcept {
        return ring_.capacity();
    }

    AtomicEventRingBuffer<256>& ring() noexcept {
        return ring_;
    }

private:
    std::atomic<LogLevel> min_level_{LogLevel::Info};
    AtomicEventRingBuffer<256> ring_;

    mutable std::mutex alarm_mutex_;
    std::map<std::string, Alarm> active_alarms_;
};

} // namespace peripheral
