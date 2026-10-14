#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace peripheral {

/**
 * @brief Classified Log and Notification Levels.
 */
enum class LogLevel : uint8_t {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Alarm = 3,
    Error = 4
};

inline constexpr const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warning: return "warning";
        case LogLevel::Alarm: return "alarm";
        case LogLevel::Error: return "error";
    }
    return "info";
}

inline LogLevel parse_log_level(std::string_view str) noexcept {
    if (str == "debug" || str == "DEBUG") return LogLevel::Debug;
    if (str == "warning" || str == "WARN" || str == "warn" || str == "WARNING") return LogLevel::Warning;
    if (str == "alarm" || str == "ALARM") return LogLevel::Alarm;
    if (str == "error" || str == "ERROR") return LogLevel::Error;
    return LogLevel::Info;
}

/**
 * @brief Decoupled POD structure representing a logged event.
 */
struct EventRecord {
    uint64_t id{0};
    int64_t at_us{0};
    LogLevel level{LogLevel::Info};
    std::string camera_id;
    std::string code;
    std::string message;

    [[nodiscard]] std::string to_json() const {
        std::ostringstream oss;
        oss << "{"
            << "\"id\":" << id << ","
            << "\"atUs\":" << at_us << ","
            << "\"level\":\"" << to_string(level) << "\",";
        if (camera_id.empty() || camera_id == "system") {
            oss << "\"cameraId\":null,";
        } else {
            oss << "\"cameraId\":\"" << camera_id << "\",";
        }
        oss << "\"code\":\"" << code << "\","
            << "\"message\":\"" << escape_json(message) << "\""
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
 * @brief Lock-free, wait-free atomic circular buffer for high-throughput event notifications.
 *
 * Designed to completely avoid mutex synchronization on the critical video and ingress path.
 * Writers reserve slots via atomic fetch_add on a 64-bit sequence counter and publish records
 * with release memory semantics. Readers use acquire semantics to guarantee zero torn writes.
 * Memory is completely preallocated in a contiguous array with zero runtime heap churn.
 */
template <size_t Capacity = 256>
class AtomicEventRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
public:
    static constexpr size_t kCapacity = Capacity;
    static constexpr size_t kMask = Capacity - 1;

    struct alignas(64) Slot {
        std::atomic<uint64_t> sequence{0};
        uint64_t id{0};
        int64_t at_us{0};
        LogLevel level{LogLevel::Info};
        char camera_id[16]{0};
        char code[32]{0};
        char message[192]{0};
    };

    AtomicEventRingBuffer() {
        for (size_t i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(0, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Atomically push an event record into the ring buffer.
     * Wait-free for writers: uses atomic fetch_add ticket reservation and release store.
     */
    uint64_t push(LogLevel level,
                  std::string_view camera_id,
                  std::string_view code,
                  std::string_view message,
                  int64_t at_us = 0) noexcept {
        if (at_us <= 0) {
            at_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        uint64_t ticket = write_ticket_.fetch_add(1, std::memory_order_relaxed);
        uint64_t event_id = ticket + 1; // 1-based monotonically increasing id
        size_t idx = ticket & kMask;
        auto& slot = slots_[idx];

        slot.id = event_id;
        slot.at_us = at_us;
        slot.level = level;

        safe_copy(slot.camera_id, sizeof(slot.camera_id), camera_id);
        safe_copy(slot.code, sizeof(slot.code), code);
        safe_copy(slot.message, sizeof(slot.message), message);

        slot.sequence.store(event_id, std::memory_order_release);
        return event_id;
    }

    /**
     * @brief Retrieve up to max_count most recent events, newest first.
     * Lock-free read with acquire semantics.
     */
    [[nodiscard]] std::vector<EventRecord> get_recent_events(size_t max_count = 50,
                                                             LogLevel min_level = LogLevel::Debug) const {
        std::vector<EventRecord> result;
        uint64_t latest_ticket = write_ticket_.load(std::memory_order_acquire);
        if (latest_ticket == 0) return result;

        size_t available = (latest_ticket > Capacity) ? Capacity : static_cast<size_t>(latest_ticket);
        size_t to_fetch = std::min(available, max_count);
        result.reserve(to_fetch);

        for (size_t i = 0; i < available && result.size() < max_count; ++i) {
            uint64_t target_id = latest_ticket - i;
            size_t idx = (target_id - 1) & kMask;
            const auto& slot = slots_[idx];

            uint64_t seq = slot.sequence.load(std::memory_order_acquire);
            if (seq == target_id) {
                if (slot.level >= min_level) {
                    EventRecord rec;
                    rec.id = slot.id;
                    rec.at_us = slot.at_us;
                    rec.level = slot.level;
                    rec.camera_id = slot.camera_id;
                    rec.code = slot.code;
                    rec.message = slot.message;
                    result.push_back(std::move(rec));
                }
            }
        }
        return result;
    }

    /**
     * @brief Retrieve events since a specific ID (for polling/streaming), oldest to newest.
     */
    [[nodiscard]] std::vector<EventRecord> get_events_since(uint64_t since_id,
                                                           size_t max_count = 100,
                                                           LogLevel min_level = LogLevel::Debug) const {
        std::vector<EventRecord> result;
        uint64_t latest_ticket = write_ticket_.load(std::memory_order_acquire);
        if (latest_ticket == 0 || since_id >= latest_ticket) return result;

        uint64_t oldest_id = (latest_ticket > Capacity) ? (latest_ticket - Capacity + 1) : 1;
        uint64_t start_id = std::max(since_id + 1, oldest_id);

        for (uint64_t curr = start_id; curr <= latest_ticket && result.size() < max_count; ++curr) {
            size_t idx = (curr - 1) & kMask;
            const auto& slot = slots_[idx];
            uint64_t seq = slot.sequence.load(std::memory_order_acquire);
            if (seq == curr) {
                if (slot.level >= min_level) {
                    EventRecord rec;
                    rec.id = slot.id;
                    rec.at_us = slot.at_us;
                    rec.level = slot.level;
                    rec.camera_id = slot.camera_id;
                    rec.code = slot.code;
                    rec.message = slot.message;
                    result.push_back(std::move(rec));
                }
            }
        }
        return result;
    }

    [[nodiscard]] uint64_t total_pushed() const noexcept {
        return write_ticket_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t capacity() const noexcept {
        return Capacity;
    }

private:
    static void safe_copy(char* dest, size_t dest_size, std::string_view src) noexcept {
        if (dest_size == 0) return;
        size_t len = std::min(src.size(), dest_size - 1);
        std::memcpy(dest, src.data(), len);
        dest[len] = '\0';
    }

    alignas(64) std::atomic<uint64_t> write_ticket_{0};
    Slot slots_[Capacity];
};

} // namespace peripheral
