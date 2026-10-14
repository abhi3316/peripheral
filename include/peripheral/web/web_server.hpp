#pragma once

#include "peripheral/camera/camera_session.hpp"
#include "peripheral/protocol/protocol.hpp"
#include "peripheral/protocol/video_frame.hpp"
#include "peripheral/storage/disk_recorder.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace peripheral {

class CloudUploader;

struct WebServerConfig {
    std::string host{"0.0.0.0"};
    uint16_t port{8080};
    std::string static_dir{"utils/viewer/dist"};
};

struct CachedPreview {
    std::string camera_id;
    uint16_t boot_id{1};
    uint32_t frame_seq{0};
    uint16_t width{320};
    uint16_t height{200};
    uint16_t source_width{1000};
    uint16_t source_height{625};
    uint64_t exposure_start_us{0};
    uint64_t exposure_end_us{0};
    double pan_deg{0.0};
    double tilt_deg{0.0};
    bool homed{true};
    bool recorded{false};
    std::vector<uint8_t> downsampled_pixels;
    TruthStamp truth_stamp{};
    uint64_t received_at_mono_us{0};
};

/**
 * @brief Embedded, lightweight HTTP server providing:
 *  - REST API for operator web viewer (/api/state, /api/preview, /api/command, /api/jog, /api/recording)
 *  - Static file serving of compiled React operator interface (utils/viewer/dist)
 *  - Low-overhead downsampled frame preview pipeline (320x200 greyscale)
 */
class WebServer {
public:
    explicit WebServer(WebServerConfig config = {});
    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    void register_session(CameraSession* session);
    void set_recorder(DiskRecorder* recorder) noexcept {
        recorder_ = recorder;
    }
    void set_cloud_uploader(CloudUploader* uploader) noexcept {
        uploader_ = uploader;
    }

    bool start();
    void stop();
    [[nodiscard]] bool is_running() const noexcept;

    // Called when a new frame is parsed by ingress
    void on_frame_received(std::string_view camera_name, const VideoFrame& frame);
    void on_pose_received(std::string_view camera_name, const PosePayload& pose);

private:
    void server_loop();
    void handle_client(int client_fd);

    std::string generate_state_json();
    std::string generate_preview_json(std::string_view camera_name);

    WebServerConfig config_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    int listen_fd_{-1};

    mutable std::mutex mutex_;
    std::map<std::string, CameraSession*> sessions_;
    std::map<std::string, CachedPreview> latest_previews_;
    std::map<std::string, PosePayload> latest_poses_;

    // Storage & Recording management
    DiskRecorder* recorder_{nullptr};
    CloudUploader* uploader_{nullptr};
};

} // namespace peripheral
