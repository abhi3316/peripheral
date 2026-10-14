#include "peripheral/camera_endpoint.hpp"
#include "peripheral/camera_session.hpp"
#include "peripheral/web_server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> g_running{true};

void handle_signal(int /*sig*/) {
    g_running = false;
}
} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    int timeout_ms = -1;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--timeout-ms" && i + 1 < argc) {
            timeout_ms = std::stoi(argv[++i]);
        }
    }

    std::cout << "====================================================\n";
    std::cout << " Peripheral Camera Gateway - Ingress & Web Server   \n";
    std::cout << "====================================================\n";

    // 1. Initialize WebServer
    peripheral::WebServerConfig web_cfg;
    web_cfg.port = 8080;
    web_cfg.static_dir = "utils/viewer/dist";
    peripheral::WebServer web_server(web_cfg);

    const auto endpoints = peripheral::CameraEndpoint::default_endpoints();
    std::vector<std::unique_ptr<peripheral::CameraSession>> sessions;

    std::cout << "Initializing " << endpoints.size() << " Camera Sessions...\n";

    for (const auto& ep : endpoints) {
        std::cout << "  Creating session for " << ep.name 
                  << " (Control: " << ep.host << ":" << ep.control_port
                  << ", Video: " << ep.host << ":" << ep.video_port << ")\n";

        auto session = std::make_unique<peripheral::CameraSession>(ep);
        std::string cam_name = ep.name;

        session->set_frame_handler([&web_server, cam_name](const peripheral::VideoFrame& frame) {
            web_server.on_frame_received(cam_name, frame);
        });

        session->set_pose_handler([&web_server, cam_name](const peripheral::PosePayload& pose) {
            web_server.on_pose_received(cam_name, pose);
        });

        session->set_status_handler([](const peripheral::StatusPayload& /*status*/) {
            // Ingress 2 Hz status arrives here
        });

        session->set_state_callback([](std::string_view stream_name, peripheral::SocketState state) {
            const char* state_str = (state == peripheral::SocketState::Connected) ? "Connected" :
                                    (state == peripheral::SocketState::Connecting) ? "Connecting" :
                                    (state == peripheral::SocketState::Disconnected) ? "Disconnected (Retrying)" : "Error";
            std::cout << "    [" << stream_name << "] State: " << state_str << "\n";
        });

        web_server.register_session(session.get());
        session->start();
        sessions.push_back(std::move(session));
    }

    if (!web_server.start()) {
        std::cerr << "Failed to start WebServer on port " << web_cfg.port << "\n";
        for (auto& s : sessions) s->stop();
        return 1;
    }

    std::cout << "Gateway running. Web Viewer accessible at: http://localhost:" << web_cfg.port << "\n";
    std::cout << "Press Ctrl+C to terminate.\n";

    auto start_time = std::chrono::steady_clock::now();
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (timeout_ms > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= timeout_ms) {
                break;
            }
        }
    }

    std::cout << "\nShutting down...\n";
    web_server.stop();
    for (auto& session : sessions) {
        session->stop();
    }
    std::cout << "All sessions stopped gracefully.\n";

    return 0;
}
