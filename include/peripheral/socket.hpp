#pragma once

#include "socket_types.hpp"

#include <atomic>
#include <chrono>
#include <span>
#include <string>
#include <string_view>

namespace peripheral {

/**
 * @brief High-performance, lean TCP socket transport primitive.
 *
 * Designed as a lightweight RAII wrapper around an OS socket descriptor.
 * Focuses purely on non-blocking I/O, socket buffer tuning (SO_RCVBUF),
 * and connection management without application-level mutex overhead.
 *
 * Thread-safety model:
 *  - Owned by its caller (e.g. dedicated receiver thread or CameraSession).
 *  - Atomic connection state and metrics allow safe external queries.
 *  - Thread coordination and command synchronization belong to the CameraSession layer.
 */
class TcpSocket {
public:
    explicit TcpSocket(SocketConfig config = {});
    ~TcpSocket();

    // Move-only semantics (RAII socket descriptor ownership)
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    // Connection lifecycle
    bool connect();
    bool connect_to(std::string_view host, uint16_t port);
    void disconnect();
    void close();

    // Status queries
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] SocketState state() const noexcept;
    [[nodiscard]] int native_handle() const noexcept;
    [[nodiscard]] const SocketConfig& config() const noexcept;
    void set_config(const SocketConfig& config);

    // Lean I/O operations (zero mutex overhead in tight loops)
    SocketStatus recv_some(std::span<uint8_t> buffer, size_t& bytes_read,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    SocketStatus recv_exact(std::span<uint8_t> buffer,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    SocketStatus send_all(std::span<const uint8_t> buffer,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    // Telemetry & metrics (atomic counters)
    [[nodiscard]] SocketStats get_stats() const noexcept;
    void reset_stats() noexcept;

private:
    int create_and_setup_socket();
    SocketStatus wait_readable(int fd, std::chrono::milliseconds timeout);
    SocketStatus wait_writable(int fd, std::chrono::milliseconds timeout);

    SocketConfig config_;
    std::atomic<SocketState> state_{SocketState::Disconnected};
    std::atomic<int> fd_{-1};

    // Atomic performance counters (zero lock contention)
    std::atomic<uint64_t> bytes_received_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> reconnect_count_{0};
    std::atomic<uint64_t> timeouts_{0};
    std::atomic<uint64_t> errors_{0};
};

} // namespace peripheral
