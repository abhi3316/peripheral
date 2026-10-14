#include "peripheral/peripheral.hpp"

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
    size_t pool_slots = 64;
    double pool_mb = -1.0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--timeout-ms" && i + 1 < argc) {
            timeout_ms = std::stoi(argv[++i]);
        } else if (arg == "--pool-slots" && i + 1 < argc) {
            pool_slots = std::stoul(argv[++i]);
        } else if (arg == "--pool-mb" && i + 1 < argc) {
            pool_mb = std::stod(argv[++i]);
        }
    }

    std::cout << "====================================================\n";
    std::cout << " Peripheral Camera Gateway - Ingress & Web Server   \n";
    std::cout << "====================================================\n";

    const auto endpoints = peripheral::CameraEndpoint::default_endpoints();

    // Configure Preallocated Slab Pool
    constexpr size_t SLOT_SIZE = 650 * 1024; // 650 KB per slot
    if (pool_mb > 0) {
        pool_slots = static_cast<size_t>((pool_mb * 1024.0 * 1024.0) / SLOT_SIZE);
    }
    if (pool_slots < endpoints.size()) {
        pool_slots = endpoints.size() * 2;
    }

    peripheral::FrameMemoryPool::Config pool_cfg;
    pool_cfg.total_slots = pool_slots;
    pool_cfg.slot_size = SLOT_SIZE;
    pool_cfg.endpoint_count = endpoints.size();
    peripheral::FrameMemoryPool frame_pool(pool_cfg);

    double total_mb = frame_pool.total_memory_mb();
    size_t slots_per_cam = frame_pool.slots_per_endpoint();
    double mb_per_cam = (slots_per_cam * SLOT_SIZE) / (1024.0 * 1024.0);
    double sec_per_cam = static_cast<double>(slots_per_cam) / 25.0; // 25 fps nominal

    std::cout << "[FramePool] Preallocated Slab Pool Initialized:\n";
    std::cout << "  Total Memory: " << total_mb << " MB (" << frame_pool.total_slots() << " slots @ 650 KB/slot)\n";
    std::cout << "  Partitioned across " << endpoints.size() << " endpoints: "
              << slots_per_cam << " slots/camera (" << mb_per_cam << " MB/camera, ~"
              << sec_per_cam << "s ring buffer)\n\n";

    // 1. Initialize WebServer
    peripheral::WebServerConfig web_cfg;
    web_cfg.port = 8080;
    web_cfg.static_dir = "utils/viewer/dist";
    peripheral::WebServer web_server(web_cfg);

    std::vector<std::unique_ptr<peripheral::CameraSession>> sessions;

    std::cout << "Initializing " << endpoints.size() << " Camera Sessions...\n";

    for (size_t ep_idx = 0; ep_idx < endpoints.size(); ++ep_idx) {
        const auto& ep = endpoints[ep_idx];
        auto* partition = frame_pool.get_partition(ep_idx);

        std::cout << "  Creating session for " << ep.name 
                  << " (Control: " << ep.host << ":" << ep.control_port
                  << ", Video: " << ep.host << ":" << ep.video_port
                  << ", Pool Slots: " << (partition ? partition->total_slots() : 0) << ")\n";

        auto session = std::make_unique<peripheral::CameraSession>(
            ep,
            peripheral::CameraSessionConfig::default_config(),
            partition
        );
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
