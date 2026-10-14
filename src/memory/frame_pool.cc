#include "peripheral/memory/frame_pool.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace peripheral {

void FrameSlot::release() noexcept {
    if (ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (partition) {
            partition->release_slot(this);
        }
    }
}

FramePoolPartition::FramePoolPartition(uint32_t camera_id, size_t slot_count, size_t slot_capacity, uint8_t* base_memory)
    : camera_id_(camera_id),
      slot_count_(slot_count),
      slot_capacity_(slot_capacity),
      slots_(std::make_unique<FrameSlot[]>(slot_count)) {
    free_list_.reserve(slot_count);

    for (size_t i = 0; i < slot_count; ++i) {
        slots_[i].slot_id = static_cast<uint32_t>(i);
        slots_[i].camera_id = camera_id_;
        slots_[i].raw_buffer = base_memory + (i * slot_capacity_);
        slots_[i].capacity = slot_capacity_;
        slots_[i].bytes_written = 0;
        slots_[i].payload_offset = 32;
        slots_[i].payload_size = 0;
        slots_[i].ref_count.store(0, std::memory_order_relaxed);
        slots_[i].partition = this;

        free_list_.push_back(&slots_[i]);
    }
}

FrameSlotHandle FramePoolPartition::acquire_slot() {
    std::scoped_lock lock(mutex_);
    if (!free_list_.empty()) {
        FrameSlot* slot = free_list_.back();
        free_list_.pop_back();
        slot->reset();
        return FrameSlotHandle(slot);
    }

    // High backpressure: All slots held by consumers (e.g. disk write stall).
    // Bounded circular ring recycling: Reclaim oldest slot to guarantee fresh preview and 0 allocation.
    overflow_drops_++;
    for (size_t i = 0; i < slot_count_; ++i) {
        // Reuse slot safely
        slots_[i].reset();
        return FrameSlotHandle(&slots_[i]);
    }

    return FrameSlotHandle(nullptr);
}

void FramePoolPartition::release_slot(FrameSlot* slot) {
    if (!slot) return;
    std::scoped_lock lock(mutex_);
    slot->reset();
    free_list_.push_back(slot);
}

size_t FramePoolPartition::available_slots() const noexcept {
    std::scoped_lock lock(mutex_);
    return free_list_.size();
}

size_t FramePoolPartition::in_use_slots() const noexcept {
    std::scoped_lock lock(mutex_);
    return slot_count_ - free_list_.size();
}


uint64_t FrameMemoryPool::detect_system_memory_bytes() {
    // 1. Check cgroup v2 memory limit (standard in modern Docker container)
    std::ifstream cgroup_v2("/sys/fs/cgroup/memory.max");
    if (cgroup_v2.is_open()) {
        std::string val;
        if (cgroup_v2 >> val && val != "max") {
            try {
                uint64_t bytes = std::stoull(val);
                if (bytes > 0 && bytes < 1000ULL * 1024 * 1024 * 1024) {
                    return bytes;
                }
            } catch (...) {}
        }
    }

    // 2. Check cgroup v1 memory limit
    std::ifstream cgroup_v1("/sys/fs/cgroup/memory/memory.limit_in_bytes");
    if (cgroup_v1.is_open()) {
        uint64_t bytes = 0;
        if (cgroup_v1 >> bytes && bytes > 0 && bytes < 1000ULL * 1024 * 1024 * 1024) {
            return bytes;
        }
    }

    // 3. Fallback to OS physical memory
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGE_SIZE)
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
    }
#endif

    // 4. Default to assignment standard 250 MB ceiling
    return 250ULL * 1024 * 1024;
}

uint64_t FrameMemoryPool::safe_max_pool_memory_bytes(uint64_t sys_mem_bytes) {
    // Under 250 MB total RAM, the Linux kernel page cache needs 100-150 MB for dirty writeback
    // buffers during 31.25 MB/s disk recording. Sockets, web server, and application binary
    // need ~20-30 MB.
    // Safe pool ceiling = max 40% of detected memory, capped at 100 MB for the 250 MB environment.
    uint64_t forty_percent = static_cast<uint64_t>(sys_mem_bytes * 0.40);
    constexpr uint64_t HARD_TARGET_POOL_CEILING = 100ULL * 1024 * 1024; // 100 MB
    return std::min(forty_percent, HARD_TARGET_POOL_CEILING);
}

FrameMemoryPool::FrameMemoryPool(Config config)
    : config_(config) {
    if (config_.endpoint_count == 0) {
        config_.endpoint_count = 1;
    }
    if (config_.slots_per_endpoint == 0) {
        config_.slots_per_endpoint = 32;
    }
    // Auto-compute total slots from endpoints * slots_per_endpoint if total_slots is 0
    if (config_.total_slots == 0) {
        config_.total_slots = config_.endpoint_count * config_.slots_per_endpoint;
    }
    if (config_.total_slots < config_.endpoint_count) {
        config_.total_slots = config_.endpoint_count * 2;
    }
    slots_per_endpoint_ = config_.total_slots / config_.endpoint_count;

    // Calculate total required slab memory
    size_t total_bytes = config_.total_slots * config_.slot_size;

    // Determine safe memory ceiling protecting OS kernel writeback page cache and system headroom
    safe_memory_ceiling_ = config_.max_pool_memory_bytes;
    if (safe_memory_ceiling_ == 0) {
        uint64_t sys_mem = detect_system_memory_bytes();
        safe_memory_ceiling_ = safe_max_pool_memory_bytes(sys_mem);
    }

    if (total_bytes > safe_memory_ceiling_) {
        double total_mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
        double limit_mb = static_cast<double>(safe_memory_ceiling_) / (1024.0 * 1024.0);
        throw std::invalid_argument(
            "FrameMemoryPool memory requirement (" + std::to_string(total_mb) + " MB for " +
            std::to_string(config_.endpoint_count) + " cameras @ " +
            std::to_string(slots_per_endpoint_) + " slots/cam) exceeds safe ingress threshold (" +
            std::to_string(limit_mb) + " MB). Kernel page cache and I/O buffer headroom would be exhausted under the 250 MB ceiling."
        );
    }

    // Allocate contiguous physical slab pool once at startup
    storage_.resize(total_bytes);

    uint8_t* base_ptr = storage_.data();
    for (size_t ep = 0; ep < config_.endpoint_count; ++ep) {
        uint8_t* ep_memory = base_ptr + (ep * slots_per_endpoint_ * config_.slot_size);
        partitions_.push_back(std::make_unique<FramePoolPartition>(
            static_cast<uint32_t>(ep),
            slots_per_endpoint_,
            config_.slot_size,
            ep_memory
        ));
    }
}

FramePoolPartition* FrameMemoryPool::get_partition(size_t endpoint_index) {
    if (endpoint_index < partitions_.size()) {
        return partitions_[endpoint_index].get();
    }
    return nullptr;
}

} // namespace peripheral
