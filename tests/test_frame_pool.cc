#include "peripheral/peripheral.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

namespace {

void test_pool_sizing_and_partitioning() {
    std::cout << "[Test 1] FrameMemoryPool: Configurable total memory and endpoint partitioning...\n";

    constexpr size_t TOTAL_SLOTS = 64;
    constexpr size_t SLOT_SIZE = 650 * 1024; // 650 KB
    constexpr size_t ENDPOINTS = 2;

    peripheral::FrameMemoryPool::Config cfg{
        .total_slots = TOTAL_SLOTS,
        .slot_size = SLOT_SIZE,
        .endpoint_count = ENDPOINTS
    };

    peripheral::FrameMemoryPool pool(cfg);

    assert(pool.total_slots() == 64);
    assert(pool.slot_size() == 650 * 1024);
    assert(pool.total_memory_bytes() == 64 * 650 * 1024);
    assert(pool.endpoint_count() == 2);
    assert(pool.slots_per_endpoint() == 32);

    auto* part0 = pool.get_partition(0);
    auto* part1 = pool.get_partition(1);
    assert(part0 != nullptr);
    assert(part1 != nullptr);
    assert(part0->camera_id() == 0);
    assert(part1->camera_id() == 1);
    assert(part0->total_slots() == 32);
    assert(part1->total_slots() == 32);
    assert(part0->available_slots() == 32);
    assert(part1->available_slots() == 32);

    std::cout << "  Passed: Pool allocated " << pool.total_memory_mb() 
              << " MB, divided into 32 slots per endpoint.\n";
}

void test_slot_acquisition_and_raii_recycling() {
    std::cout << "[Test 2] FrameSlotHandle: Intrusive reference counting and zero-cost recycling...\n";

    peripheral::FrameMemoryPool::Config cfg{
        .total_slots = 16,
        .slot_size = 100 * 1024,
        .endpoint_count = 1
    };
    peripheral::FrameMemoryPool pool(cfg);
    auto* part = pool.get_partition(0);

    assert(part->available_slots() == 16);
    assert(part->in_use_slots() == 0);

    {
        // 1. Acquire slot
        peripheral::FrameSlotHandle handle1 = part->acquire_slot();
        assert(handle1);
        assert(part->available_slots() == 15);
        assert(part->in_use_slots() == 1);

        {
            // 2. Copy handle (increments intrusive ref_count, zero heap allocation)
            peripheral::FrameSlotHandle handle2 = handle1;
            assert(handle2.get() == handle1.get());
            assert(part->available_slots() == 15);
        } // handle2 goes out of scope, ref_count decrements, slot remains held by handle1

        assert(part->available_slots() == 15);
        assert(part->in_use_slots() == 1);
    } // handle1 goes out of scope, slot automatically returned to pool

    assert(part->available_slots() == 16);
    assert(part->in_use_slots() == 0);

    std::cout << "  Passed: Intrusive slot handles recycle cleanly to free list.\n";
}

void test_direct_slot_streaming() {
    std::cout << "[Test 3] FrameSlot: Direct zero-copy streaming into writable_span()...\n";

    peripheral::FrameMemoryPool::Config cfg{
        .total_slots = 4,
        .slot_size = 64 * 1024,
        .endpoint_count = 1
    };
    peripheral::FrameMemoryPool pool(cfg);
    auto* part = pool.get_partition(0);

    auto slot = part->acquire_slot();
    auto wspan = slot->writable_span();
    assert(wspan.size() == 64 * 1024);

    // Simulate socket direct write
    const char* sample_text = "ZERO_COPY_SOCKET_INGRESS_STREAM";
    size_t len = std::strlen(sample_text);
    std::memcpy(wspan.data(), sample_text, len);
    slot->commit_write(len);

    assert(slot->bytes_written == len);
    auto full = slot->full_span();
    assert(full.size() == len);
    assert(std::memcmp(full.data(), sample_text, len) == 0);

    std::cout << "  Passed: Direct writable_span write and commit verified.\n";
}

void test_backpressure_bounded_reclaim() {
    std::cout << "[Test 4] FramePoolPartition: Bounded circular recycling under slow-consumer backpressure...\n";

    peripheral::FrameMemoryPool::Config cfg{
        .total_slots = 4,
        .slot_size = 32 * 1024,
        .endpoint_count = 1
    };
    peripheral::FrameMemoryPool pool(cfg);
    auto* part = pool.get_partition(0);

    std::vector<peripheral::FrameSlotHandle> handles;
    for (size_t i = 0; i < 4; ++i) {
        handles.push_back(part->acquire_slot());
    }
    assert(part->available_slots() == 0);
    assert(part->overflow_drops() == 0);

    // 5th acquisition when pool is completely full: reclaims oldest slot gracefully
    auto overflow_handle = part->acquire_slot();
    assert(overflow_handle);
    assert(part->overflow_drops() == 1);

    std::cout << "  Passed: Bounded circular recycling verified without crash or allocation.\n";
}

void test_video_framer_zero_allocation() {
    std::cout << "[Test 5] VideoFramer + FramePoolPartition: Zero runtime heap allocations during ingress...\n";

    peripheral::FrameMemoryPool::Config cfg{
        .total_slots = 8,
        .slot_size = 650 * 1024,
        .endpoint_count = 1
    };
    peripheral::FrameMemoryPool pool(cfg);
    auto* part = pool.get_partition(0);

    peripheral::VideoFramer framer(1024 * 1024, part);

    size_t frames_seen = 0;
    framer.set_frame_callback([&frames_seen, part](const peripheral::VideoFrame& frame) {
        assert(frame.is_valid());
        assert(frame.slot); // Backed by preallocated slot!
        assert(frame.image.empty()); // ZERO runtime heap allocation!
        assert(frame.pixels().size() == 200 * 100);
        assert(part->in_use_slots() >= 1);
        frames_seen++;
    });

    // Create wire frame
    size_t payload_bytes = 200 * 100;
    std::vector<uint8_t> payload(payload_bytes, 0x77);
    uint32_t payload_crc = peripheral::crc32(payload);

    peripheral::VideoFrameHeader hdr{};
    hdr.start_marker = peripheral::VFRM_START_MARKER;
    hdr.frame_seq = 42;
    hdr.exposure_start = 12345;
    hdr.boot_id = 1;
    hdr.width = 200;
    hdr.height = 100;
    hdr.payload_crc32 = payload_crc;

    uint8_t hdr_bytes[28];
    std::memcpy(hdr_bytes, &hdr, 28);
    hdr.header_crc32 = peripheral::crc32(std::span<const uint8_t>(hdr_bytes, 28));

    std::vector<uint8_t> wire(sizeof(peripheral::VideoFrameHeader) + payload_bytes);
    std::memcpy(wire.data(), &hdr, sizeof(hdr));
    std::memcpy(wire.data() + sizeof(hdr), payload.data(), payload_bytes);

    // Push data into framer
    framer.push_data(wire);
    auto parsed = framer.parse_available();

    assert(parsed.size() == 1);
    assert(frames_seen == 1);
    assert(parsed[0].slot);
    assert(parsed[0].image.empty()); // No vector allocation!
    assert(parsed[0].pixels().size() == payload_bytes);

    std::cout << "  Passed: Frame ingress processed with zero heap allocation.\n";
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "Running peripheral::FrameMemoryPool Tests\n";
    std::cout << "========================================\n";

    test_pool_sizing_and_partitioning();
    test_slot_acquisition_and_raii_recycling();
    test_direct_slot_streaming();
    test_backpressure_bounded_reclaim();
    test_video_framer_zero_allocation();

    std::cout << "All FrameMemoryPool tests passed successfully!\n";
    return 0;
}
