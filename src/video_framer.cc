#include "peripheral/video_framer.hpp"

#include <chrono>
#include <cstring>
#include <iostream>

namespace peripheral {

namespace {

uint64_t now_monotonic_us() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

} // namespace

VideoFramer::VideoFramer(size_t buffer_capacity)
    : buffer_(buffer_capacity) {}

void VideoFramer::push_data(std::span<const uint8_t> data) {
    if (!buffer_.append(data)) {
        // Buffer overflow: force compaction and discard leading bytes if necessary
        buffer_.compact();
        if (!buffer_.append(data)) {
            // Drop unparsed data to recover from extreme backpressure
            buffer_.clear();
            buffer_.append(data);
        }
    }
}

void VideoFramer::set_frame_callback(VideoFrameCallback cb) {
    callback_ = std::move(cb);
}

std::vector<VideoFrame> VideoFramer::parse_available() {
    std::vector<VideoFrame> output_frames;

    while (buffer_.readable_bytes() >= sizeof(VideoFrameHeader)) {
        // Step 1: Scan for VFRM marker (0x4D524656) to skip any garbage bytes
        auto marker_offset = buffer_.find_marker(VFRM_START_MARKER);
        if (!marker_offset.has_value()) {
            // No marker found: discard all bytes except the trailing 3 (in case marker is split)
            size_t available = buffer_.readable_bytes();
            if (available > 3) {
                size_t to_discard = available - 3;
                buffer_.consume(to_discard);
                garbage_bytes_ += to_discard;
            }
            break;
        }

        if (marker_offset.value() > 0) {
            // Discard leading garbage bytes up to the marker
            buffer_.consume(marker_offset.value());
            garbage_bytes_ += marker_offset.value();
        }

        // Step 2: Ensure we have at least 32 bytes for the header
        if (buffer_.readable_bytes() < sizeof(VideoFrameHeader)) {
            break; // Wait for remaining header bytes
        }

        auto readable = buffer_.readable_span();
        auto header_span = readable.subspan(0, sizeof(VideoFrameHeader));

        // Step 3: Validate header CRC-32 (bytes 0..27)
        uint32_t calc_header_crc = crc32(header_span.subspan(0, 28));
        uint32_t wire_header_crc = 0;
        std::memcpy(&wire_header_crc, header_span.data() + 28, sizeof(uint32_t));

        if (calc_header_crc != wire_header_crc) {
            // False positive marker inside garbage data: advance by 1 byte and resume scanning
            buffer_.consume(1);
            garbage_bytes_ += 1;
            continue;
        }

        // Step 4: Valid header confirmed! Extract header fields
        VideoFrameHeader header{};
        std::memcpy(&header, header_span.data(), sizeof(VideoFrameHeader));

        // Sanity check dimensions (e.g. up to 4096x4096)
        if (header.width == 0 || header.height == 0 || header.width > 4096 || header.height > 4096) {
            buffer_.consume(1);
            garbage_bytes_ += 1;
            continue;
        }

        size_t image_bytes = static_cast<size_t>(header.width) * header.height;
        size_t total_frame_bytes = sizeof(VideoFrameHeader) + image_bytes;

        // Step 5: Check if the full image payload has arrived
        if (buffer_.readable_bytes() < total_frame_bytes) {
            break; // Wait for remaining image pixels
        }

        // Step 6: Gap & loss accounting
        if (has_active_stream_ && header.boot_id == last_boot_id_) {
            if (header.frame_seq > last_frame_seq_ + 1) {
                uint32_t missing = header.frame_seq - (last_frame_seq_ + 1);
                dropped_frames_ += missing;
            }
        }
        has_active_stream_ = true;
        last_boot_id_ = header.boot_id;
        last_frame_seq_ = header.frame_seq;

        // Step 7: Validate payload CRC-32
        auto payload_span = readable.subspan(sizeof(VideoFrameHeader), image_bytes);
        uint32_t calc_payload_crc = crc32(payload_span);

        VideoFrame frame{};
        frame.header = header;
        frame.rx_monotonic_us = now_monotonic_us();

        if (calc_payload_crc == header.payload_crc32) {
            frame.fate = FrameFate::Sent;
            valid_frames_++;
        } else {
            frame.fate = FrameFate::Corrupt;
            corrupt_frames_++;
        }

        // Step 8: Extract 12-byte simulator truth stamp if present
        if (image_bytes >= sizeof(TruthStamp)) {
            std::memcpy(&frame.truth_stamp, payload_span.data(), sizeof(TruthStamp));
        }

        // Copy image payload
        frame.image.assign(payload_span.begin(), payload_span.end());

        // Step 9: Consume the frame from buffer
        buffer_.consume(total_frame_bytes);

        if (callback_) {
            callback_(frame);
        }
        output_frames.push_back(std::move(frame));
    }

    return output_frames;
}

void VideoFramer::on_connection_lost() {
    // If a valid header was partially read before connection dropped, emit Cut frame
    if (buffer_.readable_bytes() >= sizeof(VideoFrameHeader)) {
        auto readable = buffer_.readable_span();
        auto header_span = readable.subspan(0, sizeof(VideoFrameHeader));
        uint32_t calc_header_crc = crc32(header_span.subspan(0, 28));
        uint32_t wire_header_crc = 0;
        std::memcpy(&wire_header_crc, header_span.data() + 28, sizeof(uint32_t));

        if (calc_header_crc == wire_header_crc) {
            VideoFrameHeader header{};
            std::memcpy(&header, header_span.data(), sizeof(VideoFrameHeader));

            VideoFrame cut_frame{};
            cut_frame.header = header;
            cut_frame.fate = FrameFate::Cut;
            cut_frame.rx_monotonic_us = now_monotonic_us();

            if (callback_) {
                callback_(cut_frame);
            }
        }
    }

    buffer_.clear();
    has_active_stream_ = false;
}

uint64_t VideoFramer::valid_frames_count() const noexcept {
    return valid_frames_;
}

uint64_t VideoFramer::corrupt_frames_count() const noexcept {
    return corrupt_frames_;
}

uint64_t VideoFramer::dropped_frames_count() const noexcept {
    return dropped_frames_;
}

uint64_t VideoFramer::garbage_bytes_discarded() const noexcept {
    return garbage_bytes_;
}

void VideoFramer::reset() {
    buffer_.clear();
    has_active_stream_ = false;
    last_boot_id_ = 0;
    last_frame_seq_ = 0;
    valid_frames_ = 0;
    corrupt_frames_ = 0;
    dropped_frames_ = 0;
    garbage_bytes_ = 0;
}

} // namespace peripheral
