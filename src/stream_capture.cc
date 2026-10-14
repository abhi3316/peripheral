#include "peripheral/stream_capture.hpp"

#include <iostream>
#include <vector>

namespace peripheral {

StreamCapture::StreamCapture(std::string name, std::string host, uint16_t port,
                             StreamCaptureConfig config,
                             DataCallback on_data,
                             StateCallback on_state)
    : name_(std::move(name)),
      host_(std::move(host)),
      port_(port),
      config_(std::move(config)),
      data_cb_(std::move(on_data)),
      state_cb_(std::move(on_state)),
      socket_(config_.socket_config) {}

StreamCapture::~StreamCapture() {
    stop();
}

bool StreamCapture::start() {
    if (running_.exchange(true)) {
        return true; // Already running
    }

    worker_ = std::thread(&StreamCapture::capture_loop, this);
    return true;
}

void StreamCapture::stop() {
    if (!running_.exchange(false)) {
        return; // Already stopped
    }

    // Unblock any in-flight recv() or poll() operations immediately
    socket_.close();

    if (worker_.joinable()) {
        worker_.join();
    }
}

bool StreamCapture::is_running() const noexcept {
    return running_.load();
}

bool StreamCapture::is_connected() const noexcept {
    return socket_.is_connected();
}

std::string_view StreamCapture::name() const noexcept {
    return name_;
}

std::string_view StreamCapture::host() const noexcept {
    return host_;
}

uint16_t StreamCapture::port() const noexcept {
    return port_;
}

SocketStats StreamCapture::get_stats() const noexcept {
    return socket_.get_stats();
}

SocketStatus StreamCapture::send(std::span<const uint8_t> data) {
    return socket_.send_all(data);
}

void StreamCapture::capture_loop() {
    std::vector<uint8_t> read_buffer(config_.read_buffer_size);

    while (running_.load()) {
        if (!socket_.is_connected()) {
            bool connected = socket_.connect_to(host_, port_);
            if (!connected) {
                if (state_cb_) {
                    state_cb_(name_, SocketState::Error);
                }

                // Sleep with interruptible check
                auto deadline = std::chrono::steady_clock::now() + config_.reconnect_delay;
                while (running_.load() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                continue;
            }

            if (state_cb_) {
                state_cb_(name_, SocketState::Connected);
            }
        }

        // Read available chunk from socket
        size_t bytes_read = 0;
        // Use a 200ms slice to ensure responsive thread termination
        auto status = socket_.recv_some(read_buffer, bytes_read, std::chrono::milliseconds(200));

        if (status == SocketStatus::Ok) {
            if (bytes_read > 0 && data_cb_) {
                data_cb_(name_, std::span<const uint8_t>(read_buffer.data(), bytes_read));
            }
        } else if (status == SocketStatus::Timeout) {
            // Read timeout: healthy keepalive iteration, check running_ flag again
            continue;
        } else if (status == SocketStatus::Closed || status == SocketStatus::Error) {
            // Connection dropped or reset
            if (state_cb_) {
                state_cb_(name_, SocketState::Disconnected);
            }
            socket_.disconnect();

            // Wait before reconnecting
            auto deadline = std::chrono::steady_clock::now() + config_.reconnect_delay;
            while (running_.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    socket_.close();
    if (state_cb_) {
        state_cb_(name_, SocketState::Closed);
    }
}

std::unique_ptr<StreamCapture> capture_stream(
    std::string_view name,
    std::string_view host,
    uint16_t port,
    StreamCaptureConfig config,
    DataCallback on_data,
    StateCallback on_state)
{
    auto capture = std::make_unique<StreamCapture>(
        std::string(name),
        std::string(host),
        port,
        std::move(config),
        std::move(on_data),
        std::move(on_state)
    );

    capture->start();
    return capture;
}

} // namespace peripheral
