#include "peripheral/protocol/video_framer.hpp"

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

VideoFramer::VideoFramer(size_t buffer_capacity, FramePoolPartition* pool)
    : pool_partition_(pool),
      fallback_buffer_(buffer_capacity) {
    if (pool_partition_) {
        active_slot_ = pool_partition_->acquire_slot();
    }
}

void VideoFramer::set_pool_partition(FramePoolPartition* pool) {
    pool_partition_ = pool;
    if (pool_partition_ && !active_slot_) {
        active_slot_ = pool_partition_->acquire_slot();
    }
}

std::span<uint8_t> VideoFramer::writable_span(size_t min_writable_bytes) {
    if (pool_partition_) {
        if (!active_slot_) {
            active_slot_ = pool_partition_->acquire_slot();
        }
        if (active_slot_) {
            auto span = active_slot_->writable_span(min_writable_bytes);
            if (!span.empty()) {
                return span;
            }
        }
    }
    return fallback_buffer_.writable_span(min_writable_bytes);
}

void VideoFramer::commit_write(size_t bytes) {
    if (pool_partition_ && active_slot_) {
        active_slot_->commit_write(bytes);
        return;
    }
    fallback_buffer_.commit_write(bytes);
}

void VideoFramer::push_data(std::span<const uint8_t> data) {
    if (data.empty()) return;

    if (pool_partition_) {
        if (!active_slot_) {
            active_slot_ = pool_partition_->acquire_slot();
        }
        if (active_slot_) {
            auto wspan = active_slot_->writable_span();
            if (wspan.size() >= data.size()) {
                std::memcpy(wspan.data(), data.data(), data.size());
                active_slot_->commit_write(data.size());
                return;
            }
        }
    }

    if (!fallback_buffer_.append(data)) {
        fallback_buffer_.compact();
        if (!fallback_buffer_.append(data)) {
            fallback_buffer_.clear();
            fallback_buffer_.append(data);
        }
    }
}

void VideoFramer::set_frame_callback(VideoFrameCallback cb) {
    callback_ = std::move(cb);
}

std::vector<VideoFrame> VideoFramer::parse_available() {
    if (pool_partition_ && active_slot_) {
        auto frames = parse_from_slot();
        if (!frames.empty() || active_slot_->bytes_written > 0) {
            return frames;
        }
    }
    return parse_from_buffer();
}

std::vector<VideoFrame> VideoFramer::parse_from_slot() {
    std::vector<VideoFrame> output_frames;

    while (active_slot_ && active_slot_->bytes_written >= sizeof(VideoFrameHeader)) {
        // Step 1: Scan for VFRM marker (0x4D524656) to skip any garbage bytes
        const uint8_t* start = active_slot_->raw_buffer;
        size_t available = active_slot_->bytes_written;

        // Search for marker
        size_t marker_pos = 0;
        bool found_marker = false;
        for (size_t i = 0; i + 4 <= available; ++i) {
            uint32_t val = 0;
            std::memcpy(&val, start + i, 4);
            if (val == VFRM_START_MARKER) {
                marker_pos = i;
                found_marker = true;
                break;
            }
        }

        if (!found_marker) {
            // Discard leading bytes except last 3
            if (available > 3) {
                size_t to_discard = available - 3;
                std::memmove(active_slot_->raw_buffer, active_slot_->raw_buffer + to_discard, 3);
                active_slot_->bytes_written = 3;
                garbage_bytes_ += to_discard;
            }
            break;
        }

        if (marker_pos > 0) {
            // Discard leading garbage bytes up to marker
            std::memmove(active_slot_->raw_buffer, active_slot_->raw_buffer + marker_pos, available - marker_pos);
            active_slot_->bytes_written -= marker_pos;
            garbage_bytes_ += marker_pos;
            available = active_slot_->bytes_written;
        }

        // Step 2: Ensure complete header is present
        if (available < sizeof(VideoFrameHeader)) {
            break;
        }

        // Step 3: Validate header CRC-32 (bytes 0..27)
        uint32_t calc_header_crc = crc32(std::span<const uint8_t>(active_slot_->raw_buffer, 28));
        uint32_t wire_header_crc = 0;
        std::memcpy(&wire_header_crc, active_slot_->raw_buffer + 28, sizeof(uint32_t));

        if (calc_header_crc != wire_header_crc) {
            // False marker in garbage data: advance by 1 byte and resume scanning
            std::memmove(active_slot_->raw_buffer, active_slot_->raw_buffer + 1, available - 1);
            active_slot_->bytes_written -= 1;
            garbage_bytes_ += 1;
            continue;
        }

        // Step 4: Extract header fields
        VideoFrameHeader header{};
        std::memcpy(&header, active_slot_->raw_buffer, sizeof(VideoFrameHeader));

        if (header.width == 0 || header.height == 0 || header.width > 4096 || header.height > 4096) {
            std::memmove(active_slot_->raw_buffer, active_slot_->raw_buffer + 1, available - 1);
            active_slot_->bytes_written -= 1;
            garbage_bytes_ += 1;
            continue;
        }

        size_t image_bytes = static_cast<size_t>(header.width) * header.height;
        size_t total_frame_bytes = sizeof(VideoFrameHeader) + image_bytes;

        // Step 5: Check if full image payload has arrived in slot
        if (available < total_frame_bytes) {
            break; // Await remaining socket bytes
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

        // Step 7: Validate payload CRC-32 directly in slot memory
        const uint8_t* payload_ptr = active_slot_->raw_buffer + sizeof(VideoFrameHeader);
        uint32_t calc_payload_crc = crc32(std::span<const uint8_t>(payload_ptr, image_bytes));

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

        // Step 8: Extract truth stamp if present
        if (image_bytes >= sizeof(TruthStamp)) {
            std::memcpy(&frame.truth_stamp, payload_ptr, sizeof(TruthStamp));
        }

        // ZERO RUNTIME HEAP ALLOCATION:
        // Hand ownership of active_slot_ to VideoFrame!
        active_slot_->payload_offset = sizeof(VideoFrameHeader);
        active_slot_->payload_size = image_bytes;
        frame.slot = active_slot_;

        // Check for excess bytes from socket read belonging to next frame
        size_t excess = available - total_frame_bytes;
        FrameSlotHandle next_slot = pool_partition_->acquire_slot();
        if (next_slot && excess > 0) {
            std::memcpy(next_slot->raw_buffer, active_slot_->raw_buffer + total_frame_bytes, excess);
            next_slot->bytes_written = excess;
        }
        active_slot_ = std::move(next_slot);

        if (callback_) {
            callback_(frame);
        }
        output_frames.push_back(std::move(frame));
    }

    return output_frames;
}

std::vector<VideoFrame> VideoFramer::parse_from_buffer() {
    std::vector<VideoFrame> output_frames;

    while (fallback_buffer_.readable_bytes() >= sizeof(VideoFrameHeader)) {
        auto marker_offset = fallback_buffer_.find_marker(VFRM_START_MARKER);
        if (!marker_offset.has_value()) {
            size_t available = fallback_buffer_.readable_bytes();
            if (available > 3) {
                size_t to_discard = available - 3;
                fallback_buffer_.consume(to_discard);
                garbage_bytes_ += to_discard;
            }
            break;
        }

        if (marker_offset.value() > 0) {
            fallback_buffer_.consume(marker_offset.value());
            garbage_bytes_ += marker_offset.value();
        }

        if (fallback_buffer_.readable_bytes() < sizeof(VideoFrameHeader)) {
            break;
        }

        auto readable = fallback_buffer_.readable_span();
        auto header_span = readable.subspan(0, sizeof(VideoFrameHeader));

        uint32_t calc_header_crc = crc32(header_span.subspan(0, 28));
        uint32_t wire_header_crc = 0;
        std::memcpy(&wire_header_crc, header_span.data() + 28, sizeof(uint32_t));

        if (calc_header_crc != wire_header_crc) {
            fallback_buffer_.consume(1);
            garbage_bytes_ += 1;
            continue;
        }

        VideoFrameHeader header{};
        std::memcpy(&header, header_span.data(), sizeof(VideoFrameHeader));

        if (header.width == 0 || header.height == 0 || header.width > 4096 || header.height > 4096) {
            fallback_buffer_.consume(1);
            garbage_bytes_ += 1;
            continue;
        }

        size_t image_bytes = static_cast<size_t>(header.width) * header.height;
        size_t total_frame_bytes = sizeof(VideoFrameHeader) + image_bytes;

        if (fallback_buffer_.readable_bytes() < total_frame_bytes) {
            break;
        }

        if (has_active_stream_ && header.boot_id == last_boot_id_) {
            if (header.frame_seq > last_frame_seq_ + 1) {
                uint32_t missing = header.frame_seq - (last_frame_seq_ + 1);
                dropped_frames_ += missing;
            }
        }
        has_active_stream_ = true;
        last_boot_id_ = header.boot_id;
        last_frame_seq_ = header.frame_seq;

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

        if (image_bytes >= sizeof(TruthStamp)) {
            std::memcpy(&frame.truth_stamp, payload_span.data(), sizeof(TruthStamp));
        }

        // If pool partition is attached, borrow preallocated slot to eliminate heap allocation
        if (pool_partition_) {
            auto slot = pool_partition_->acquire_slot();
            if (slot) {
                std::memcpy(slot->raw_buffer, payload_span.data(), image_bytes);
                slot->payload_offset = 0;
                slot->payload_size = image_bytes;
                frame.slot = std::move(slot);
            }
        } else {
            frame.image.assign(payload_span.begin(), payload_span.end());
        }

        fallback_buffer_.consume(total_frame_bytes);

        if (callback_) {
            callback_(frame);
        }
        output_frames.push_back(std::move(frame));
    }

    return output_frames;
}

void VideoFramer::on_connection_lost() {
    if (active_slot_ && active_slot_->bytes_written >= sizeof(VideoFrameHeader)) {
        uint32_t calc_header_crc = crc32(std::span<const uint8_t>(active_slot_->raw_buffer, 28));
        uint32_t wire_header_crc = 0;
        std::memcpy(&wire_header_crc, active_slot_->raw_buffer + 28, sizeof(uint32_t));

        if (calc_header_crc == wire_header_crc) {
            VideoFrameHeader header{};
            std::memcpy(&header, active_slot_->raw_buffer, sizeof(VideoFrameHeader));

            VideoFrame cut_frame{};
            cut_frame.header = header;
            cut_frame.fate = FrameFate::Cut;
            cut_frame.rx_monotonic_us = now_monotonic_us();

            if (callback_) {
                callback_(cut_frame);
            }
        }
    } else if (fallback_buffer_.readable_bytes() >= sizeof(VideoFrameHeader)) {
        auto readable = fallback_buffer_.readable_span();
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

    if (active_slot_) {
        active_slot_->reset();
    }
    fallback_buffer_.clear();
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
    if (active_slot_) {
        active_slot_->reset();
    }
    fallback_buffer_.clear();
    has_active_stream_ = false;
    last_boot_id_ = 0;
    last_frame_seq_ = 0;
    valid_frames_ = 0;
    corrupt_frames_ = 0;
    dropped_frames_ = 0;
    garbage_bytes_ = 0;
}

} // namespace peripheral
