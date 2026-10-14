#include "peripheral/camera/camera_session.hpp"

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

void test_camera_session_lifecycle() {
    std::cout << "[Test 1] CameraSession: lifecycle and session-level mutex synchronization...\n";

    uint16_t ctrl_port = 0;
    uint16_t vid_port = 0;
    int srv_ctrl = create_server(ctrl_port);
    int srv_vid = create_server(vid_port);

    std::atomic<bool> server_running{true};
    std::atomic<size_t> commands_received{0};

    // Mock Control Server (receives commands, sends POSE packets)
    std::thread ctrl_thread([srv_ctrl, &server_running, &commands_received]() {
        int client_fd = ::accept(srv_ctrl, nullptr, nullptr);
        if (client_fd >= 0) {
            // Read loop for incoming commands from session
            std::thread reader([client_fd, &server_running, &commands_received]() {
                uint8_t cmd_buf[32];
                while (server_running) {
                    ssize_t n = ::recv(client_fd, cmd_buf, sizeof(cmd_buf), 0);
                    if (n > 0) {
                        commands_received += static_cast<size_t>(n);
                    } else if (n <= 0) {
                        break;
                    }
                }
            });

            // Write loop sending telemetry
            uint8_t pose_pkt[16] = {'P', 'T', 'C', 'L', 1, 2, 0, 16};
            while (server_running) {
                ::send(client_fd, pose_pkt, sizeof(pose_pkt), 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }

            ::shutdown(client_fd, SHUT_RDWR);
            ::close(client_fd);
            reader.join();
        }
        ::close(srv_ctrl);
    });

    // Mock Video Server
    std::thread vid_thread([srv_vid, &server_running]() {
        int client_fd = ::accept(srv_vid, nullptr, nullptr);
        if (client_fd >= 0) {
            uint8_t frame_pkt[64] = {'V', 'F', 'R', 'M'};
            while (server_running) {
                ::send(client_fd, frame_pkt, sizeof(frame_pkt), 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            ::close(client_fd);
        }
        ::close(srv_vid);
    });

    peripheral::CameraEndpoint endpoint{
        .name = "cam0",
        .host = "127.0.0.1",
        .control_port = ctrl_port,
        .video_port = vid_port
    };

    peripheral::CameraSession session(endpoint);
    assert(!session.is_running());

    std::atomic<size_t> frames_seen{0};
    std::atomic<size_t> poses_seen{0};

    session.set_frame_handler([&frames_seen](const peripheral::VideoFrame& /*frame*/) {
        frames_seen++;
    });

    session.set_pose_handler([&poses_seen](const peripheral::PosePayload& /*pose*/) {
        poses_seen++;
    });

    assert(session.start());
    assert(session.is_running());

    // Wait until connections establish and stream
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send control commands concurrently while receiver threads are running
    std::vector<uint8_t> jog_cmd = {0x82, 0x01, 0x00, 0x00};
    for (int i = 0; i < 5; ++i) {
        auto status = session.send_control_raw(jog_cmd);
        assert(status == peripheral::SocketStatus::Ok);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server_running = false;
    session.stop();
    assert(!session.is_running());

    ctrl_thread.join();
    vid_thread.join();

    assert(commands_received.load() >= 20); // 5 commands * 4 bytes

    std::cout << "  Passed: CameraSession successfully coordinated session lifecycle and dispatched "
              << commands_received.load() << " command bytes safely.\n";
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "Running peripheral::CameraSession Tests \n";
    std::cout << "========================================\n";

    test_camera_session_lifecycle();

    std::cout << "All CameraSession tests passed successfully!\n";
    return 0;
}
