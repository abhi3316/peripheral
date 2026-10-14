#include "peripheral/transport/socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace peripheral {

namespace {

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

bool configure_socket(int fd, const SocketConfig& config) {
    if (fd < 0) return false;

    // Set non-blocking mode
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }

    // Set receive buffer size (crucial for high-bitrate video ingress at 125 Mb/s)
    if (config.rcv_buf_bytes > 0) {
        int rcv_buf = config.rcv_buf_bytes;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv_buf, sizeof(rcv_buf));
    }

    // Set send buffer size
    if (config.snd_buf_bytes > 0) {
        int snd_buf = config.snd_buf_bytes;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd_buf, sizeof(snd_buf));
    }

    // Disable Nagle's algorithm for immediate packet dispatch
    if (config.tcp_nodelay) {
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    }

#ifdef SO_NOSIGPIPE
    // Prevent SIGPIPE on macOS
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    return true;
}

} // namespace

TcpSocket::TcpSocket(SocketConfig config)
    : config_(std::move(config)) {}

TcpSocket::~TcpSocket() {
    close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept {
    config_ = std::move(other.config_);
    state_.store(other.state_.load());
    int old_fd = other.fd_.exchange(-1);
    fd_.store(old_fd);
    other.state_.store(SocketState::Disconnected);

    bytes_received_.store(other.bytes_received_.load());
    bytes_sent_.store(other.bytes_sent_.load());
    reconnect_count_.store(other.reconnect_count_.load());
    timeouts_.store(other.timeouts_.load());
    errors_.store(other.errors_.load());
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this == &other) return *this;

    close();

    config_ = std::move(other.config_);
    state_.store(other.state_.load());
    int old_fd = other.fd_.exchange(-1);
    fd_.store(old_fd);
    other.state_.store(SocketState::Disconnected);

    bytes_received_.store(other.bytes_received_.load());
    bytes_sent_.store(other.bytes_sent_.load());
    reconnect_count_.store(other.reconnect_count_.load());
    timeouts_.store(other.timeouts_.load());
    errors_.store(other.errors_.load());

    return *this;
}

bool TcpSocket::connect() {
    return connect_to(config_.host, config_.port);
}

bool TcpSocket::connect_to(std::string_view host, uint16_t port) {
    disconnect();

    config_.host = std::string(host);
    config_.port = port;
    state_.store(SocketState::Connecting);

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string port_str = std::to_string(port);
    struct addrinfo* res = nullptr;

    if (getaddrinfo(config_.host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        state_.store(SocketState::Error);
        errors_++;
        return false;
    }

    int socket_fd = -1;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        socket_fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (socket_fd < 0) continue;

        if (!configure_socket(socket_fd, config_)) {
            ::close(socket_fd);
            socket_fd = -1;
            continue;
        }

        int conn_res = ::connect(socket_fd, p->ai_addr, p->ai_addrlen);
        if (conn_res == 0) {
            break;
        }

        if (errno == EINPROGRESS) {
            struct pollfd pfd{};
            pfd.fd = socket_fd;
            pfd.events = POLLOUT;

            int poll_timeout = static_cast<int>(config_.connect_timeout.count());
            int pr = ::poll(&pfd, 1, poll_timeout);

            if (pr > 0 && (pfd.revents & POLLOUT)) {
                int opt_err = 0;
                socklen_t opt_len = sizeof(opt_err);
                if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &opt_err, &opt_len) == 0 && opt_err == 0) {
                    break;
                }
            }
        }

        ::close(socket_fd);
        socket_fd = -1;
    }

    freeaddrinfo(res);

    if (socket_fd < 0) {
        state_.store(SocketState::Error);
        errors_++;
        return false;
    }

    fd_.store(socket_fd);
    state_.store(SocketState::Connected);
    reconnect_count_++;
    return true;
}

void TcpSocket::disconnect() {
    int current_fd = fd_.exchange(-1);
    if (current_fd >= 0) {
        ::shutdown(current_fd, SHUT_RDWR);
        ::close(current_fd);
    }
    state_.store(SocketState::Disconnected);
}

void TcpSocket::close() {
    disconnect();
    state_.store(SocketState::Closed);
}

bool TcpSocket::is_connected() const noexcept {
    return state_.load() == SocketState::Connected && fd_.load() >= 0;
}

SocketState TcpSocket::state() const noexcept {
    return state_.load();
}

int TcpSocket::native_handle() const noexcept {
    return fd_.load();
}

const SocketConfig& TcpSocket::config() const noexcept {
    return config_;
}

void TcpSocket::set_config(const SocketConfig& config) {
    config_ = config;
}

SocketStatus TcpSocket::wait_readable(int fd, std::chrono::milliseconds timeout) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    int timeout_ms = static_cast<int>(timeout.count());
    int pr = ::poll(&pfd, 1, timeout_ms);

    if (pr == 0) {
        timeouts_++;
        return SocketStatus::Timeout;
    }
    if (pr < 0) {
        if (errno == EINTR) return wait_readable(fd, timeout);
        errors_++;
        return SocketStatus::Error;
    }

    if (pfd.revents & POLLIN) {
        return SocketStatus::Ok;
    }

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return SocketStatus::Closed;
    }
    return SocketStatus::Ok;
}

SocketStatus TcpSocket::wait_writable(int fd, std::chrono::milliseconds timeout) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;

    int timeout_ms = static_cast<int>(timeout.count());
    int pr = ::poll(&pfd, 1, timeout_ms);

    if (pr == 0) {
        timeouts_++;
        return SocketStatus::Timeout;
    }
    if (pr < 0) {
        if (errno == EINTR) return wait_writable(fd, timeout);
        errors_++;
        return SocketStatus::Error;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return SocketStatus::Closed;
    }
    return SocketStatus::Ok;
}

SocketStatus TcpSocket::recv_some(std::span<uint8_t> buffer, size_t& bytes_read,
                                  std::chrono::milliseconds timeout) {
    bytes_read = 0;
    if (buffer.empty()) return SocketStatus::Ok;

    int current_fd = fd_.load();
    if (current_fd < 0 || state_.load() != SocketState::Connected) {
        return SocketStatus::Closed;
    }

    auto effective_timeout = (timeout.count() > 0) ? timeout : config_.read_timeout;
    SocketStatus status = wait_readable(current_fd, effective_timeout);
    if (status != SocketStatus::Ok) {
        if (status == SocketStatus::Closed) {
            state_.store(SocketState::Disconnected);
        }
        return status;
    }

    ssize_t ret = ::recv(current_fd, buffer.data(), buffer.size(), 0);
    if (ret > 0) {
        bytes_read = static_cast<size_t>(ret);
        bytes_received_ += bytes_read;
        return SocketStatus::Ok;
    }

    if (ret == 0) {
        state_.store(SocketState::Disconnected);
        return SocketStatus::Closed;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        timeouts_++;
        return SocketStatus::Timeout;
    }
    if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN) {
        state_.store(SocketState::Disconnected);
        return SocketStatus::Closed;
    }

    errors_++;
    return SocketStatus::Error;
}

SocketStatus TcpSocket::recv_exact(std::span<uint8_t> buffer,
                                  std::chrono::milliseconds timeout) {
    if (buffer.empty()) return SocketStatus::Ok;

    auto remaining_timeout = (timeout.count() > 0) ? timeout : config_.read_timeout;
    auto deadline = std::chrono::steady_clock::now() + remaining_timeout;

    size_t total_received = 0;
    while (total_received < buffer.size()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timeouts_++;
            return SocketStatus::Timeout;
        }

        auto chunk_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        size_t chunk_read = 0;
        auto chunk_span = buffer.subspan(total_received);

        SocketStatus status = recv_some(chunk_span, chunk_read, chunk_timeout);
        if (status != SocketStatus::Ok) {
            return status;
        }

        total_received += chunk_read;
    }

    return SocketStatus::Ok;
}

SocketStatus TcpSocket::send_all(std::span<const uint8_t> buffer,
                                 std::chrono::milliseconds timeout) {
    if (buffer.empty()) return SocketStatus::Ok;

    int current_fd = fd_.load();
    if (current_fd < 0 || state_.load() != SocketState::Connected) {
        return SocketStatus::Closed;
    }

    auto remaining_timeout = (timeout.count() > 0) ? timeout : config_.write_timeout;
    auto deadline = std::chrono::steady_clock::now() + remaining_timeout;

    size_t total_sent = 0;
    while (total_sent < buffer.size()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timeouts_++;
            return SocketStatus::Timeout;
        }

        auto chunk_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        SocketStatus status = wait_writable(current_fd, chunk_timeout);
        if (status != SocketStatus::Ok) {
            if (status == SocketStatus::Closed) {
                state_.store(SocketState::Disconnected);
            }
            return status;
        }

        const uint8_t* send_ptr = buffer.data() + total_sent;
        size_t send_len = buffer.size() - total_sent;

        ssize_t ret = ::send(current_fd, send_ptr, send_len, MSG_NOSIGNAL);
        if (ret > 0) {
            total_sent += static_cast<size_t>(ret);
            bytes_sent_ += static_cast<size_t>(ret);
            continue;
        }

        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (errno == ECONNRESET || errno == EPIPE) {
                state_.store(SocketState::Disconnected);
                return SocketStatus::Closed;
            }
            errors_++;
            return SocketStatus::Error;
        }
    }

    return SocketStatus::Ok;
}

SocketStats TcpSocket::get_stats() const noexcept {
    SocketStats s;
    s.bytes_received = bytes_received_.load();
    s.bytes_sent = bytes_sent_.load();
    s.reconnect_count = reconnect_count_.load();
    s.timeouts = timeouts_.load();
    s.errors = errors_.load();
    return s;
}

void TcpSocket::reset_stats() noexcept {
    bytes_received_.store(0);
    bytes_sent_.store(0);
    reconnect_count_.store(0);
    timeouts_.store(0);
    errors_.store(0);
}

} // namespace peripheral
