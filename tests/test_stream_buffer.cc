#include "peripheral/memory/stream_buffer.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

void test_basic_rw() {
    std::cout << "[Test 1] StreamBuffer: basic write, read, and consume...\n";

    peripheral::StreamBuffer buf(1024 * 64);
    assert(buf.empty());
    assert(buf.readable_bytes() == 0);

    // Direct write via writable_span
    auto writable = buf.writable_span();
    assert(writable.size() >= 100);

    for (size_t i = 0; i < 100; ++i) {
        writable[i] = static_cast<uint8_t>(i & 0xFF);
    }
    buf.commit_write(100);

    assert(buf.readable_bytes() == 100);
    auto readable = buf.readable_span();
    assert(readable.size() == 100);
    for (size_t i = 0; i < 100; ++i) {
        assert(readable[i] == static_cast<uint8_t>(i & 0xFF));
    }

    // Consume first 40 bytes
    buf.consume(40);
    assert(buf.readable_bytes() == 60);
    readable = buf.readable_span();
    assert(readable.size() == 60);
    assert(readable[0] == 40);

    // Consume remaining 60 bytes
    buf.consume(60);
    assert(buf.empty());
    assert(buf.readable_bytes() == 0);

    std::cout << "  Passed: Basic write/read/consume verified.\n";
}

void test_auto_compaction() {
    std::cout << "[Test 2] StreamBuffer: auto-compaction when tail space is low...\n";

    peripheral::StreamBuffer buf(64 * 1024);

    // Fill 40 KB
    std::vector<uint8_t> chunk(40 * 1024, 0xAA);
    assert(buf.append(chunk));
    assert(buf.readable_bytes() == 40 * 1024);

    // Consume 30 KB from front
    buf.consume(30 * 1024);
    assert(buf.readable_bytes() == 10 * 1024);

    // Request writable span of 35 KB (triggers compaction)
    auto wspan = buf.writable_span(35 * 1024);
    (void)wspan;
    assert(wspan.size() >= 35 * 1024);

    auto readable = buf.readable_span();
    assert(readable.size() == 10 * 1024);
    for (size_t i = 0; i < readable.size(); ++i) {
        assert(readable[i] == 0xAA);
    }

    std::cout << "  Passed: Auto-compaction verified without data corruption.\n";
}

void test_marker_search() {
    std::cout << "[Test 3] StreamBuffer: find_marker with garbage resilience...\n";

    peripheral::StreamBuffer buf(128 * 1024);

    std::vector<uint8_t> garbage(5000, 0x55);
    buf.append(garbage);

    constexpr uint32_t VFRM_MARKER = 0x4D524656;
    constexpr uint32_t PTCL_MARKER = 0x4C435450;

    assert(!buf.find_marker(VFRM_MARKER).has_value());

    uint8_t vfrm_bytes[4] = {0x56, 0x46, 0x52, 0x4D};
    buf.append(std::span<const uint8_t>(vfrm_bytes, 4));

    auto found = buf.find_marker(VFRM_MARKER);
    assert(found.has_value());
    assert(found.value() == 5000);

    buf.consume(found.value());
    assert(buf.readable_bytes() == 4);
    assert(buf.find_marker(VFRM_MARKER).value() == 0);

    uint8_t ptcl_bytes[4] = {'P', 'T', 'C', 'L'};
    buf.append(std::span<const uint8_t>(ptcl_bytes, 4));
    auto found_ptcl = buf.find_marker(PTCL_MARKER);
    (void)found_ptcl;
    assert(found_ptcl.has_value());
    assert(found_ptcl.value() == 4);

    std::cout << "  Passed: Marker search verified across 5,000 garbage bytes.\n";
}

void test_concurrent_producer_consumers() {
    std::cout << "[Test 4] StreamBuffer: concurrent Producer (Socket), Consumer (Cloud/Storage), and Peeker (Web Viewer)...\n";

    peripheral::StreamBuffer buffer(512 * 1024);

    constexpr size_t TOTAL_CHUNKS = 500;
    constexpr size_t CHUNK_SIZE = 1024;
    const size_t TOTAL_BYTES = TOTAL_CHUNKS * CHUNK_SIZE;

    std::atomic<bool> producer_done{false};
    std::atomic<size_t> bytes_peeked_by_viewer{0};
    std::atomic<size_t> bytes_consumed_by_cloud{0};

    // Thread 1: Socket Receiver (Producer)
    std::thread producer([&]() {
        std::vector<uint8_t> chunk(CHUNK_SIZE, 0x33);
        for (size_t i = 0; i < TOTAL_CHUNKS; ++i) {
            // Write chunk
            while (!buffer.append(chunk)) {
                // Buffer momentarily full; wait for consumer to drain
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            // Yield so consumer and viewer can interleave
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
        producer_done = true;
    });

    // Thread 2: Web Viewer (Non-destructive Peeker for live preview)
    std::thread viewer([&]() {
        std::vector<uint8_t> preview_buf(128);
        while (!producer_done || buffer.readable_bytes() > 0) {
            size_t n = buffer.peek(preview_buf, 0);
            if (n > 0) {
                bytes_peeked_by_viewer += n;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Thread 3: Cloud Uploader / Disk Recorder (Draining Consumer)
    std::thread cloud_uploader([&]() {
        std::vector<uint8_t> drain_buf(2048);
        while (!producer_done || buffer.readable_bytes() > 0) {
            if (buffer.wait_readable(1, std::chrono::milliseconds(20))) {
                size_t read_bytes = buffer.read(drain_buf);
                bytes_consumed_by_cloud += read_bytes;
            }
        }
    });

    producer.join();
    viewer.join();
    cloud_uploader.join();

    assert(bytes_consumed_by_cloud.load() == TOTAL_BYTES);
    assert(bytes_peeked_by_viewer.load() > 0);
    assert(buffer.empty());

    std::cout << "  Passed: Concurrently produced " << TOTAL_BYTES << " bytes, consumed " 
              << bytes_consumed_by_cloud.load() << " bytes by cloud uploader, and peeked " 
              << bytes_peeked_by_viewer.load() << " bytes by Web Viewer.\n";
}

void test_buffer_exhaustion_and_backpressure() {
    std::cout << "[Test 5] StreamBuffer: capacity exhaustion, empty writable span, and recovery...\n";

    constexpr size_t BUF_CAP = 64 * 1024; // 64 KB capacity
    peripheral::StreamBuffer buffer(BUF_CAP);

    assert(buffer.capacity() == BUF_CAP);
    assert(buffer.writable_bytes() == BUF_CAP);

    // 1. Fill buffer to 100% capacity
    std::vector<uint8_t> full_chunk(BUF_CAP, 0x77);
    bool ok = buffer.append(full_chunk);
    (void)ok;
    assert(ok && "Initial fill should succeed");
    assert(buffer.readable_bytes() == BUF_CAP);
    assert(buffer.writable_bytes() == 0);

    // 2. Consequence: Attempting to append more data when full MUST return false (reject data)
    std::vector<uint8_t> extra_chunk(1024, 0x88);
    bool overflow_ok = buffer.append(extra_chunk);
    (void)overflow_ok;
    assert(!overflow_ok && "Append must fail when StreamBuffer has no available capacity");

    // 3. Consequence: Direct socket ingress writable_span provider returns empty span (0 bytes)
    //    This signals StreamCapture to apply TCP backpressure (skip recv, sleep 1ms)
    auto target_span = buffer.writable_span(1024);
    (void)target_span;
    assert(target_span.empty() && "Writable span must be empty when buffer is full");
    assert(target_span.size() == 0);

    // 4. Consequence & Recovery: Downstream consumer consumes 16 KB
    buffer.consume(16 * 1024);
    assert(buffer.readable_bytes() == 48 * 1024);

    // 5. Verify writable space is now available again via automatic compaction
    auto recovered_span = buffer.writable_span(16 * 1024);
    (void)recovered_span;
    assert(recovered_span.size() >= 16 * 1024);

    // 6. Verify append now succeeds
    bool append_after_consume = buffer.append(extra_chunk);
    (void)append_after_consume;
    assert(append_after_consume && "Append must succeed once downstream consumer drains buffer");

    std::cout << "  Passed: Buffer capacity exhaustion, zero writable span backpressure, and consumer recovery verified.\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "Running peripheral::StreamBuffer Tests  \n";
    std::cout << "========================================\n";

    test_basic_rw();
    test_auto_compaction();
    test_marker_search();
    test_concurrent_producer_consumers();
    test_buffer_exhaustion_and_backpressure();

    std::cout << "All StreamBuffer tests passed successfully!\n";
    return 0;
}

