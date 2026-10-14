#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace peripheral {

class FramePoolPartition;

/**
 * @brief Preallocated memory slot holding one video frame.
 *
 * Sockets stream directly into writable_span() to eliminate runtime heap allocations.
 * Reference-counted intrusively via FrameSlotHandle for zero-overhead, lock-free ownership.
 */
struct FrameSlot {
    uint32_t slot_id{0};
    uint32_t camera_id{0};
    uint8_t* raw_buffer{nullptr};
    size_t capacity{0};
    size_t bytes_written{0};
    size_t payload_offset{32}; // sizeof(VideoFrameHeader)
    size_t payload_size{0};

    std::atomic<uint32_t> ref_count{0};
    FramePoolPartition* partition{nullptr};

    FrameSlot() = default;
    FrameSlot(const FrameSlot&) = delete;
    FrameSlot& operator=(const FrameSlot&) = delete;

    FrameSlot(FrameSlot&& other) noexcept
        : slot_id(other.slot_id),
          camera_id(other.camera_id),
          raw_buffer(other.raw_buffer),
          capacity(other.capacity),
          bytes_written(other.bytes_written),
          payload_offset(other.payload_offset),
          payload_size(other.payload_size),
          ref_count(other.ref_count.load(std::memory_order_relaxed)),
          partition(other.partition) {}

    FrameSlot& operator=(FrameSlot&& other) noexcept {
        if (this != &other) {
            slot_id = other.slot_id;
            camera_id = other.camera_id;
            raw_buffer = other.raw_buffer;
            capacity = other.capacity;
            bytes_written = other.bytes_written;
            payload_offset = other.payload_offset;
            payload_size = other.payload_size;
            ref_count.store(other.ref_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
            partition = other.partition;
        }
        return *this;
    }

    [[nodiscard]] std::span<uint8_t> writable_span(size_t /*min_bytes*/ = 0) noexcept {
        if (bytes_written >= capacity) {
            return {};
        }
        return std::span<uint8_t>(raw_buffer + bytes_written, capacity - bytes_written);
    }

    void commit_write(size_t bytes) noexcept {
        bytes_written = std::min(capacity, bytes_written + bytes);
    }

    [[nodiscard]] std::span<uint8_t> full_span() noexcept {
        return std::span<uint8_t>(raw_buffer, bytes_written);
    }

    [[nodiscard]] std::span<const uint8_t> full_span() const noexcept {
        return std::span<const uint8_t>(raw_buffer, bytes_written);
    }

    [[nodiscard]] std::span<const uint8_t> payload() const noexcept {
        if (payload_offset >= bytes_written) return {};
        size_t avail = bytes_written - payload_offset;
        size_t sz = (payload_size > 0) ? std::min(payload_size, avail) : avail;
        return std::span<const uint8_t>(raw_buffer + payload_offset, sz);
    }

    [[nodiscard]] std::span<uint8_t> mutable_payload() noexcept {
        if (payload_offset >= bytes_written) return {};
        size_t avail = bytes_written - payload_offset;
        size_t sz = (payload_size > 0) ? std::min(payload_size, avail) : avail;
        return std::span<uint8_t>(raw_buffer + payload_offset, sz);
    }

    void reset() noexcept {
        bytes_written = 0;
        payload_offset = 32;
        payload_size = 0;
    }

    void retain() noexcept {
        ref_count.fetch_add(1, std::memory_order_relaxed);
    }

    void release() noexcept;
};

/**
 * @brief Intrusive RAII handle to a FrameSlot.
 *        Guarantees zero heap allocation for reference-counting control blocks.
 */
class FrameSlotHandle {
public:
    FrameSlotHandle() noexcept : slot_(nullptr) {}

    explicit FrameSlotHandle(FrameSlot* slot) noexcept : slot_(slot) {
        if (slot_) {
            slot_->retain();
        }
    }

    ~FrameSlotHandle() {
        reset();
    }

    FrameSlotHandle(const FrameSlotHandle& other) noexcept : slot_(other.slot_) {
        if (slot_) {
            slot_->retain();
        }
    }

    FrameSlotHandle& operator=(const FrameSlotHandle& other) noexcept {
        if (this != &other) {
            reset();
            slot_ = other.slot_;
            if (slot_) {
                slot_->retain();
            }
        }
        return *this;
    }

    FrameSlotHandle(FrameSlotHandle&& other) noexcept : slot_(other.slot_) {
        other.slot_ = nullptr;
    }

    FrameSlotHandle& operator=(FrameSlotHandle&& other) noexcept {
        if (this != &other) {
            reset();
            slot_ = other.slot_;
            other.slot_ = nullptr;
        }
        return *this;
    }

    void reset() noexcept {
        if (slot_) {
            slot_->release();
            slot_ = nullptr;
        }
    }

    [[nodiscard]] FrameSlot* get() const noexcept { return slot_; }
    [[nodiscard]] FrameSlot* operator->() const noexcept { return slot_; }
    [[nodiscard]] FrameSlot& operator*() const noexcept { return *slot_; }
    [[nodiscard]] explicit operator bool() const noexcept { return slot_ != nullptr; }

private:
    FrameSlot* slot_{nullptr};
};

/**
 * @brief Dedicated circular pool partition for a single camera endpoint.
 *        Maintains strict memory isolation so one camera cannot exhaust buffers of another.
 */
class FramePoolPartition {
public:
    FramePoolPartition(uint32_t camera_id, size_t slot_count, size_t slot_capacity, uint8_t* base_memory);
    ~FramePoolPartition() = default;

    FramePoolPartition(const FramePoolPartition&) = delete;
    FramePoolPartition& operator=(const FramePoolPartition&) = delete;
    FramePoolPartition(FramePoolPartition&&) = delete;
    FramePoolPartition& operator=(FramePoolPartition&&) = delete;

    /**
     * @brief Acquires an idle slot from the circular pool.
     *        If all slots are in use (slow consumer backpressure), drops oldest and reclaims.
     */
    FrameSlotHandle acquire_slot();

    /**
     * @brief Internal recycle callback invoked when FrameSlotHandle ref_count drops to 0.
     */
    void release_slot(FrameSlot* slot);

    [[nodiscard]] uint32_t camera_id() const noexcept { return camera_id_; }
    [[nodiscard]] size_t total_slots() const noexcept { return slot_count_; }
    [[nodiscard]] size_t slot_capacity() const noexcept { return slot_capacity_; }
    [[nodiscard]] size_t available_slots() const noexcept;
    [[nodiscard]] size_t in_use_slots() const noexcept;
    [[nodiscard]] uint64_t overflow_drops() const noexcept { return overflow_drops_.load(); }

private:
    uint32_t camera_id_;
    size_t slot_count_{0};
    size_t slot_capacity_{0};
    std::unique_ptr<FrameSlot[]> slots_;

    mutable std::mutex mutex_;
    std::vector<FrameSlot*> free_list_;
    std::atomic<uint64_t> overflow_drops_{0};
};

/**
 * @brief Master Preallocated Slab Pool allocator.
 *        Allocates a single contiguous block of memory at startup and divides slots
 *        equally across all registered camera endpoints.
 */
class FrameMemoryPool {
public:
    struct Config {
        size_t endpoint_count{2};               // e.g. 2 cameras (cam0, cam1)
        size_t slots_per_endpoint{32};          // Default 32 slots per camera
        size_t slot_size{650 * 1024};           // 650 KB per slot (fits 1000x625 + header)
        size_t total_slots{0};                  // 0 = auto-compute (endpoint_count * slots_per_endpoint)
        uint64_t max_pool_memory_bytes{0};      // 0 = auto-detect safe ceiling (< 100 MB under 250 MB ceiling)
    };

    explicit FrameMemoryPool(Config config);
    ~FrameMemoryPool() = default;

    FrameMemoryPool(const FrameMemoryPool&) = delete;
    FrameMemoryPool& operator=(const FrameMemoryPool&) = delete;

    [[nodiscard]] FramePoolPartition* get_partition(size_t endpoint_index);
    [[nodiscard]] size_t total_slots() const noexcept { return config_.total_slots; }
    [[nodiscard]] size_t slot_size() const noexcept { return config_.slot_size; }
    [[nodiscard]] size_t total_memory_bytes() const noexcept { return storage_.size(); }
    [[nodiscard]] double total_memory_mb() const noexcept {
        return static_cast<double>(storage_.size()) / (1024.0 * 1024.0);
    }
    [[nodiscard]] size_t slots_per_endpoint() const noexcept { return slots_per_endpoint_; }
    [[nodiscard]] size_t endpoint_count() const noexcept { return partitions_.size(); }
    [[nodiscard]] uint64_t max_pool_memory_bytes() const noexcept { return safe_memory_ceiling_; }

    /// Helper to detect host/container memory and calculate safe frame pool memory budget
    static uint64_t detect_system_memory_bytes();
    static uint64_t safe_max_pool_memory_bytes(uint64_t sys_mem_bytes);

private:
    Config config_;
    size_t slots_per_endpoint_{0};
    uint64_t safe_memory_ceiling_{0};
    std::vector<uint8_t> storage_;
    std::vector<std::unique_ptr<FramePoolPartition>> partitions_;
};

} // namespace peripheral
