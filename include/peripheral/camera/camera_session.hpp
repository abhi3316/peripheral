#pragma once

#include "peripheral/camera/camera_endpoint.hpp"
#include "peripheral/camera/pose_interpolator.hpp"
#include "peripheral/camera/time_sync.hpp"
#include "peripheral/memory/frame_pool.hpp"
#include "peripheral/protocol/control_framer.hpp"
#include "peripheral/protocol/protocol.hpp"
#include "peripheral/protocol/video_frame.hpp"
#include "peripheral/protocol/video_framer.hpp"
#include "peripheral/transport/socket.hpp"
#include "peripheral/transport/stream_capture.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace peripheral {

struct CameraSessionConfig {
    StreamCaptureConfig video_config{};
    StreamCaptureConfig control_config{};

    static CameraSessionConfig default_config() {
        CameraSessionConfig cfg;
        // Video: 2 MB socket receive buffer
        cfg.video_config.socket_config.rcv_buf_bytes = 2 * 1024 * 1024;
        cfg.video_config.read_buffer_size = 128 * 1024;
        // Control: TCP_NODELAY for low latency
        cfg.control_config.socket_config.tcp_nodelay = true;
        cfg.control_config.read_buffer_size = 4 * 1024;
        return cfg;
    }
};

using FrameHandler = std::function<void(const VideoFrame& frame)>;
using PoseHandler = std::function<void(const PosePayload& pose)>;
using StatusHandler = std::function<void(const StatusPayload& status)>;
using HelloHandler = std::function<void(const HelloPayload& hello)>;

/**
 * @brief Domain entity managing capture, framing, and command control for a camera rig.
 *
 * Coordinates:
 *  - Ingress Video Framer (VFRM parsing, CRC checks, garbage recovery)
 *  - Direct zero-copy streaming into preallocated FramePoolPartition slots
 *  - Ingress Control Framer (PTCL parsing, POSE, STATUS, HELLO)
 *  - Outgoing command generation (TIME_REQ, CMD jog/home)
 *  - Session-level mutex thread synchronization
 */
class CameraSession {
public:
    explicit CameraSession(CameraEndpoint endpoint,
                           CameraSessionConfig config = CameraSessionConfig::default_config(),
                           FramePoolPartition* pool_partition = nullptr);
    ~CameraSession();

    CameraSession(const CameraSession&) = delete;
    CameraSession& operator=(const CameraSession&) = delete;

    void set_pool_partition(FramePoolPartition* pool);

    // Start background receiver & framer threads
    bool start();

    // Gracefully stop all streams
    void stop();

    // Outgoing control commands
    SocketStatus send_time_req(uint64_t echo_timestamp);
    SocketStatus send_jog(float pan_speed, float tilt_speed);
    SocketStatus send_home();
    SocketStatus send_goto(float pan_deg, float tilt_deg);
    SocketStatus send_stop();
    SocketStatus send_control_raw(std::span<const uint8_t> control_msg);

    // Handlers for parsed frames and telemetry
    void set_frame_handler(FrameHandler handler);
    void set_pose_handler(PoseHandler handler);
    void set_status_handler(StatusHandler handler);
    void set_hello_handler(HelloHandler handler);
    void set_state_callback(StateCallback cb);

    // Queries
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] const CameraEndpoint& endpoint() const noexcept;
    [[nodiscard]] bool is_video_connected() const noexcept;
    [[nodiscard]] bool is_control_connected() const noexcept;
    [[nodiscard]] bool is_running() const noexcept;

    // Clock & Pose Queries
    [[nodiscard]] const TimeSyncFilter& time_filter() const noexcept { return time_filter_; }
    [[nodiscard]] const PoseInterpolator& pose_interpolator() const noexcept { return pose_interpolator_; }
    [[nodiscard]] bool is_time_synchronized() const noexcept { return time_filter_.is_synchronized(); }

    // Framer statistics
    [[nodiscard]] uint64_t valid_frames() const noexcept;
    [[nodiscard]] uint64_t corrupt_frames() const noexcept;
    [[nodiscard]] uint64_t dropped_frames() const noexcept;
    [[nodiscard]] uint64_t garbage_bytes_discarded() const noexcept;

private:
    void time_sync_loop();

    CameraEndpoint endpoint_;
    CameraSessionConfig config_;
    FramePoolPartition* pool_partition_{nullptr};

    mutable std::recursive_mutex session_mutex_;

    std::unique_ptr<StreamCapture> video_stream_;
    std::unique_ptr<StreamCapture> control_stream_;

    VideoFramer video_framer_;
    ControlFramer control_framer_;

    TimeSyncFilter time_filter_;
    PoseInterpolator pose_interpolator_;
    uint32_t exposure_us_{10000};
    uint16_t boot_id_{0};

    // 4 Hz periodic TIME_REQ synchronization worker thread
    std::thread time_sync_thread_;
    std::atomic<bool> time_sync_running_{false};
    std::condition_variable time_sync_cv_;
    std::mutex time_sync_cv_mutex_;

    FrameHandler frame_handler_;
    PoseHandler pose_handler_;
    StatusHandler status_handler_;
    HelloHandler hello_handler_;
    StateCallback on_state_;

    std::atomic<bool> running_{false};
    uint32_t cmd_seq_{0};
    uint32_t msg_seq_{0};
};

} // namespace peripheral
