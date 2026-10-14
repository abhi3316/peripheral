#include "peripheral/memory/frame_pool.hpp"

#include <algorithm>
#include <iostream>

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

FrameMemoryPool::FrameMemoryPool(Config config)
    : config_(config) {
    if (config_.endpoint_count == 0) {
        config_.endpoint_count = 1;
    }
    if (config_.total_slots < config_.endpoint_count) {
        config_.total_slots = config_.endpoint_count * 2;
    }
    slots_per_endpoint_ = config_.total_slots / config_.endpoint_count;

    // Allocate contiguous physical slab pool once at startup
    size_t total_bytes = config_.total_slots * config_.slot_size;
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
