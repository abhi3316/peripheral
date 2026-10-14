#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace peripheral {

/**
 * @brief Thread-safe contiguous stream accumulator buffer.
 *
 * Supports concurrent multi-threaded usage across:
 *  - Socket Receiver thread (writing incoming stream bytes)
 *  - Web Viewer thread (peeking frames for live preview)
 *  - Cloud Uploader / Disk Storage thread (reading/consuming recorded chunks)
 *
 * Synchronization:
 *  - Internal mutex guards all pointer updates and memory movements.
 *  - Condition variable signals waiting consumers when new data is committed.
 *  - peek() allows non-destructive inspection (e.g. for Web Viewer previews).
 *  - read() provides thread-safe copy-and-consume.
 */
class StreamBuffer {
public:
    explicit StreamBuffer(size_t capacity = 2 * 1024 * 1024);
    ~StreamBuffer() = default;

    // Move-only semantics (protected by mutex)
    StreamBuffer(const StreamBuffer&) = delete;
    StreamBuffer& operator=(const StreamBuffer&) = delete;
    StreamBuffer(StreamBuffer&& other) noexcept;
    StreamBuffer& operator=(StreamBuffer&& other) noexcept;

    // --- Thread-Safe Write APIs (Socket Receiver) ---

    /**
     * @brief Appends external bytes into the buffer in a thread-safe manner,
     *        automatically compacting if necessary and notifying waiting consumers.
     * @return true if appended successfully, false if exceeds capacity.
     */
    bool append(std::span<const uint8_t> data);

    /**
     * @brief Returns a writable slice for direct socket.recv_some() writing.
     *        Caller MUST call commit_write() when done.
     *        (Note: locks internally during span acquisition).
     */
    std::span<uint8_t> writable_span(size_t min_writable_bytes = 64 * 1024);

    /**
     * @brief Commits bytes written into writable_span() and wakes up waiting consumers.
     */
    void commit_write(size_t bytes);

    // --- Thread-Safe Read & Consumer APIs (Framer, Storage, Cloud) ---

    /**
     * @brief Copies up to dest.size() unconsumed bytes into dest and consumes them.
     * @param dest Target buffer.
     * @return Number of bytes actually read.
     */
    size_t read(std::span<uint8_t> dest);

    /**
     * @brief Peeks up to dest.size() bytes starting at offset WITHOUT consuming them.
     *        Essential for the Web Viewer live preview to observe frames without
     *        removing them from the Storage/Cloud upload pipeline!
     * @param dest Target buffer to copy into.
     * @param offset Byte offset from current read_pos.
     * @return Number of bytes actually peeked.
     */
    size_t peek(std::span<uint8_t> dest, size_t offset = 0) const;

    /**
     * @brief Marks bytes as consumed/discarded from the front.
     */
    void consume(size_t bytes);

    /**
     * @brief Blocks current thread until at least min_bytes are readable or timeout expires.
     * @return true if min_bytes are ready, false on timeout.
     */
    bool wait_readable(size_t min_bytes, std::chrono::milliseconds timeout);

    /**
     * @brief Shifts all unconsumed bytes to the start of the buffer.
     */
    void compact();

    /**
     * @brief Fast thread-safe search for a 32-bit marker (e.g. 0x4D524656 'VFRM').
     * @return Byte offset from the start of current readable data, or std::nullopt.
     */
    [[nodiscard]] std::optional<size_t> find_marker(uint32_t marker_le) const noexcept;

    /**
     * @brief Returns non-owning span of currently readable bytes.
     *        (Caution: thread-confinement recommended when using raw span directly).
     */
    [[nodiscard]] std::span<const uint8_t> readable_span() const noexcept;

    // --- Thread-Safe State Queries ---
    [[nodiscard]] size_t readable_bytes() const noexcept;
    [[nodiscard]] size_t writable_bytes() const noexcept;
    [[nodiscard]] size_t capacity() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    void clear() noexcept;

private:
    std::vector<uint8_t> storage_;
    size_t capacity_{0};
    size_t read_pos_{0};
    size_t write_pos_{0};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace peripheral
