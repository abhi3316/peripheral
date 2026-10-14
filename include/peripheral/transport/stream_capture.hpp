#pragma once

#include "peripheral/camera/camera_endpoint.hpp"
#include "peripheral/transport/socket.hpp"
#include "peripheral/transport/socket_types.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace peripheral {

using WritableSpanProvider = std::function<std::span<uint8_t>(size_t min_bytes)>;
using CommitWriteCallback = std::function<void(size_t bytes_written)>;
using DataCallback = std::function<void(std::string_view stream_name, std::span<const uint8_t> data)>;
using StateCallback = std::function<void(std::string_view stream_name, SocketState state)>;

struct StreamCaptureConfig {
    SocketConfig socket_config{};
    std::chrono::milliseconds reconnect_delay{500};
    size_t read_buffer_size{64 * 1024}; // 64 KB read chunk size

    // Direct zero-copy ingress hooks:
    WritableSpanProvider writable_span_provider{nullptr};
    CommitWriteCallback on_commit_write{nullptr};
};

/**
 * @brief Manages a background capture thread for a TCP stream (e.g. video or control).
 *        Handles auto-connection, automatic reconnection on network cuts/camera reboots,
 *        and thread-safe zero-copy data dispatch to preallocated slot spans.
 */
class StreamCapture {
public:
    StreamCapture(std::string name, std::string host, uint16_t port,
                  StreamCaptureConfig config,
                  DataCallback on_data,
                  StateCallback on_state = nullptr);
    ~StreamCapture();

    StreamCapture(const StreamCapture&) = delete;
    StreamCapture& operator=(const StreamCapture&) = delete;
    StreamCapture(StreamCapture&&) = delete;
    StreamCapture& operator=(StreamCapture&&) = delete;

    // Start background capture thread
    bool start();

    // Gracefully stop background capture thread
    void stop();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::string_view host() const noexcept;
    [[nodiscard]] uint16_t port() const noexcept;
    [[nodiscard]] SocketStats get_stats() const noexcept;

    // Send data over the stream (e.g., commands / TIME_REQ on control ports)
    SocketStatus send(std::span<const uint8_t> data);

private:
    void capture_loop();

    std::string name_;
    std::string host_;
    uint16_t port_;
    StreamCaptureConfig config_;
    DataCallback data_cb_;
    StateCallback state_cb_;

    TcpSocket socket_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

/**
 * @brief Extensible API to start capturing any stream from a TCP port.
 */
std::unique_ptr<StreamCapture> capture_stream(
    std::string_view name,
    std::string_view host,
    uint16_t port,
    StreamCaptureConfig config,
    DataCallback on_data,
    StateCallback on_state = nullptr
);

} // namespace peripheral
