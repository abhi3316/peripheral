#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace peripheral {

enum class SocketState {
    Disconnected,
    Connecting,
    Connected,
    Closed,
    Error
};

enum class SocketStatus {
    Ok,
    Timeout,
    Closed,
    Error
};

struct SocketConfig {
    std::string host{"127.0.0.1"};
    uint16_t port{0};
    int rcv_buf_bytes{2 * 1024 * 1024}; // 2 MB buffer (essential for absorbing 125 Mb/s video ingress)
    int snd_buf_bytes{128 * 1024};       // 128 KB write buffer
    bool tcp_nodelay{true};              // Disable Nagle's algorithm for low latency
    bool non_blocking{true};             // Use non-blocking I/O multiplexing
    std::chrono::milliseconds connect_timeout{3000};
    std::chrono::milliseconds read_timeout{1000};
    std::chrono::milliseconds write_timeout{1000};
};

struct SocketStats {
    uint64_t bytes_received{0};
    uint64_t bytes_sent{0};
    uint64_t reconnect_count{0};
    uint64_t timeouts{0};
    uint64_t errors{0};
};

} // namespace peripheral
