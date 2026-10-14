#include "peripheral/memory/stream_buffer.hpp"

#include <algorithm>
#include <cassert>

namespace peripheral {

StreamBuffer::StreamBuffer(size_t capacity)
    : capacity_(std::max<size_t>(capacity, 64 * 1024)),
      read_pos_(0),
      write_pos_(0) {
    storage_.resize(capacity_);
}

StreamBuffer::StreamBuffer(StreamBuffer&& other) noexcept {
    std::scoped_lock lock(other.mutex_);
    storage_ = std::move(other.storage_);
    capacity_ = other.capacity_;
    read_pos_ = other.read_pos_;
    write_pos_ = other.write_pos_;

    other.capacity_ = 0;
    other.read_pos_ = 0;
    other.write_pos_ = 0;
}

StreamBuffer& StreamBuffer::operator=(StreamBuffer&& other) noexcept {
    if (this == &other) return *this;

    std::scoped_lock lock(mutex_, other.mutex_);
    storage_ = std::move(other.storage_);
    capacity_ = other.capacity_;
    read_pos_ = other.read_pos_;
    write_pos_ = other.write_pos_;

    other.capacity_ = 0;
    other.read_pos_ = 0;
    other.write_pos_ = 0;

    return *this;
}

bool StreamBuffer::append(std::span<const uint8_t> data) {
    if (data.empty()) return true;

    {
        std::unique_lock lock(mutex_);
        if (capacity_ - write_pos_ < data.size()) {
            // Internal compaction
            if (read_pos_ > 0) {
                size_t unconsumed = (write_pos_ > read_pos_) ? (write_pos_ - read_pos_) : 0;
                if (unconsumed > 0) {
                    std::memmove(storage_.data(), storage_.data() + read_pos_, unconsumed);
                }
                read_pos_ = 0;
                write_pos_ = unconsumed;
            }
        }

        if (capacity_ - write_pos_ < data.size()) {
            return false; // Still exceeds capacity
        }

        std::memcpy(storage_.data() + write_pos_, data.data(), data.size());
        write_pos_ += data.size();
    }

    cv_.notify_all();
    return true;
}

std::span<uint8_t> StreamBuffer::writable_span(size_t min_writable_bytes) {
    std::scoped_lock lock(mutex_);
    if (capacity_ - write_pos_ < min_writable_bytes) {
        if (read_pos_ > 0) {
            size_t unconsumed = (write_pos_ > read_pos_) ? (write_pos_ - read_pos_) : 0;
            if (unconsumed > 0) {
                std::memmove(storage_.data(), storage_.data() + read_pos_, unconsumed);
            }
            read_pos_ = 0;
            write_pos_ = unconsumed;
        }
    }
    return std::span<uint8_t>(storage_.data() + write_pos_, capacity_ - write_pos_);
}

void StreamBuffer::commit_write(size_t bytes) {
    {
        std::scoped_lock lock(mutex_);
        write_pos_ = std::min(capacity_, write_pos_ + bytes);
    }
    cv_.notify_all();
}

size_t StreamBuffer::read(std::span<uint8_t> dest) {
    if (dest.empty()) return 0;

    std::scoped_lock lock(mutex_);
    size_t available = (write_pos_ > read_pos_) ? (write_pos_ - read_pos_) : 0;
    size_t to_read = std::min(dest.size(), available);
    if (to_read == 0) return 0;

    std::memcpy(dest.data(), storage_.data() + read_pos_, to_read);
    read_pos_ += to_read;

    if (read_pos_ >= write_pos_) {
        read_pos_ = 0;
        write_pos_ = 0;
    }

    return to_read;
}

size_t StreamBuffer::peek(std::span<uint8_t> dest, size_t offset) const {
    if (dest.empty()) return 0;

    std::scoped_lock lock(mutex_);
    size_t available = (write_pos_ > read_pos_) ? (write_pos_ - read_pos_) : 0;
    if (offset >= available) return 0;

    size_t to_peek = std::min(dest.size(), available - offset);
    std::memcpy(dest.data(), storage_.data() + read_pos_ + offset, to_peek);
    return to_peek;
}

void StreamBuffer::consume(size_t bytes) {
    std::scoped_lock lock(mutex_);
    read_pos_ += bytes;
    if (read_pos_ >= write_pos_) {
        read_pos_ = 0;
        write_pos_ = 0;
    }
}

bool StreamBuffer::wait_readable(size_t min_bytes, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] {
        return (write_pos_ > read_pos_) && ((write_pos_ - read_pos_) >= min_bytes);
    });
}

void StreamBuffer::compact() {
    std::scoped_lock lock(mutex_);
    if (read_pos_ == 0) return;

    if (read_pos_ >= write_pos_) {
        read_pos_ = 0;
        write_pos_ = 0;
        return;
    }

    size_t unconsumed = write_pos_ - read_pos_;
    std::memmove(storage_.data(), storage_.data() + read_pos_, unconsumed);
    read_pos_ = 0;
    write_pos_ = unconsumed;
}

std::optional<size_t> StreamBuffer::find_marker(uint32_t marker_le) const noexcept {
    std::scoped_lock lock(mutex_);
    size_t available = (write_pos_ > read_pos_) ? (write_pos_ - read_pos_) : 0;
    if (available < sizeof(uint32_t)) {
        return std::nullopt;
    }

    const uint8_t b0 = static_cast<uint8_t>(marker_le & 0xFF);
    const uint8_t b1 = static_cast<uint8_t>((marker_le >> 8) & 0xFF);
    const uint8_t b2 = static_cast<uint8_t>((marker_le >> 16) & 0xFF);
    const uint8_t b3 = static_cast<uint8_t>((marker_le >> 24) & 0xFF);

    const uint8_t* start = storage_.data() + read_pos_;
    const uint8_t* end = start + available - sizeof(uint32_t) + 1;
    const uint8_t* cur = start;

    while (cur < end) {
        size_t remaining = static_cast<size_t>(end - cur);
        const void* found = std::memchr(cur, b0, remaining);
        if (!found) {
            return std::nullopt;
        }

        const uint8_t* p = static_cast<const uint8_t*>(found);
        if (p[1] == b1 && p[2] == b2 && p[3] == b3) {
            return static_cast<size_t>(p - start);
        }

        cur = p + 1;
    }

    return std::nullopt;
}

std::span<const uint8_t> StreamBuffer::readable_span() const noexcept {
    std::scoped_lock lock(mutex_);
    if (write_pos_ <= read_pos_) {
        return {};
    }
    return std::span<const uint8_t>(storage_.data() + read_pos_, write_pos_ - read_pos_);
}

size_t StreamBuffer::readable_bytes() const noexcept {
    std::scoped_lock lock(mutex_);
    return (write_pos_ > read_pos_) ? (write_pos_ - read_pos_) : 0;
}

size_t StreamBuffer::writable_bytes() const noexcept {
    std::scoped_lock lock(mutex_);
    return (capacity_ > write_pos_) ? (capacity_ - write_pos_) : 0;
}

size_t StreamBuffer::capacity() const noexcept {
    std::scoped_lock lock(mutex_);
    return capacity_;
}

bool StreamBuffer::empty() const noexcept {
    return readable_bytes() == 0;
}

void StreamBuffer::clear() noexcept {
    std::scoped_lock lock(mutex_);
    read_pos_ = 0;
    write_pos_ = 0;
}

} // namespace peripheral
