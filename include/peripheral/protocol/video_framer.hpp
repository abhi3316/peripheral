#pragma once

#include "peripheral/memory/frame_pool.hpp"
#include "peripheral/memory/stream_buffer.hpp"
#include "peripheral/protocol/crc32.hpp"
#include "peripheral/protocol/protocol.hpp"
#include "peripheral/protocol/video_frame.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace peripheral {

using VideoFrameCallback = std::function<void(const VideoFrame& frame)>;

/**
 * @brief Robust Video Stream Framer for VFRM wire format.
 *
 * Supports direct zero-copy socket streaming via writable_span() into preallocated
 * FramePool slots, eliminating runtime heap allocations during ingress.
 *
 * Fault Tolerance Capabilities:
 *  - Automatic Garbage Recovery: Searches for VFRM marker (0x4D524656) across up to
 *    5,000 injected garbage bytes.
 *  - Header Integrity: Validates 32-bit header_crc32. Rejects false markers and resumes search.
 *  - Payload Integrity: Validates 32-bit payload_crc32 and classifies frame as Sent vs Corrupt.
 *  - Gap Detection: Tracks frame_seq gaps to account for frames dropped by camera.
 *  - Cut Frame Detection: Flags partially received frames when socket terminates.
 *  - Truth Stamp Extraction: Extracts embedded pan, tilt, and sequence truth values.
 */
class VideoFramer {
public:
    explicit VideoFramer(size_t buffer_capacity = 2 * 1024 * 1024, FramePoolPartition* pool = nullptr);
    ~VideoFramer() = default;

    // Attach preallocated slab pool partition for zero-allocation ingress
    void set_pool_partition(FramePoolPartition* pool);

    // Direct zero-copy ingress: Sockets stream directly into preallocated slot
    std::span<uint8_t> writable_span(size_t min_writable_bytes = 64 * 1024);
    void commit_write(size_t bytes);

    // Push new raw TCP bytes received from network into framer (legacy / test interface)
    void push_data(std::span<const uint8_t> data);

    // Parse all complete frames currently available
    std::vector<VideoFrame> parse_available();

    // Register callback for asynchronous frame dispatch
    void set_frame_callback(VideoFrameCallback cb);

    // Notify framer of connection drop (emits Cut frame if partial frame was pending)
    void on_connection_lost();

    // Metrics
    [[nodiscard]] uint64_t valid_frames_count() const noexcept;
    [[nodiscard]] uint64_t corrupt_frames_count() const noexcept;
    [[nodiscard]] uint64_t dropped_frames_count() const noexcept;
    [[nodiscard]] uint64_t garbage_bytes_discarded() const noexcept;

    void reset();

private:
    std::vector<VideoFrame> parse_from_buffer();
    std::vector<VideoFrame> parse_from_slot();

    FramePoolPartition* pool_partition_{nullptr};
    FrameSlotHandle active_slot_{nullptr};

    StreamBuffer fallback_buffer_;
    VideoFrameCallback callback_;

    bool has_active_stream_{false};
    uint16_t last_boot_id_{0};
    uint32_t last_frame_seq_{0};

    std::atomic<uint64_t> valid_frames_{0};
    std::atomic<uint64_t> corrupt_frames_{0};
    std::atomic<uint64_t> dropped_frames_{0};
    std::atomic<uint64_t> garbage_bytes_{0};
};

} // namespace peripheral
