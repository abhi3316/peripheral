#include "peripheral/socket.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

// Spin up a simple loopback test server
int start_test_server(uint16_t& out_port) {
    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(server_fd >= 0);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // OS assigns ephemeral port

    assert(::bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(::listen(server_fd, 5) == 0);

    socklen_t len = sizeof(addr);
    assert(::getsockname(server_fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0);
    out_port = ntohs(addr.sin_port);
    return server_fd;
}

void test_unreachable_connection() {
    std::cout << "[Test 1] Connection to unreachable port...\n";
    peripheral::SocketConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 65432; // Unused port
    cfg.connect_timeout = std::chrono::milliseconds{100};

    peripheral::TcpSocket sock(cfg);
    assert(!sock.is_connected());
    bool ok = sock.connect();
    assert(!ok);
    assert(!sock.is_connected());
    assert(sock.state() == peripheral::SocketState::Error);
    std::cout << "  Passed: Handled connection failure cleanly.\n";
}

void test_loopback_transfer() {
    std::cout << "[Test 2] Loopback data transfer (recv_exact, send_all)...\n";
    uint16_t port = 0;
    int server_fd = start_test_server(port);

    std::thread server_thread([server_fd]() {
        int client_fd = ::accept(server_fd, nullptr, nullptr);
        assert(client_fd >= 0);

        // Echo server: read 16 bytes, send back 16 bytes reversed
        uint8_t buf[16];
        ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
        assert(n == 16);

        for (size_t i = 0; i < 8; ++i) {
            std::swap(buf[i], buf[15 - i]);
        }
        ssize_t sent = ::send(client_fd, buf, sizeof(buf), 0);
        assert(sent == 16);

        ::close(client_fd);
        ::close(server_fd);
    });

    peripheral::SocketConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.read_timeout = std::chrono::milliseconds{1000};
    cfg.write_timeout = std::chrono::milliseconds{1000};

    peripheral::TcpSocket client(cfg);
    assert(client.connect());
    assert(client.is_connected());

    std::vector<uint8_t> send_payload = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    auto status = client.send_all(send_payload);
    assert(status == peripheral::SocketStatus::Ok);

    std::vector<uint8_t> recv_payload(16, 0);
    status = client.recv_exact(recv_payload);
    assert(status == peripheral::SocketStatus::Ok);

    for (size_t i = 0; i < 16; ++i) {
        assert(recv_payload[i] == send_payload[15 - i]);
    }

    server_thread.join();
    client.disconnect();
    assert(!client.is_connected());
    std::cout << "  Passed: Loopback transfer verified.\n";
}

void test_thread_safe_full_duplex() {
    std::cout << "[Test 3] Thread-safe full-duplex concurrent send/recv...\n";
    uint16_t port = 0;
    int server_fd = start_test_server(port);

    std::atomic<bool> server_running{true};
    std::thread server_thread([server_fd, &server_running]() {
        int client_fd = ::accept(server_fd, nullptr, nullptr);
        assert(client_fd >= 0);

        // Discard incoming data while continuously streaming back data
        std::thread receiver([client_fd, &server_running]() {
            uint8_t dummy[1024];
            while (server_running) {
                ssize_t r = ::recv(client_fd, dummy, sizeof(dummy), 0);
                if (r <= 0) break;
            }
        });

        std::thread sender([client_fd, &server_running]() {
            uint8_t packet[32];
            std::memset(packet, 0xAB, sizeof(packet));
            while (server_running) {
                ssize_t s = ::send(client_fd, packet, sizeof(packet), 0);
                if (s <= 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        receiver.join();
        sender.join();
        ::close(client_fd);
        ::close(server_fd);
    });

    peripheral::SocketConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.read_timeout = std::chrono::milliseconds{500};
    cfg.write_timeout = std::chrono::milliseconds{500};

    peripheral::TcpSocket client(cfg);
    assert(client.connect());

    // Run client sender and client receiver on different threads simultaneously
    std::atomic<bool> stop_client{false};
    std::thread client_writer([&client, &stop_client]() {
        std::vector<uint8_t> data(64, 0x11);
        while (!stop_client) {
            auto st = client.send_all(data);
            if (st != peripheral::SocketStatus::Ok) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::thread client_reader([&client, &stop_client]() {
        std::vector<uint8_t> data(32, 0);
        while (!stop_client) {
            auto st = client.recv_exact(data);
            if (st != peripheral::SocketStatus::Ok) break;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_client = true;
    server_running = false;

    client.disconnect();

    client_writer.join();
    client_reader.join();
    server_thread.join();

    auto stats = client.get_stats();
    assert(stats.bytes_sent > 0);
    assert(stats.bytes_received > 0);
    std::cout << "  Passed: Concurrently transmitted " << stats.bytes_sent 
              << " bytes and received " << stats.bytes_received << " bytes.\n";
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "Running peripheral::TcpSocket Test Suite\n";
    std::cout << "========================================\n";

    test_unreachable_connection();
    test_loopback_transfer();
    test_thread_safe_full_duplex();

    std::cout << "All socket tests passed successfully!\n";
    return 0;
}
