#pragma once

#include "camera_endpoint.hpp"
#include "socket.hpp"
#include "socket_types.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace peripheral {

struct StreamCaptureConfig {
    SocketConfig socket_config{};
    std::chrono::milliseconds reconnect_delay{500};
    size_t read_buffer_size{64 * 1024}; // 64 KB read chunk size
};

using DataCallback = std::function<void(std::string_view stream_name, std::span<const uint8_t> data)>;
using StateCallback = std::function<void(std::string_view stream_name, SocketState state)>;

/**
 * @brief Manages a background capture thread for a TCP stream (e.g. video or control).
 *        Handles auto-connection, automatic reconnection on network cuts/camera reboots,
 *        and thread-safe data dispatch to registered callbacks.
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
 *        Can be called for cam0, cam1, or any future cameras (camN) and endpoints.
 *
 * @param name Unique identifier for this stream (e.g. "cam0_video", "cam1_ctrl").
 * @param host Remote hostname or IP address (e.g. "127.0.0.1").
 * @param port TCP port to connect and stream from.
 * @param config Socket and buffer tuning options.
 * @param on_data Callback invoked when raw data chunks are received.
 * @param on_state Optional callback notifying of connection transitions.
 * @return std::unique_ptr<StreamCapture> Active stream capture handle.
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
