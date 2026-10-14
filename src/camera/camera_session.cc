#include "peripheral/camera/camera_session.hpp"

namespace peripheral {

CameraSession::CameraSession(CameraEndpoint endpoint, CameraSessionConfig config, FramePoolPartition* pool_partition)
    : endpoint_(std::move(endpoint)),
      config_(std::move(config)),
      pool_partition_(pool_partition),
      video_framer_(2 * 1024 * 1024, pool_partition_) {
    // Wire up framer internal callbacks
    video_framer_.set_frame_callback([this](const VideoFrame& raw_frame) {
        VideoFrame frame = raw_frame;
        // Mid-exposure time calculation in camera clock µs
        uint32_t t_mid_cam = frame.header.exposure_start + (exposure_us_ / 2);

        // 1. Pose Interpolation at mid-exposure
        auto interp = pose_interpolator_.interpolate(t_mid_cam);
        if (interp.valid) {
            frame.estimated_pan_deg = interp.pan_deg;
            frame.estimated_tilt_deg = interp.tilt_deg;
            frame.pose_valid = true;
        }

        // 2. Exact Server Monotonic Time translation
        if (time_filter_.is_synchronized()) {
            frame.rx_monotonic_us = time_filter_.camera_to_server_us(t_mid_cam);
        }

        if (frame_handler_) {
            frame_handler_(frame);
        }
    });

    control_framer_.set_pose_callback([this](const PosePayload& pose) {
        pose_interpolator_.push_pose(pose);
        if (pose_handler_) {
            pose_handler_(pose);
        }
    });

    control_framer_.set_time_resp_callback([this](const TimeRespPayload& resp) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        uint64_t t4 = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        time_filter_.on_time_resp(resp.echo, resp.t2, resp.t3, t4, boot_id_);
    });

    control_framer_.set_status_callback([this](const StatusPayload& status) {
        if (status_handler_) {
            status_handler_(status);
        }
    });

    control_framer_.set_hello_callback([this](const HelloPayload& hello) {
        exposure_us_ = (hello.exposure_us > 0) ? hello.exposure_us : 10000;
        boot_id_ = hello.boot_id;
        time_filter_.on_boot_change(hello.boot_id);
        if (hello_handler_) {
            hello_handler_(hello);
        }
    });
}

CameraSession::~CameraSession() {
    stop();
}

void CameraSession::set_pool_partition(FramePoolPartition* pool) {
    std::scoped_lock lock(session_mutex_);
    pool_partition_ = pool;
    video_framer_.set_pool_partition(pool);
}

bool CameraSession::start() {
    std::scoped_lock lock(session_mutex_);
    if (running_.exchange(true)) {
        return true;
    }

    // Reset framers and filters for a fresh session
    video_framer_.reset();
    control_framer_.reset();
    pose_interpolator_.reset();
    time_filter_.reset();

    // Configure direct zero-copy ingress into preallocated slot
    config_.video_config.writable_span_provider = [this](size_t min_bytes) {
        std::scoped_lock lk(session_mutex_);
        return video_framer_.writable_span(min_bytes);
    };
    config_.video_config.on_commit_write = [this](size_t bytes) {
        std::scoped_lock lk(session_mutex_);
        video_framer_.commit_write(bytes);
        video_framer_.parse_available();
    };

    // 1. Spawn video stream receiver
    video_stream_ = capture_stream(
        endpoint_.name + "_video",
        endpoint_.host,
        endpoint_.video_port,
        config_.video_config,
        [this](std::string_view /*name*/, std::span<const uint8_t> data) {
            std::scoped_lock lk(session_mutex_);
            video_framer_.push_data(data);
            video_framer_.parse_available();
        },
        [this](std::string_view name, SocketState state) {
            if (state == SocketState::Disconnected) {
                std::scoped_lock lk(session_mutex_);
                video_framer_.on_connection_lost();
            }
            if (on_state_) {
                on_state_(name, state);
            }
        }
    );

    // 2. Spawn control stream receiver
    control_stream_ = capture_stream(
        endpoint_.name + "_control",
        endpoint_.host,
        endpoint_.control_port,
        config_.control_config,
        [this](std::string_view /*name*/, std::span<const uint8_t> data) {
            std::scoped_lock lk(session_mutex_);
            control_framer_.push_data(data);
            control_framer_.parse_available();
        },
        [this](std::string_view name, SocketState state) {
            if (state == SocketState::Disconnected) {
                std::scoped_lock lk(session_mutex_);
                control_framer_.reset();
            }
            if (on_state_) {
                on_state_(name, state);
            }
        }
    );

    // 3. Spawn 4 Hz periodic TIME_REQ synchronization worker thread
    time_sync_running_ = true;
    time_sync_thread_ = std::thread(&CameraSession::time_sync_loop, this);

    return true;
}

void CameraSession::stop() {
    std::unique_ptr<StreamCapture> vid;
    std::unique_ptr<StreamCapture> ctrl;
    std::thread sync_th;
    {
        std::scoped_lock lock(session_mutex_);
        if (!running_.exchange(false)) {
            return;
        }
        time_sync_running_ = false;
        time_sync_cv_.notify_all();
        sync_th = std::move(time_sync_thread_);
        vid = std::move(video_stream_);
        ctrl = std::move(control_stream_);
    }

    if (sync_th.joinable()) {
        sync_th.join();
    }
    if (vid) {
        vid->stop();
    }
    if (ctrl) {
        ctrl->stop();
    }
}

/**
 * =========================================================================================
 * ARCHITECTURAL NOTE ON PRODUCTION DESIGN & PTP (IEEE 1588) ALTERNATIVE:
 *
 * In an ideal production environment where cameras and host network interfaces (NICs)
 * support hardware PTP (IEEE 1588 / 802.1AS), clock synchronization is performed directly
 * at the Ethernet MAC/PHY layer using hardware timestamping and standard system daemons 
 * (e.g. linuxptp / ptp4l / phc2sys).
 *
 * Under that architecture:
 *  1. The camera's hardware clock and the host's system clock are continuously disciplined
 *     to sub-microsecond alignment.
 *  2. Camera timestamps on video frames and telemetry directly correlate to system
 *     CLOCK_REALTIME / CLOCK_MONOTONIC.
 *  3. This entire software synchronization loop (4 Hz TIME_REQ polling, offset tracking,
 *     and software filtering) would NOT be present—the server would simply read the
 *     camera timestamp directly as a valid system time.
 *
 * However, because:
 *  - The target deployment is constrained to 1 CPU core, 250 MB RAM, and userland permissions.
 *  - The camera communicates via a standard application-layer TCP socket without hardware PTP.
 *  - The camera's 32-bit clock counter drifts by up to ±100 ppm, resets on reboot, and
 *    rolls over to 0 every 71.58 minutes.
 *
 * We implement this userland software TimeSyncFilter. It applies PTP's 4-timestamp formula
 * (t1, t2, t3, t4) over TCP to compute round-trip delay, reject network jitter, handle
 * 32-bit counter unwrapping, and maintain a smooth clock offset with zero runtime heap allocations.
 * =========================================================================================
 */
void CameraSession::time_sync_loop() {
    while (time_sync_running_) {
        {
            std::unique_lock<std::mutex> lock(time_sync_cv_mutex_);
            time_sync_cv_.wait_for(lock, std::chrono::milliseconds(250), [this] {
                return !time_sync_running_.load();
            });
        }
        if (!time_sync_running_) {
            break;
        }

        // Send TIME_REQ with current CLOCK_MONOTONIC in microseconds as echo
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        uint64_t t1 = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        send_time_req(t1);
    }
}

SocketStatus CameraSession::send_time_req(uint64_t echo_timestamp) {
    std::scoped_lock lock(session_mutex_);
    if (!control_stream_ || !control_stream_->is_connected()) {
        return SocketStatus::Closed;
    }
    auto packet = ControlFramer::serialize_time_req(echo_timestamp, ++msg_seq_);
    return control_stream_->send(packet);
}

SocketStatus CameraSession::send_jog(float pan_speed, float tilt_speed) {
    std::scoped_lock lock(session_mutex_);
    if (!control_stream_ || !control_stream_->is_connected()) {
        return SocketStatus::Closed;
    }
    auto packet = ControlFramer::serialize_cmd(++cmd_seq_, CommandType::Jog, pan_speed, tilt_speed, ++msg_seq_);
    return control_stream_->send(packet);
}

SocketStatus CameraSession::send_home() {
    std::scoped_lock lock(session_mutex_);
    if (!control_stream_ || !control_stream_->is_connected()) {
        return SocketStatus::Closed;
    }
    auto packet = ControlFramer::serialize_cmd(++cmd_seq_, CommandType::Home, 0.0f, 0.0f, ++msg_seq_);
    return control_stream_->send(packet);
}

SocketStatus CameraSession::send_goto(float pan_deg, float tilt_deg) {
    std::scoped_lock lock(session_mutex_);
    if (!control_stream_ || !control_stream_->is_connected()) {
        return SocketStatus::Closed;
    }
    auto packet = ControlFramer::serialize_cmd(++cmd_seq_, CommandType::GoTo, pan_deg, tilt_deg, ++msg_seq_);
    return control_stream_->send(packet);
}

SocketStatus CameraSession::send_stop() {
    std::scoped_lock lock(session_mutex_);
    if (!control_stream_ || !control_stream_->is_connected()) {
        return SocketStatus::Closed;
    }
    auto packet = ControlFramer::serialize_cmd(++cmd_seq_, CommandType::Stop, 0.0f, 0.0f, ++msg_seq_);
    return control_stream_->send(packet);
}

SocketStatus CameraSession::send_control_raw(std::span<const uint8_t> control_msg) {
    std::scoped_lock lock(session_mutex_);
    if (!control_stream_ || !control_stream_->is_connected()) {
        return SocketStatus::Closed;
    }
    return control_stream_->send(control_msg);
}

void CameraSession::set_frame_handler(FrameHandler handler) {
    std::scoped_lock lock(session_mutex_);
    frame_handler_ = std::move(handler);
}

void CameraSession::set_pose_handler(PoseHandler handler) {
    std::scoped_lock lock(session_mutex_);
    pose_handler_ = std::move(handler);
}

void CameraSession::set_status_handler(StatusHandler handler) {
    std::scoped_lock lock(session_mutex_);
    status_handler_ = std::move(handler);
}

void CameraSession::set_hello_handler(HelloHandler handler) {
    std::scoped_lock lock(session_mutex_);
    hello_handler_ = std::move(handler);
}

void CameraSession::set_state_callback(StateCallback cb) {
    std::scoped_lock lock(session_mutex_);
    on_state_ = std::move(cb);
}

std::string_view CameraSession::name() const noexcept {
    return endpoint_.name;
}

const CameraEndpoint& CameraSession::endpoint() const noexcept {
    return endpoint_;
}

bool CameraSession::is_video_connected() const noexcept {
    std::scoped_lock lock(session_mutex_);
    return video_stream_ && video_stream_->is_connected();
}

bool CameraSession::is_control_connected() const noexcept {
    std::scoped_lock lock(session_mutex_);
    return control_stream_ && control_stream_->is_connected();
}

bool CameraSession::is_running() const noexcept {
    return running_.load();
}

uint64_t CameraSession::valid_frames() const noexcept {
    return video_framer_.valid_frames_count();
}

uint64_t CameraSession::corrupt_frames() const noexcept {
    return video_framer_.corrupt_frames_count();
}

uint64_t CameraSession::dropped_frames() const noexcept {
    return video_framer_.dropped_frames_count();
}

uint64_t CameraSession::garbage_bytes_discarded() const noexcept {
    return video_framer_.garbage_bytes_discarded();
}

} // namespace peripheral
