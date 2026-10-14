#include "peripheral/camera_session.hpp"

namespace peripheral {

CameraSession::CameraSession(CameraEndpoint endpoint, CameraSessionConfig config)
    : endpoint_(std::move(endpoint)),
      config_(std::move(config)) {
    // Wire up framer internal callbacks
    video_framer_.set_frame_callback([this](const VideoFrame& frame) {
        if (frame_handler_) {
            frame_handler_(frame);
        }
    });

    control_framer_.set_pose_callback([this](const PosePayload& pose) {
        if (pose_handler_) {
            pose_handler_(pose);
        }
    });

    control_framer_.set_status_callback([this](const StatusPayload& status) {
        if (status_handler_) {
            status_handler_(status);
        }
    });

    control_framer_.set_hello_callback([this](const HelloPayload& hello) {
        if (hello_handler_) {
            hello_handler_(hello);
        }
    });
}

CameraSession::~CameraSession() {
    stop();
}

bool CameraSession::start() {
    std::scoped_lock lock(session_mutex_);
    if (running_.exchange(true)) {
        return true;
    }

    // Reset framers for a fresh session
    video_framer_.reset();
    control_framer_.reset();

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

    return true;
}

void CameraSession::stop() {
    std::unique_ptr<StreamCapture> vid;
    std::unique_ptr<StreamCapture> ctrl;
    {
        std::scoped_lock lock(session_mutex_);
        if (!running_.exchange(false)) {
            return;
        }
        vid = std::move(video_stream_);
        ctrl = std::move(control_stream_);
    }

    if (vid) {
        vid->stop();
    }
    if (ctrl) {
        ctrl->stop();
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
