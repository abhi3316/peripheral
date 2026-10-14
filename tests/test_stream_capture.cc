#include "peripheral/stream_capture.hpp"

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

int create_server(uint16_t& out_port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    assert(::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(::listen(fd, 5) == 0);

    socklen_t len = sizeof(addr);
    assert(::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0);
    out_port = ntohs(addr.sin_port);
    return fd;
}

void test_capture_stream_basic() {
    std::cout << "[Test 1] capture_stream: receiving streaming data in background thread...\n";

    uint16_t port = 0;
    int server_fd = create_server(port);

    std::atomic<bool> server_done{false};
    std::thread server_thread([server_fd, &server_done]() {
        int client_fd = ::accept(server_fd, nullptr, nullptr);
        assert(client_fd >= 0);

        // Stream 10 packets of 100 bytes each
        std::vector<uint8_t> payload(100, 0x42);
        for (int i = 0; i < 10; ++i) {
            ssize_t s = ::send(client_fd, payload.data(), payload.size(), 0);
            assert(s == 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        server_done = true;
        ::close(client_fd);
        ::close(server_fd);
    });

    std::atomic<size_t> total_bytes_received{0};
    std::atomic<bool> connected_state_seen{false};

    peripheral::StreamCaptureConfig cfg;
    cfg.reconnect_delay = std::chrono::milliseconds{50};

    auto capture = peripheral::capture_stream(
        "cam0_test_stream",
        "127.0.0.1",
        port,
        cfg,
        [&total_bytes_received](std::string_view stream_name, std::span<const uint8_t> data) {
            assert(stream_name == "cam0_test_stream");
            total_bytes_received += data.size();
        },
        [&connected_state_seen](std::string_view /*stream_name*/, peripheral::SocketState state) {
            if (state == peripheral::SocketState::Connected) {
                connected_state_seen = true;
            }
        }
    );

    assert(capture != nullptr);
    assert(capture->is_running());

    // Wait until server has finished sending all packets
    server_thread.join();

    // Allow capture thread to finish reading remaining buffer
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    assert(connected_state_seen.load());
    assert(total_bytes_received.load() == 1000);

    capture->stop();
    assert(!capture->is_running());

    std::cout << "  Passed: Successfully captured " << total_bytes_received.load()
              << " bytes from background stream.\n";
}

void test_multiple_camera_capture_streams() {
    std::cout << "[Test 2] Multiple camera capture streams running concurrently...\n";

    uint16_t port1 = 0;
    uint16_t port2 = 0;
    int srv1 = create_server(port1);
    int srv2 = create_server(port2);

    std::atomic<bool> run_servers{true};

    auto server_job = [](int srv_fd, std::atomic<bool>& running, uint8_t fill_byte) {
        int client_fd = ::accept(srv_fd, nullptr, nullptr);
        if (client_fd >= 0) {
            std::vector<uint8_t> buf(64, fill_byte);
            while (running) {
                ssize_t s = ::send(client_fd, buf.data(), buf.size(), 0);
                if (s <= 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            ::close(client_fd);
        }
        ::close(srv_fd);
    };

    std::thread th1(server_job, srv1, std::ref(run_servers), 0xAA);
    std::thread th2(server_job, srv2, std::ref(run_servers), 0xBB);

    std::atomic<size_t> bytes_cam0{0};
    std::atomic<size_t> bytes_cam1{0};

    peripheral::StreamCaptureConfig cfg;

    auto cap_cam0 = peripheral::capture_stream(
        "cam0_video", "127.0.0.1", port1, cfg,
        [&bytes_cam0](std::string_view, std::span<const uint8_t> d) {
            bytes_cam0 += d.size();
        }
    );

    auto cap_cam1 = peripheral::capture_stream(
        "cam1_video", "127.0.0.1", port2, cfg,
        [&bytes_cam1](std::string_view, std::span<const uint8_t> d) {
            bytes_cam1 += d.size();
        }
    );

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    run_servers = false;
    cap_cam0->stop();
    cap_cam1->stop();

    th1.join();
    th2.join();

    assert(bytes_cam0.load() > 0);
    assert(bytes_cam1.load() > 0);

    std::cout << "  Passed: Concurrently captured cam0 (" << bytes_cam0.load()
              << " bytes) and cam1 (" << bytes_cam1.load() << " bytes).\n";
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "Running peripheral::StreamCapture Tests \n";
    std::cout << "========================================\n";

    test_capture_stream_basic();
    test_multiple_camera_capture_streams();

    std::cout << "All StreamCapture tests passed successfully!\n";
    return 0;
}
