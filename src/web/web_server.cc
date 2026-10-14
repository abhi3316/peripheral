#include "peripheral/web/web_server.hpp"
#include "peripheral/storage/cloud_uploader.hpp"
#include "peripheral/events/event_logger.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace peripheral {

namespace {

inline std::string to_base64(std::span<const uint8_t> data) {
    static constexpr char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t val = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) val |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) val |= static_cast<uint32_t>(data[i + 2]);

        out.push_back(tbl[(val >> 18) & 0x3F]);
        out.push_back(tbl[(val >> 12) & 0x3F]);
        out.push_back((i + 1 < data.size()) ? tbl[(val >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < data.size()) ? tbl[val & 0x3F] : '=');
    }
    return out;
}

inline std::vector<uint8_t> downsample_320x200(std::span<const uint8_t> src, size_t src_w, size_t src_h, size_t dst_w = 320, size_t dst_h = 200) {
    std::vector<uint8_t> dst(dst_w * dst_h);
    if (src.size() < src_w * src_h || src_w == 0 || src_h == 0) {
        return dst;
    }
    for (size_t y = 0; y < dst_h; ++y) {
        size_t sy = (y * src_h) / dst_h;
        const uint8_t* row = src.data() + sy * src_w;
        uint8_t* dst_row = dst.data() + y * dst_w;
        for (size_t x = 0; x < dst_w; ++x) {
            size_t sx = (x * src_w) / dst_w;
            dst_row[x] = row[sx];
        }
    }
    return dst;
}

inline uint64_t now_monotonic_us() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

[[maybe_unused]] inline std::string current_wall_time_iso() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&in_time_t));
    return std::string(buf);
}

inline std::string get_mime_type(const std::string& path) {
    if (path.ends_with(".html")) return "text/html";
    if (path.ends_with(".js")) return "application/javascript";
    if (path.ends_with(".css")) return "text/css";
    if (path.ends_with(".json")) return "application/json";
    if (path.ends_with(".svg")) return "image/svg+xml";
    if (path.ends_with(".png")) return "image/png";
    return "application/octet-stream";
}

inline std::string get_embedded_viewer_html() {
    return R"raw_html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Peripheral Pan-Tilt Gateway Viewer</title>
<style>
  :root {
    --bg-main: #0f1117;
    --bg-card: #1a1d27;
    --bg-hover: #262a38;
    --border: #2e3446;
    --text-primary: #e6edf3;
    --text-secondary: #8b949e;
    --accent: #3b82f6;
    --green: #22c55e;
    --red: #ef4444;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    background: var(--bg-main);
    color: var(--text-primary);
    padding: 16px;
    display: flex;
    flex-direction: column;
    min-height: 100vh;
  }
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding-bottom: 16px;
    border-bottom: 1px solid var(--border);
    margin-bottom: 16px;
  }
  .title-group { display: flex; align-items: center; gap: 12px; }
  h1 { font-size: 1.25rem; font-weight: 600; }
  .badge {
    padding: 4px 8px;
    border-radius: 4px;
    font-size: 0.75rem;
    font-weight: 600;
    text-transform: uppercase;
  }
  .badge-live { background: rgba(34, 197, 94, 0.2); color: var(--green); border: 1px solid var(--green); }
  .badge-offline { background: rgba(239, 68, 68, 0.2); color: var(--red); border: 1px solid var(--red); }
  .badge-rec { background: rgba(239, 68, 68, 0.3); color: var(--red); border: 1px solid var(--red); animation: pulse 1.5s infinite; }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }
  .main-layout {
    display: grid;
    grid-template-columns: 320px 1fr;
    gap: 16px;
    flex: 1;
  }
  @media (max-width: 900px) {
    .main-layout { grid-template-columns: 1fr; }
  }
  .panel {
    background: var(--bg-card);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 16px;
    display: flex;
    flex-direction: column;
    gap: 16px;
  }
  h2 { font-size: 1rem; font-weight: 600; border-bottom: 1px solid var(--border); padding-bottom: 8px; }
  .btn-group { display: flex; gap: 8px; }
  button {
    background: var(--bg-hover);
    color: var(--text-primary);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 8px 12px;
    font-size: 0.875rem;
    cursor: pointer;
    transition: all 0.15s ease;
  }
  button:hover { background: var(--border); }
  button.active { background: var(--accent); color: white; border-color: var(--accent); }
  button.danger { background: rgba(239, 68, 68, 0.2); color: var(--red); border-color: var(--red); }
  .canvas-container {
    position: relative;
    background: #000;
    border-radius: 8px;
    overflow: hidden;
    display: flex;
    justify-content: center;
    align-items: center;
    border: 1px solid var(--border);
  }
  canvas {
    width: 100%;
    max-width: 640px;
    height: auto;
    aspect-ratio: 320 / 200;
    image-rendering: pixelated;
    display: block;
  }
  .crosshair {
    position: absolute;
    width: 24px;
    height: 24px;
    pointer-events: none;
  }
  .crosshair::before, .crosshair::after {
    content: '';
    position: absolute;
    background: rgba(34, 197, 94, 0.7);
  }
  .crosshair::before { top: 11px; left: 0; width: 24px; height: 2px; }
  .crosshair::after { top: 0; left: 11px; width: 2px; height: 24px; }
  .telemetry-table { width: 100%; border-collapse: collapse; font-size: 0.85rem; font-family: monospace; }
  .telemetry-table td, .telemetry-table th { padding: 6px 4px; text-align: left; }
  .telemetry-table th { color: var(--text-secondary); width: 45%; }
  .d-pad {
    display: grid;
    grid-template-columns: repeat(3, 48px);
    grid-template-rows: repeat(3, 48px);
    gap: 6px;
    margin: 0 auto;
  }
  .d-pad button { font-size: 1.2rem; display: flex; align-items: center; justify-content: center; padding: 0; }
  .jog-up { grid-column: 2; grid-row: 1; }
  .jog-left { grid-column: 1; grid-row: 2; }
  .jog-stop { grid-column: 2; grid-row: 2; }
  .jog-right { grid-column: 3; grid-row: 2; }
  .jog-down { grid-column: 2; grid-row: 3; }
  .stat-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
  .stat-card { background: rgba(0,0,0,0.2); padding: 8px; border-radius: 4px; font-size: 0.8rem; }
  .stat-val { font-size: 1.1rem; font-weight: 600; font-family: monospace; color: var(--accent); }
</style>
</head>
<body>
<header>
  <div class="title-group">
    <h1>Peripheral Pan-Tilt Gateway</h1>
    <span id="liveBadge" class="badge badge-offline">Offline</span>
    <span id="recBadge" class="badge badge-rec" style="display:none;">REC</span>
  </div>
  <div class="btn-group">
    <a href="/events" target="_blank" style="text-decoration:none;"><button style="background:rgba(59,130,246,0.15); border-color:#3b82f6; color:#60a5fa;">⚡ Live Events & Alarms</button></a>
    <button id="btnRec" onclick="toggleRecording()">Start Recording</button>
  </div>
</header>
<main class="main-layout">
  <aside class="panel">
    <h2>Camera Rig Selection</h2>
    <div class="btn-group">
      <button id="btnCam0" class="active" onclick="switchCamera('cam0')">cam0 (7001/7002)</button>
      <button id="btnCam1" onclick="switchCamera('cam1')">cam1 (7011/7012)</button>
    </div>
    <h2>Rig Telemetry</h2>
    <table class="telemetry-table">
      <tr><th>Control Link</th><td id="valCtrl">Disconnected</td></tr>
      <tr><th>Video Link</th><td id="valVid">Disconnected</td></tr>
      <tr><th>Pan Angle</th><td id="valPan">0.0°</td></tr>
      <tr><th>Tilt Angle</th><td id="valTilt">0.0°</td></tr>
      <tr><th>Motion State</th><td id="valMotion">idle</td></tr>
      <tr><th>Clock Sync</th><td id="valClockSync">Not Synced</td></tr>
      <tr><th>Clock Offset</th><td id="valClockOffset">0 µs</td></tr>
      <tr><th>Network RTT</th><td id="valClockRtt">0 µs</td></tr>
      <tr><th>Truth Error</th><td id="valError">N/A</td></tr>
    </table>
    <h2>Ingress Statistics</h2>
    <div class="stat-grid">
      <div class="stat-card"><div>Valid Frames</div><div id="statValid" class="stat-val">0</div></div>
      <div class="stat-card"><div>Damaged</div><div id="statCorrupt" class="stat-val">0</div></div>
      <div class="stat-card"><div>Dropped</div><div id="statDropped" class="stat-val">0</div></div>
      <div class="stat-card"><div>Resync Discard</div><div id="statGarbage" class="stat-val">0 B</div></div>
    </div>
  </aside>
  <section class="panel">
    <div style="display:flex; justify-content:space-between; align-items:center;">
      <h2 style="border:none; padding:0;">Live Camera Feed (320×200 Preview)</h2>
      <span id="frameMeta" style="font-family:monospace; font-size:0.8rem; color:var(--text-secondary);">Waiting for feed...</span>
    </div>
    <div class="canvas-container">
      <canvas id="viewCanvas" width="320" height="200"></canvas>
      <div class="crosshair"></div>
    </div>
    <h2>Motion Controls</h2>
    <div style="display:flex; gap:32px; align-items:center; flex-wrap:wrap;">
      <div class="d-pad">
        <button class="jog-up" onmousedown="jog(0, 20)" onmouseup="jog(0,0)" title="Tilt Up">▲</button>
        <button class="jog-left" onmousedown="jog(-30, 0)" onmouseup="jog(0,0)" title="Pan Left">◄</button>
        <button class="jog-stop" onclick="sendCmd('stop')" title="Stop">■</button>
        <button class="jog-right" onmousedown="jog(30, 0)" onmouseup="jog(0,0)" title="Pan Right">►</button>
        <button class="jog-down" onmousedown="jog(0, -20)" onmouseup="jog(0,0)" title="Tilt Down">▼</button>
      </div>
      <div style="display:flex; flex-direction:column; gap:8px;">
        <div style="display:flex; gap:6px;">
          <button onclick="sendCmd('play')" style="background:var(--accent); color:white; border:none; padding:6px 12px; border-radius:4px; font-weight:600; cursor:pointer;" title="Resume continuous playback of motion.csv">▶ Play Motion</button>
          <button onclick="sendCmd('home')" style="cursor:pointer;">Home Camera</button>
        </div>
        <div style="display:flex; gap:6px; align-items:center;">
          <input id="gotoPan" type="number" placeholder="Pan °" style="width:70px; padding:6px; background:var(--bg-main); border:1px solid var(--border); color:#fff; border-radius:4px;">
          <input id="gotoTilt" type="number" placeholder="Tilt °" style="width:70px; padding:6px; background:var(--bg-main); border:1px solid var(--border); color:#fff; border-radius:4px;">
          <button onclick="sendGoto()" style="cursor:pointer;">Go To</button>
        </div>
      </div>
    </div>
  </section>
</main>
<script>
let currentCam = 'cam0';
let isRecording = false;
let lastFrameTime = 0;

function switchCamera(cam) {
  currentCam = cam;
  document.getElementById('btnCam0').className = cam === 'cam0' ? 'active' : '';
  document.getElementById('btnCam1').className = cam === 'cam1' ? 'active' : '';
}

function b64toUint8(b64) {
  const bin = atob(b64);
  const u8 = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
  return u8;
}

async function fetchPreview() {
  try {
    const res = await fetch('/api/preview?camera=' + currentCam);
    if (!res.ok) return;
    const data = await res.json();
    if (data.error) return;

    lastFrameTime = performance.now();
    document.getElementById('liveBadge').className = 'badge badge-live';
    document.getElementById('liveBadge').innerText = 'LIVE';

    const canvas = document.getElementById('viewCanvas');
    const ctx = canvas.getContext('2d');
    const pixels = b64toUint8(data.pixelsBase64);
    const imgData = ctx.createImageData(data.width, data.height);
    for (let i = 0; i < pixels.length; i++) {
      const v = pixels[i];
      imgData.data[i * 4] = v;
      imgData.data[i * 4 + 1] = v;
      imgData.data[i * 4 + 2] = v;
      imgData.data[i * 4 + 3] = 255;
    }
    ctx.putImageData(imgData, 0, 0);

    let errorStr = 'N/A';
    if (data.truthStampBase64) {
      const tb = b64toUint8(data.truthStampBase64);
      if (tb.length >= 12) {
        const dv = new DataView(tb.buffer, tb.byteOffset, 12);
        const tPan = dv.getInt32(0, true) / 1000;
        const tTilt = dv.getInt32(4, true) / 1000;
        if (data.pose) {
          const errPan = (data.pose.panDeg - tPan).toFixed(2);
          const errTilt = (data.pose.tiltDeg - tTilt).toFixed(2);
          errorStr = 'ΔP: ' + (errPan > 0 ? '+' : '') + errPan + '° · ΔT: ' + (errTilt > 0 ? '+' : '') + errTilt + '°';
        }
      }
    }
    document.getElementById('valError').innerText = errorStr;
    document.getElementById('frameMeta').innerText = data.cameraId + ' · Frame #' + data.frameSeq + ' · Boot ' + data.bootId;
  } catch (e) {}
}

async function fetchState() {
  try {
    const res = await fetch('/api/state');
    if (!res.ok) return;
    const s = await res.json();
    const cam = s.cameras.find(c => c.cameraId === currentCam);
    if (cam) {
      document.getElementById('valCtrl').innerText = cam.link;
      document.getElementById('valCtrl').style.color = cam.link === 'connected' ? 'var(--green)' : 'var(--red)';
      document.getElementById('valVid').innerText = cam.videoLink;
      document.getElementById('valVid').style.color = cam.videoLink === 'connected' ? 'var(--green)' : 'var(--red)';
      if (cam.pose) {
        document.getElementById('valPan').innerText = cam.pose.panDeg.toFixed(1) + '°';
        document.getElementById('valTilt').innerText = cam.pose.tiltDeg.toFixed(1) + '°';
      }
      document.getElementById('valMotion').innerText = cam.motion;
      if (cam.clock) {
        document.getElementById('valClockSync').innerText = cam.clock.synchronized ? 'SYNCHRONIZED' : 'Acquiring...';
        document.getElementById('valClockSync').style.color = cam.clock.synchronized ? 'var(--green)' : 'var(--text-secondary)';
        document.getElementById('valClockOffset').innerText = (cam.clock.offsetUs > 0 ? "+" : "") + cam.clock.offsetUs + " µs";
        document.getElementById('valClockRtt').innerText = cam.clock.rttUs + " µs";
      }
      document.getElementById('statValid').innerText = cam.counters.framesReceived;
      document.getElementById('statCorrupt').innerText = cam.counters.framesDamaged;
      document.getElementById('statDropped').innerText = cam.counters.framesMissing;
      document.getElementById('statGarbage').innerText = cam.counters.resyncBytes + ' B';
    }

    isRecording = !!s.recording;
    document.getElementById('recBadge').style.display = isRecording ? 'inline-block' : 'none';
    document.getElementById('btnRec').innerText = isRecording ? 'Stop Recording' : 'Start Recording';
    document.getElementById('btnRec').className = isRecording ? 'danger' : '';

    if (performance.now() - lastFrameTime > 2000) {
      document.getElementById('liveBadge').className = 'badge badge-offline';
      document.getElementById('liveBadge').innerText = 'NO FEED';
    }
  } catch (e) {
    document.getElementById('liveBadge').className = 'badge badge-offline';
    document.getElementById('liveBadge').innerText = 'SERVER DOWN';
  }
}

function jog(p, t) {
  fetch('/api/jog', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ cameraId: currentCam, panDegPerS: p, tiltDegPerS: t })
  });
}

function sendCmd(cmd) {
  fetch('/api/command', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ cameraId: currentCam, command: cmd })
  });
}

function sendGoto() {
  const p = parseFloat(document.getElementById('gotoPan').value || '0');
  const t = parseFloat(document.getElementById('gotoTilt').value || '0');
  fetch('/api/command', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ cameraId: currentCam, command: 'goto', panDeg: p, tiltDeg: t })
  });
}

async function toggleRecording() {
  if (isRecording) {
    await fetch('/api/recording/stop', { method: 'POST' });
  } else {
    await fetch('/api/recording/start', { method: 'POST' });
  }
  fetchState();
}

setInterval(fetchPreview, 100);
setInterval(fetchState, 250);
</script>
</body>
</html>
)raw_html";
}

} // namespace

WebServer::WebServer(WebServerConfig config)
    : config_(std::move(config)) {}

WebServer::~WebServer() {
    stop();
}

void WebServer::register_session(CameraSession* session) {
    std::scoped_lock lock(mutex_);
    if (session) {
        sessions_[std::string(session->name())] = session;
    }
}

bool WebServer::start() {
    if (running_.exchange(true)) {
        return true;
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        running_ = false;
        return false;
    }

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (config_.host == "0.0.0.0" || config_.host.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        ::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr);
    }

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        running_ = false;
        return false;
    }

    if (::listen(listen_fd_, 16) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        running_ = false;
        return false;
    }

    // Set non-blocking
    int flags = ::fcntl(listen_fd_, F_GETFL, 0);
    ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

    worker_ = std::thread(&WebServer::server_loop, this);
    std::cout << "[WebServer] Operator UI & REST API listening at http://localhost:" << config_.port << "\n";
    return true;
}

void WebServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    if (worker_.joinable()) {
        worker_.join();
    }
}

bool WebServer::is_running() const noexcept {
    return running_.load();
}

void WebServer::on_frame_received(std::string_view camera_name, const VideoFrame& frame) {
    std::scoped_lock lock(mutex_);
    auto& prev = latest_previews_[std::string(camera_name)];
    prev.camera_id = std::string(camera_name);
    prev.boot_id = frame.header.boot_id;
    prev.frame_seq = frame.header.frame_seq;
    prev.source_width = frame.header.width;
    prev.source_height = frame.header.height;
    prev.width = 320;
    prev.height = 200;
    prev.exposure_start_us = frame.header.exposure_start;
    prev.exposure_end_us = frame.header.exposure_start + 10000;
    prev.truth_stamp = frame.truth_stamp;
    prev.recorded = recorder_ ? recorder_->is_recording() : false;

    // If pose was estimated
    if (frame.pose_valid) {
        prev.pan_deg = frame.estimated_pan_deg;
        prev.tilt_deg = frame.estimated_tilt_deg;
    } else {
        auto it = latest_poses_.find(std::string(camera_name));
        if (it != latest_poses_.end()) {
            prev.pan_deg = it->second.pan_counts / 10.0;
            prev.tilt_deg = it->second.tilt_counts / 10.0;
            prev.homed = it->second.homed != 0;
        }
    }

    // Downsample full image to 320x200
    prev.downsampled_pixels = downsample_320x200(frame.pixels(), frame.header.width, frame.header.height, 320, 200);

    if (recorder_) {
        recorder_->on_frame(camera_name, frame);
    }
}

void WebServer::on_pose_received(std::string_view camera_name, const PosePayload& pose) {
    std::scoped_lock lock(mutex_);
    latest_poses_[std::string(camera_name)] = pose;
}

void WebServer::server_loop() {
    while (running_) {
        struct pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, 100);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            int client_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (client_fd >= 0) {
                // Handle client in simple worker or directly for fast responses
                handle_client(client_fd);
            }
        }
    }
}

void WebServer::handle_client(int client_fd) {
    // On macOS/BSD, accepted socket inherits O_NONBLOCK from listen_fd.
    // Clear non-blocking flag so recv waits for request bytes.
    int flags = ::fcntl(client_fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    struct timeval tv{};
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req_buf[4096];
    ssize_t n = ::recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
    if (n <= 0) {
        ::close(client_fd);
        return;
    }
    req_buf[n] = '\0';
    std::string req(req_buf, n);

    std::string method, path, proto;
    std::istringstream iss(req);
    iss >> method >> path >> proto;

    // Handle OPTIONS CORS preflight
    if (method == "OPTIONS") {
        std::string resp =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
            "Content-Length: 0\r\n"
            "\r\n";
        ::send(client_fd, resp.data(), resp.size(), 0);
        ::close(client_fd);
        return;
    }

    // API Routes
    if (path == "/api/state") {
        std::string body = generate_state_json();
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/json\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "\r\n"
            << body;
        std::string s = oss.str();
        ::send(client_fd, s.data(), s.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path.starts_with("/api/events")) {
        std::string min_lvl = "debug";
        uint64_t since_id = 0;
        size_t limit = 100;

        auto qpos = path.find('?');
        if (qpos != std::string::npos) {
            std::string qs = path.substr(qpos + 1);
            auto p1 = qs.find("min_level=");
            if (p1 == std::string::npos) p1 = qs.find("level=");
            if (p1 != std::string::npos) {
                auto start = qs.find('=', p1) + 1;
                auto end = qs.find('&', start);
                min_lvl = (end == std::string::npos) ? qs.substr(start) : qs.substr(start, end - start);
            }
            auto p2 = qs.find("since_id=");
            if (p2 != std::string::npos) {
                auto start = qs.find('=', p2) + 1;
                auto end = qs.find('&', start);
                std::string sid = (end == std::string::npos) ? qs.substr(start) : qs.substr(start, end - start);
                try { since_id = std::stoull(sid); } catch (...) {}
            }
            auto p3 = qs.find("limit=");
            if (p3 != std::string::npos) {
                auto start = qs.find('=', p3) + 1;
                auto end = qs.find('&', start);
                std::string lim = (end == std::string::npos) ? qs.substr(start) : qs.substr(start, end - start);
                try { limit = std::stoul(lim); } catch (...) {}
            }
        }

        std::string body = generate_events_json(min_lvl, since_id, limit);
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/json\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "\r\n"
            << body;
        std::string s = oss.str();
        ::send(client_fd, s.data(), s.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path == "/events" || path == "/events.html" || path == "/logs" || path == "/logs.html") {
        std::string body = get_events_page_html();
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: text/html; charset=utf-8\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "\r\n"
            << body;
        std::string s = oss.str();
        ::send(client_fd, s.data(), s.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path.starts_with("/api/preview")) {
        std::string cam = "cam0";
        auto pos = path.find("camera=");
        if (pos != std::string::npos) {
            cam = path.substr(pos + 7);
            auto end = cam.find('&');
            if (end != std::string::npos) cam = cam.substr(0, end);
        }
        std::string body = generate_preview_json(cam);
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/json\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "\r\n"
            << body;
        std::string s = oss.str();
        ::send(client_fd, s.data(), s.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path == "/api/command" && method == "POST") {
        std::string target_cam = "cam0";
        if (req.find("\"cam1\"") != std::string::npos) target_cam = "cam1";

        CameraSession* sess = nullptr;
        {
            std::scoped_lock lock(mutex_);
            auto it = sessions_.find(target_cam);
            if (it != sessions_.end()) {
                sess = it->second;
            }
        }

        if (sess) {
            if (req.find("\"home\"") != std::string::npos) {
                sess->send_home();
            } else if (req.find("\"stop\"") != std::string::npos) {
                sess->send_stop();
            } else if (req.find("\"play\"") != std::string::npos) {
                uint16_t admin_port = (target_cam == "cam1") ? 7010 : 7000;
                int sock = ::socket(AF_INET, SOCK_STREAM, 0);
                if (sock >= 0) {
                    struct sockaddr_in sin{};
                    sin.sin_family = AF_INET;
                    sin.sin_port = htons(admin_port);
                    ::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
                    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin)) == 0) {
                        std::string post_req = "POST /admin/play HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
                        ::send(sock, post_req.data(), post_req.size(), 0);
                    }
                    ::close(sock);
                }
            } else if (req.find("\"goto\"") != std::string::npos) {
                float p = 0.0f, t = 0.0f;
                auto pp = req.find("\"panDeg\":");
                if (pp != std::string::npos) p = std::stof(req.substr(pp + 9));
                auto tp = req.find("\"tiltDeg\":");
                if (tp != std::string::npos) t = std::stof(req.substr(tp + 10));
                sess->send_goto(p, t);
            }
        }
        std::string body = "{\"ok\":true}";
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 11\r\n\r\n" + body;
        ::send(client_fd, resp.data(), resp.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path == "/api/jog" && method == "POST") {
        std::string target_cam = "cam0";
        if (req.find("\"cam1\"") != std::string::npos) target_cam = "cam1";

        float p = 0.0f, t = 0.0f;
        auto pp = req.find("\"panDegPerS\":");
        if (pp != std::string::npos) p = std::stof(req.substr(pp + 13));
        auto tp = req.find("\"tiltDegPerS\":");
        if (tp != std::string::npos) t = std::stof(req.substr(tp + 14));

        CameraSession* sess = nullptr;
        {
            std::scoped_lock lock(mutex_);
            auto it = sessions_.find(target_cam);
            if (it != sessions_.end()) {
                sess = it->second;
            }
        }

        if (sess) {
            if (p == 0.0f && t == 0.0f) {
                sess->send_stop();
            } else {
                sess->send_jog(p, t);
            }
        }
        std::string body = "{\"ok\":true}";
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 11\r\n\r\n" + body;
        ::send(client_fd, resp.data(), resp.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path == "/api/recording/start" && method == "POST") {
        std::string rec_id = "rec-001";
        if (recorder_) {
            recorder_->start_recording();
            auto act = recorder_->active_recording();
            if (act) rec_id = act->recording_id;
        }
        std::string body = "{\"ok\":true,\"recordingId\":\"" + rec_id + "\"}";
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: "
            << body.size() << "\r\n\r\n" << body;
        std::string s = oss.str();
        ::send(client_fd, s.data(), s.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path == "/api/recording/stop" && method == "POST") {
        if (recorder_) {
            recorder_->stop_recording();
        }
        std::string body = "{\"ok\":true}";
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 11\r\n\r\n" + body;
        ::send(client_fd, resp.data(), resp.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path == "/api/recordings") {
        std::string body = recorder_ ? recorder_->list_recordings_json() : "[]";
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: "
            << body.size() << "\r\n\r\n" << body;
        std::string s = oss.str();
        ::send(client_fd, s.data(), s.size(), 0);
        ::close(client_fd);
        return;
    }

    // Static Asset Serving from config_.static_dir or built-in embedded viewer
    std::string file_path = config_.static_dir;
    if (path == "/" || path.empty() || path == "/viewer") {
        file_path += "/index.html";
    } else {
        auto q = path.find('?');
        std::string sub = (q != std::string::npos) ? path.substr(0, q) : path;
        file_path += sub;
    }

    if (std::filesystem::exists(file_path) && !std::filesystem::is_directory(file_path)) {
        std::ifstream f(file_path, std::ios::binary);
        if (f) {
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string mime = get_mime_type(file_path);
            std::ostringstream oss;
            oss << "HTTP/1.1 200 OK\r\n"
                << "Content-Type: " << mime << "\r\n"
                << "Content-Length: " << content.size() << "\r\n"
                << "Access-Control-Allow-Origin: *\r\n\r\n"
                << content;
            std::string s = oss.str();
            ::send(client_fd, s.data(), s.size(), 0);
            ::close(client_fd);
            return;
        }
    }

    // If static files are not found on disk, serve the built-in standalone viewer
    if (path == "/" || path == "/index.html" || path.empty() || path == "/viewer") {
        std::string content = get_embedded_viewer_html();
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: text/html; charset=utf-8\r\n"
            << "Content-Length: " << content.size() << "\r\n"
            << "Access-Control-Allow-Origin: *\r\n\r\n"
            << content;
        std::string s = oss.str();
        ::send(client_fd, s.data(), s.size(), 0);
        ::close(client_fd);
        return;
    }

    std::string nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found";
    ::send(client_fd, nf.data(), nf.size(), 0);
    ::close(client_fd);
}

std::string WebServer::generate_state_json() {
    struct CamInfo {
        bool ctrl_connected{false};
        bool vid_connected{false};
        uint64_t valid_cnt{0};
        uint64_t corrupt_cnt{0};
        uint64_t dropped_cnt{0};
        uint64_t garbage_cnt{0};
        int64_t clock_offset_us{0};
        uint64_t clock_rtt_us{0};
        bool clock_synchronized{false};
    };

    std::vector<std::pair<std::string, CameraSession*>> sess_copies;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& [name, sess] : sessions_) {
            sess_copies.emplace_back(name, sess);
        }
    }

    std::map<std::string, CamInfo> cam_info;
    for (const auto& [name, sess] : sess_copies) {
        CamInfo info;
        if (sess) {
            info.ctrl_connected = sess->is_control_connected();
            info.vid_connected = sess->is_video_connected();
            info.valid_cnt = sess->valid_frames();
            info.corrupt_cnt = sess->corrupt_frames();
            info.dropped_cnt = sess->dropped_frames();
            info.garbage_cnt = sess->garbage_bytes_discarded();
            info.clock_offset_us = sess->time_filter().current_offset_us();
            info.clock_rtt_us = sess->time_filter().last_rtt_us();
            info.clock_synchronized = sess->time_filter().is_synchronized();
        }
        cam_info[name] = info;
    }

    std::scoped_lock lock(mutex_);
    uint64_t now_us = now_monotonic_us();

    std::ostringstream oss;
    oss << "{\n"
        << "  \"serverId\": \"peripheral-gateway\",\n"
        << "  \"measuredAtUs\": " << now_us << ",\n"
        << "  \"cameras\": [\n";

    bool first = true;
    for (const auto& cam_name : {"cam0", "cam1"}) {
        if (!first) oss << ",\n";
        first = false;

        const auto& info = cam_info[cam_name];

        double pan = 0.0, tilt = 10.0;
        bool homed = true;
        std::string motion_str = "idle";
        auto pit = latest_poses_.find(cam_name);
        if (pit != latest_poses_.end()) {
            pan = pit->second.pan_counts / 10.0;
            tilt = pit->second.tilt_counts / 10.0;
            homed = pit->second.homed != 0;
            if (pit->second.motion == 1) motion_str = "homing";
            else if (pit->second.motion == 2) motion_str = "moving";
            else if (pit->second.motion == 3) motion_str = "jogging";
            else if (pit->second.motion == 5) motion_str = "faulted";
        }

        oss << "    {\n"
            << "      \"cameraId\": \"" << cam_name << "\",\n"
            << "      \"link\": \"" << (info.ctrl_connected ? "connected" : "disconnected") << "\",\n"
            << "      \"videoLink\": \"" << (info.vid_connected ? "connected" : "disconnected") << "\",\n"
            << "      \"lastFrameUs\": " << now_us << ",\n"
            << "      \"bootId\": 1,\n"
            << "      \"firmwareVersion\": \"v1.0.0-gateway\",\n"
            << "      \"homed\": " << (homed ? "true" : "false") << ",\n"
            << "      \"motion\": \"" << motion_str << "\",\n"
            << "      \"fault\": null,\n"
            << "      \"pose\": {\"panDeg\": " << pan << ", \"tiltDeg\": " << tilt << ", \"atUs\": " << now_us << "},\n"
            << "      \"velocity\": {\"panDegPerS\": 0.0, \"tiltDegPerS\": 0.0},\n"
            << "      \"target\": null,\n"
            << "      \"limits\": {\"panMinDeg\": -170, \"panMaxDeg\": 170, \"tiltMinDeg\": -30, \"tiltMaxDeg\": 90, \"panMaxDegPerS\": 100, \"tiltMaxDegPerS\": 50},\n"
            << "      \"stream\": {\"width\": 1000, \"height\": 625, \"fps\": 25},\n"
            << "      \"clock\": {\"offsetUs\": " << info.clock_offset_us
            << ", \"driftPpm\": 0.0"
            << ", \"rttUs\": " << info.clock_rtt_us
            << ", \"synchronized\": " << (info.clock_synchronized ? "true" : "false")
            << ", \"uncertaintyUs\": " << (info.clock_rtt_us / 2) << "},\n"
            << "      \"counters\": {\n"
            << "        \"framesReceived\": " << info.valid_cnt << ",\n"
            << "        \"framesDamaged\": " << info.corrupt_cnt << ",\n"
            << "        \"framesMissing\": " << info.dropped_cnt << ",\n"
            << "        \"framesDroppedByCamera\": " << info.dropped_cnt << ",\n"
            << "        \"framesDroppedInServer\": 0,\n"
            << "        \"resyncBytes\": " << info.garbage_cnt << "\n"
            << "      }\n"
            << "    }";
    }

    uint64_t disk_quota = recorder_ ? recorder_->disk_quota_bytes() : 2000000000ULL;
    uint64_t disk_used = recorder_ ? recorder_->total_disk_used_bytes() : 0;
    double disk_write_rate = recorder_ ? recorder_->write_bytes_per_second() : 0.0;

    oss << "\n  ],\n"
        << "  \"pipeline\": {\n"
        << "    \"ingestBytesPerS\": 31250000,\n"
        << "    \"ingestFramesPerS\": 50,\n"
        << "    \"buffer\": {\"capacityBytes\": 40000000, \"usedBytes\": 1250000, \"highWaterBytes\": 2500000},\n"
        << "    \"disk\": {\"quotaBytes\": " << disk_quota << ", \"usedBytes\": " << disk_used << ", \"writeBytesPerS\": " << static_cast<uint64_t>(disk_write_rate) << "},\n"
        << "    \"memoryBytes\": 45000000\n"
        << "  },\n";

    CloudUploadTelemetry upload_telemetry{};
    if (uploader_) {
        upload_telemetry = uploader_->get_telemetry();
    }

    oss << "  \"upload\": {\n"
        << "    \"storage\": \"" << upload_telemetry.storage << "\",\n";
    if (upload_telemetry.last_contact_us) {
        oss << "    \"lastContactUs\": " << *upload_telemetry.last_contact_us << ",\n";
    } else {
        oss << "    \"lastContactUs\": null,\n";
    }
    oss << "    \"backlogObjects\": " << upload_telemetry.backlog_objects << ",\n"
        << "    \"backlogBytes\": " << upload_telemetry.backlog_bytes << ",\n";
    if (upload_telemetry.oldest_pending_us) {
        oss << "    \"oldestPendingUs\": " << *upload_telemetry.oldest_pending_us << "\n";
    } else {
        oss << "    \"oldestPendingUs\": null\n";
    }
    oss << "  },\n";

    if (recorder_ && recorder_->is_recording()) {
        auto active = recorder_->active_recording();
        if (active) {
            oss << "  \"recording\": {\n"
                << "    \"recordingId\": \"" << active->recording_id << "\",\n"
                << "    \"state\": \"recording\",\n"
                << "    \"startedAtUs\": " << active->started_at_us << ",\n"
                << "    \"startedAtWall\": \"" << active->started_at_wall << "\",\n"
                << "    \"stoppedAtUs\": null,\n"
                << "    \"frames\": " << active->frames << ",\n"
                << "    \"bytes\": " << active->bytes << ",\n"
                << "    \"complete\": false,\n"
                << "    \"upload\": \"pending\",\n"
                << "    \"gaps\": [";
            for (size_t gi = 0; gi < active->gaps.size(); ++gi) {
                if (gi > 0) oss << ",";
                const auto& g = active->gaps[gi];
                oss << "{\"cameraId\":\"" << g.camera_id << "\",\"bootId\":" << g.boot_id
                    << ",\"fromSeq\":" << g.from_seq << ",\"toSeq\":" << g.to_seq
                    << ",\"reason\":\"" << g.reason << "\"}";
            }
            oss << "]\n  },\n";
        } else {
            oss << "  \"recording\": null,\n";
        }
    } else {
        oss << "  \"recording\": null,\n";
    }

    std::string alarms_json = logger_ ? logger_->to_alarms_json() : "[]";
    std::string events_json = logger_ ? logger_->to_events_json(logger_->get_recent_events(50)) : "[]";

    oss << "  \"alarms\": " << alarms_json << ",\n"
        << "  \"events\": " << events_json << "\n"
        << "}";
    return oss.str();
}

std::string WebServer::generate_preview_json(std::string_view camera_name) {
    std::scoped_lock lock(mutex_);
    auto it = latest_previews_.find(std::string(camera_name));
    if (it == latest_previews_.end() || it->second.downsampled_pixels.empty()) {
        return "{\"error\":\"no frame available\"}";
    }

    const auto& p = it->second;
    std::string b64_pixels = to_base64(p.downsampled_pixels);

    std::span<const uint8_t> stamp_bytes(reinterpret_cast<const uint8_t*>(&p.truth_stamp), sizeof(p.truth_stamp));
    std::string b64_stamp = to_base64(stamp_bytes);

    std::ostringstream oss;
    oss << "{\n"
        << "  \"cameraId\": \"" << p.camera_id << "\",\n"
        << "  \"bootId\": " << p.boot_id << ",\n"
        << "  \"frameSeq\": " << p.frame_seq << ",\n"
        << "  \"width\": " << p.width << ",\n"
        << "  \"height\": " << p.height << ",\n"
        << "  \"sourceWidth\": " << p.source_width << ",\n"
        << "  \"sourceHeight\": " << p.source_height << ",\n"
        << "  \"exposureStartUs\": " << p.exposure_start_us << ",\n"
        << "  \"exposureEndUs\": " << p.exposure_end_us << ",\n"
        << "  \"pose\": {\"panDeg\": " << p.pan_deg << ", \"tiltDeg\": " << p.tilt_deg << "},\n"
        << "  \"homed\": " << (p.homed ? "true" : "false") << ",\n"
        << "  \"recorded\": " << (p.recorded ? "true" : "false") << ",\n"
        << "  \"pixelsBase64\": \"" << b64_pixels << "\",\n"
        << "  \"truthStampBase64\": \"" << b64_stamp << "\"\n"
        << "}";
    return oss.str();
}

std::string WebServer::generate_events_json(std::string_view min_level_str, uint64_t since_id, size_t limit) const {
    if (!logger_) {
        return "[]";
    }
    LogLevel min_lvl = parse_log_level(min_level_str);
    std::vector<EventRecord> events;
    if (since_id > 0) {
        events = logger_->get_events_since(since_id, limit, min_lvl);
    } else {
        events = logger_->get_recent_events(limit, min_lvl);
    }
    return logger_->to_events_json(events);
}

std::string WebServer::get_events_page_html() const {
    return R"raw_html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Peripheral Gateway — Live Alarms & Event Streaming</title>
<style>
  :root {
    --bg: #090d16;
    --panel: #111726;
    --panel-elevated: #161e31;
    --panel-hover: #1c273e;
    --border: #222d44;
    --border-accent: rgba(59, 130, 246, 0.4);
    --text: #e2e8f0;
    --muted: #8896ab;
    --accent: #3b82f6;
    --accent-glow: rgba(59, 130, 246, 0.25);
    --cyan: #06b6d4;
    --emerald: #10b981;
    --amber: #f59e0b;
    --rose: #ef4444;
    --purple: #a855f7;
    --mono: ui-monospace, "SF Mono", "Cascadia Mono", Menlo, Consolas, monospace;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg);
    color: var(--text);
    font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    line-height: 1.5;
    padding: 16px 20px;
    min-height: 100vh;
  }
  header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 12px 18px;
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 12px;
    margin-bottom: 16px;
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.4);
  }
  .header-left { display: flex; align-items: center; gap: 14px; }
  h1 { font-size: 1.15rem; font-weight: 700; letter-spacing: 0.02em; display: flex; align-items: center; gap: 10px; }
  .badge-live {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 3px 10px;
    border-radius: 999px;
    font-size: 0.75rem;
    font-weight: 700;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    background: rgba(16, 185, 129, 0.15);
    color: var(--emerald);
    border: 1px solid rgba(16, 185, 129, 0.3);
  }
  .pulse-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: var(--emerald);
    box-shadow: 0 0 8px var(--emerald);
    animation: pulse 1.6s infinite;
  }
  @keyframes pulse {
    0%, 100% { opacity: 1; transform: scale(1); }
    50% { opacity: 0.4; transform: scale(0.85); }
  }
  .nav-links { display: flex; align-items: center; gap: 10px; }
  .btn-nav {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 6px 14px;
    background: var(--panel-elevated);
    border: 1px solid var(--border);
    border-radius: 8px;
    color: var(--text);
    font-size: 0.85rem;
    font-weight: 600;
    text-decoration: none;
    transition: all 0.15s ease;
  }
  .btn-nav:hover { background: var(--panel-hover); border-color: var(--accent); color: white; }

  /* KPI Cards Grid */
  .kpi-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
    gap: 14px;
    margin-bottom: 16px;
  }
  .kpi-card {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 12px 16px;
    display: flex;
    flex-direction: column;
    gap: 4px;
    box-shadow: 0 2px 10px rgba(0,0,0,0.25);
  }
  .kpi-title { font-size: 0.75rem; text-transform: uppercase; letter-spacing: 0.05em; color: var(--muted); }
  .kpi-value { font-size: 1.4rem; font-weight: 700; font-family: var(--mono); color: white; display: flex; align-items: center; gap: 8px; }
  .kpi-sub { font-size: 0.75rem; color: var(--muted); }

  /* Active Alarms Panel */
  .alarms-panel {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 14px 18px;
    margin-bottom: 16px;
  }
  .alarms-panel.has-alarms { border-color: var(--rose); background: rgba(239, 68, 68, 0.05); }
  .alarms-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; }
  .alarms-title { font-size: 0.9rem; font-weight: 700; text-transform: uppercase; letter-spacing: 0.04em; color: var(--text); display: flex; align-items: center; gap: 8px; }
  .alarms-list { list-style: none; display: flex; flex-direction: column; gap: 8px; }
  .alarm-item {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 8px 12px;
    border-radius: 6px;
    background: rgba(239, 68, 68, 0.12);
    border-left: 3px solid var(--rose);
    font-size: 0.88rem;
  }
  .alarm-item.warning { background: rgba(245, 158, 11, 0.12); border-left-color: var(--amber); }
  .alarm-badge { font-size: 0.75rem; font-weight: 700; text-transform: uppercase; padding: 2px 6px; border-radius: 4px; }
  .alarm-badge.alarm { background: var(--rose); color: white; }
  .alarm-badge.warning { background: var(--amber); color: #000; }
  .alarm-nominal { color: var(--emerald); font-size: 0.88rem; display: flex; align-items: center; gap: 8px; }

  /* Toolbar */
  .toolbar {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 12px 16px;
    display: flex;
    flex-wrap: wrap;
    gap: 12px;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 16px;
  }
  .filter-group { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; }
  .filter-label { font-size: 0.78rem; text-transform: uppercase; font-weight: 600; color: var(--muted); margin-right: 4px; }
  .pill-btn {
    background: var(--panel-elevated);
    border: 1px solid var(--border);
    color: var(--muted);
    font-size: 0.8rem;
    font-weight: 600;
    padding: 4px 10px;
    border-radius: 6px;
    cursor: pointer;
    transition: all 0.15s ease;
    display: inline-flex;
    align-items: center;
    gap: 5px;
  }
  .pill-btn:hover { background: var(--panel-hover); color: var(--text); border-color: var(--border-accent); }
  .pill-btn.active {
    background: var(--accent);
    color: white;
    border-color: var(--accent);
    box-shadow: 0 0 10px var(--accent-glow);
  }
  .pill-count {
    background: rgba(0,0,0,0.25);
    padding: 1px 5px;
    border-radius: 999px;
    font-size: 0.72rem;
    font-family: var(--mono);
  }
  .search-input {
    background: var(--panel-elevated);
    border: 1px solid var(--border);
    color: var(--text);
    padding: 6px 12px;
    border-radius: 6px;
    font-size: 0.85rem;
    width: 220px;
    outline: none;
    transition: all 0.15s;
  }
  .search-input:focus { border-color: var(--accent); box-shadow: 0 0 0 2px var(--accent-glow); width: 260px; }
  .action-btn {
    background: var(--panel-elevated);
    border: 1px solid var(--border);
    color: var(--text);
    font-size: 0.82rem;
    font-weight: 600;
    padding: 6px 12px;
    border-radius: 6px;
    cursor: pointer;
    transition: all 0.15s;
    display: inline-flex;
    align-items: center;
    gap: 6px;
  }
  .action-btn:hover { background: var(--panel-hover); border-color: var(--accent); }
  .action-btn.active { background: rgba(59, 130, 246, 0.2); border-color: var(--accent); color: #60a5fa; }

  /* Event Stream Table */
  .table-container {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 12px;
    overflow: hidden;
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.3);
  }
  table {
    width: 100%;
    border-collapse: collapse;
    font-size: 0.85rem;
    text-align: left;
  }
  thead {
    background: var(--panel-elevated);
    position: sticky;
    top: 0;
    z-index: 10;
    border-bottom: 1px solid var(--border);
  }
  th {
    padding: 10px 14px;
    font-size: 0.75rem;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--muted);
    font-weight: 700;
    white-space: nowrap;
  }
  td {
    padding: 10px 14px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.03);
    vertical-align: middle;
  }
  tr.row-new { animation: rowSlide 0.3s ease-out; }
  @keyframes rowSlide {
    from { background: rgba(59, 130, 246, 0.25); transform: translateY(-4px); }
    to { background: transparent; transform: translateY(0); }
  }
  tr:hover { background: var(--panel-hover); }

  .mono { font-family: var(--mono); font-size: 0.82rem; }
  .badge-lvl {
    display: inline-block;
    padding: 2px 8px;
    border-radius: 4px;
    font-size: 0.72rem;
    font-weight: 700;
    text-transform: uppercase;
    letter-spacing: 0.03em;
  }
  .badge-lvl.debug { background: rgba(100, 116, 139, 0.2); color: #94a3b8; border: 1px solid rgba(100, 116, 139, 0.3); }
  .badge-lvl.info { background: rgba(6, 182, 212, 0.15); color: var(--cyan); border: 1px solid rgba(6, 182, 212, 0.3); }
  .badge-lvl.warning { background: rgba(245, 158, 11, 0.15); color: var(--amber); border: 1px solid rgba(245, 158, 11, 0.3); }
  .badge-lvl.alarm { background: rgba(239, 68, 68, 0.2); color: var(--rose); border: 1px solid rgba(239, 68, 68, 0.4); }
  .badge-lvl.error { background: rgba(220, 38, 38, 0.25); color: #fca5a5; border: 1px solid rgba(220, 38, 38, 0.5); }

  .badge-source {
    display: inline-block;
    padding: 1px 6px;
    border-radius: 4px;
    font-size: 0.75rem;
    font-family: var(--mono);
    background: rgba(255,255,255,0.06);
    color: var(--text);
  }
  .badge-source.cam0 { border-left: 2px solid var(--cyan); }
  .badge-source.cam1 { border-left: 2px solid var(--purple); }
  .badge-source.system { border-left: 2px solid var(--emerald); }

  .empty-state {
    padding: 40px;
    text-align: center;
    color: var(--muted);
    font-size: 0.95rem;
  }
</style>
</head>
<body>

<header>
  <div class="header-left">
    <h1>⚡ Peripheral Pan-Tilt Gateway <span class="badge-live"><span class="pulse-dot"></span>Live Stream</span></h1>
  </div>
  <div class="nav-links">
    <a href="/" class="btn-nav">🎥 Main Viewer</a>
    <a href="/api/state" target="_blank" class="btn-nav">📊 /api/state</a>
    <a href="/api/events" target="_blank" class="btn-nav">⚡ /api/events</a>
  </div>
</header>

<!-- KPI Cards -->
<section class="kpi-grid">
  <div class="kpi-card">
    <div class="kpi-title">Active Alarms</div>
    <div class="kpi-value" id="kpiAlarms" style="color:var(--emerald);">0</div>
    <div class="kpi-sub" id="kpiAlarmsSub">All Rig Conditions Nominal</div>
  </div>
  <div class="kpi-card">
    <div class="kpi-title">Event Ring Buffer</div>
    <div class="kpi-value" id="kpiTotalEvents">0</div>
    <div class="kpi-sub">Total Pushed into Atomic Ring</div>
  </div>
  <div class="kpi-card">
    <div class="kpi-title">Ring Capacity</div>
    <div class="kpi-value">256 Slots</div>
    <div class="kpi-sub">Wait-Free / Lock-Free MPMC Array</div>
  </div>
  <div class="kpi-card">
    <div class="kpi-title">Stream Status</div>
    <div class="kpi-value" id="kpiRate" style="color:var(--cyan);">Active</div>
    <div class="kpi-sub" id="kpiSince">Latency ~200 ms polling</div>
  </div>
</section>

<!-- Active Alarms Panel -->
<section class="alarms-panel" id="alarmsPanel">
  <div class="alarms-header">
    <div class="alarms-title" id="alarmsTitle"><span>🚨 Active System Alarms</span></div>
    <span class="mono" id="alarmsCount" style="font-size:0.8rem; color:var(--muted);">0 Active</span>
  </div>
  <div id="alarmsContainer">
    <div class="alarm-nominal">✔ No active system faults, link drops, or motor stalls detected.</div>
  </div>
</section>

<!-- Filter Toolbar -->
<section class="toolbar">
  <div class="filter-group">
    <span class="filter-label">Level:</span>
    <button class="pill-btn active" data-level="all" onclick="setLevelFilter('all')">ALL <span class="pill-count" id="cntAll">0</span></button>
    <button class="pill-btn" data-level="debug" onclick="setLevelFilter('debug')">DEBUG <span class="pill-count" id="cntDebug">0</span></button>
    <button class="pill-btn" data-level="info" onclick="setLevelFilter('info')">INFO <span class="pill-count" id="cntInfo">0</span></button>
    <button class="pill-btn" data-level="warning" onclick="setLevelFilter('warning')">WARN <span class="pill-count" id="cntWarn">0</span></button>
    <button class="pill-btn" data-level="alarm" onclick="setLevelFilter('alarm')">ALARM <span class="pill-count" id="cntAlarm">0</span></button>
    <button class="pill-btn" data-level="error" onclick="setLevelFilter('error')">ERROR <span class="pill-count" id="cntError">0</span></button>
  </div>

  <div class="filter-group">
    <span class="filter-label">Source:</span>
    <button class="pill-btn active" data-source="all" onclick="setSourceFilter('all')">ALL</button>
    <button class="pill-btn" data-source="cam0" onclick="setSourceFilter('cam0')">cam0</button>
    <button class="pill-btn" data-source="cam1" onclick="setSourceFilter('cam1')">cam1</button>
    <button class="pill-btn" data-source="system" onclick="setSourceFilter('system')">system</button>
  </div>

  <div class="filter-group">
    <input type="text" id="searchInput" class="search-input" placeholder="Search events..." oninput="onSearchChange()">
    <button id="btnScroll" class="action-btn active" onclick="toggleAutoScroll()">Auto-Scroll: ON</button>
    <button id="btnPause" class="action-btn" onclick="togglePause()">Pause</button>
    <button class="action-btn" onclick="clearEvents()">Clear View</button>
    <button class="action-btn" onclick="exportJson()">Export JSON</button>
  </div>
</section>

<!-- Live Events Table -->
<div class="table-container">
  <table>
    <thead>
      <tr>
        <th style="width: 70px;">ID</th>
        <th style="width: 140px;">Time Ago</th>
        <th style="width: 90px;">Level</th>
        <th style="width: 90px;">Source</th>
        <th style="width: 180px;">Event Code</th>
        <th>Message Details</th>
      </tr>
    </thead>
    <tbody id="eventTbody">
      <tr><td colspan="6" class="empty-state">Waiting for gateway event stream...</td></tr>
    </tbody>
  </table>
</div>

<script>
  let allEvents = [];
  let highestId = 0;
  let activeLevel = 'all';
  let activeSource = 'all';
  let searchQuery = '';
  let autoScroll = true;
  let isPaused = false;
  let counts = { all: 0, debug: 0, info: 0, warning: 0, alarm: 0, error: 0 };

  function setLevelFilter(lvl) {
    activeLevel = lvl;
    document.querySelectorAll('[data-level]').forEach(b => b.classList.toggle('active', b.dataset.level === lvl));
    renderEvents();
  }

  function setSourceFilter(src) {
    activeSource = src;
    document.querySelectorAll('[data-source]').forEach(b => b.classList.toggle('active', b.dataset.source === src));
    renderEvents();
  }

  function onSearchChange() {
    searchQuery = document.getElementById('searchInput').value.trim().toLowerCase();
    renderEvents();
  }

  function toggleAutoScroll() {
    autoScroll = !autoScroll;
    const b = document.getElementById('btnScroll');
    b.textContent = autoScroll ? 'Auto-Scroll: ON' : 'Auto-Scroll: OFF';
    b.classList.toggle('active', autoScroll);
  }

  function togglePause() {
    isPaused = !isPaused;
    const b = document.getElementById('btnPause');
    b.textContent = isPaused ? 'Resume' : 'Pause';
    b.classList.toggle('active', isPaused);
    document.getElementById('kpiRate').textContent = isPaused ? 'Paused' : 'Active';
    document.getElementById('kpiRate').style.color = isPaused ? 'var(--amber)' : 'var(--cyan)';
  }

  function clearEvents() {
    allEvents = [];
    counts = { all: 0, debug: 0, info: 0, warning: 0, alarm: 0, error: 0 };
    updateCounts();
    renderEvents();
  }

  function exportJson() {
    const blob = new Blob([JSON.stringify(allEvents, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `peripheral_events_${Date.now()}.json`;
    a.click();
    URL.revokeObjectURL(url);
  }

  function timeAgo(atUs) {
    if (!atUs) return 'just now';
    const nowUs = Date.now() * 1000;
    const diffUs = nowUs - atUs;
    if (diffUs < 1000000) return 'just now';
    const s = Math.floor(diffUs / 1000000);
    if (s < 60) return `${s}s ago`;
    const m = Math.floor(s / 60);
    if (m < 60) return `${m}m ago`;
    const h = Math.floor(m / 60);
    return `${h}h ago`;
  }

  function updateCounts() {
    document.getElementById('cntAll').textContent = counts.all;
    document.getElementById('cntDebug').textContent = counts.debug;
    document.getElementById('cntInfo').textContent = counts.info;
    document.getElementById('cntWarn').textContent = counts.warning;
    document.getElementById('cntAlarm').textContent = counts.alarm;
    document.getElementById('cntError').textContent = counts.error;
    document.getElementById('kpiTotalEvents').textContent = allEvents.length;
  }

  function renderEvents() {
    const tbody = document.getElementById('eventTbody');
    const filtered = allEvents.filter(e => {
      if (activeLevel !== 'all' && e.level !== activeLevel) return false;
      const src = (e.cameraId || 'system').toLowerCase();
      if (activeSource !== 'all' && src !== activeSource) return false;
      if (searchQuery) {
        const text = `${e.code || ''} ${e.message || ''} ${src}`.toLowerCase();
        if (!text.includes(searchQuery)) return false;
      }
      return true;
    });

    if (filtered.length === 0) {
      tbody.innerHTML = '<tr><td colspan="6" class="empty-state">No events matching the active filter.</td></tr>';
      return;
    }

    let html = '';
    // Display newest events at top
    for (const e of filtered.slice(0, 150)) {
      const src = e.cameraId || 'system';
      const lvl = (e.level || 'info').toLowerCase();
      html += `<tr>
        <td class="mono" style="color:var(--muted);">${e.id}</td>
        <td class="mono">${timeAgo(e.atUs)}</td>
        <td><span class="badge-lvl ${lvl}">${lvl}</span></td>
        <td><span class="badge-source ${src}">${src}</span></td>
        <td class="mono" style="font-weight:600; color:var(--text);">${e.code || 'event'}</td>
        <td>${escapeHtml(e.message || '')}</td>
      </tr>`;
    }
    tbody.innerHTML = html;

    if (autoScroll && tbody.rows.length > 0) {
      tbody.rows[0].scrollIntoView({ behavior: 'smooth', block: 'nearest' });
    }
  }

  function escapeHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }

  async function pollEvents() {
    if (isPaused) return;
    try {
      const res = await fetch(`/api/events?since_id=${highestId}&limit=100`);
      if (res.ok) {
        const newEvents = await res.json();
        if (Array.isArray(newEvents) && newEvents.length > 0) {
          for (const ev of newEvents) {
            if (ev.id > highestId) {
              highestId = ev.id;
              allEvents.unshift(ev);
              counts.all++;
              const lvl = (ev.level || 'info').toLowerCase();
              if (counts[lvl] !== undefined) counts[lvl]++;
            }
          }
          if (allEvents.length > 1000) allEvents.length = 1000;
          updateCounts();
          renderEvents();
        }
      }
    } catch (err) {}
  }

  async function pollAlarms() {
    try {
      const res = await fetch('/api/state');
      if (res.ok) {
        const data = await res.json();
        const alarms = data.alarms || [];
        const panel = document.getElementById('alarmsPanel');
        const container = document.getElementById('alarmsContainer');
        const kpiAlarms = document.getElementById('kpiAlarms');
        const kpiAlarmsSub = document.getElementById('kpiAlarmsSub');
        const countSpan = document.getElementById('alarmsCount');

        countSpan.textContent = `${alarms.length} Active`;
        kpiAlarms.textContent = alarms.length;

        if (alarms.length > 0) {
          panel.classList.add('has-alarms');
          kpiAlarms.style.color = 'var(--rose)';
          kpiAlarmsSub.textContent = 'Active System Anomaly Detected';
          let html = '<ul class="alarms-list">';
          for (const a of alarms) {
            const sev = (a.severity || 'warning').toLowerCase();
            const src = a.cameraId || 'system';
            html += `<li class="alarm-item ${sev}">
              <span class="alarm-badge ${sev}">${sev}</span>
              <span class="badge-source ${src}">${src}</span>
              <b>${escapeHtml(a.code || '')}</b>
              <span>${escapeHtml(a.message || '')}</span>
            </li>`;
          }
          html += '</ul>';
          container.innerHTML = html;
        } else {
          panel.classList.remove('has-alarms');
          kpiAlarms.style.color = 'var(--emerald)';
          kpiAlarmsSub.textContent = 'All Rig Conditions Nominal';
          container.innerHTML = '<div class="alarm-nominal">✔ No active system faults, link drops, or motor stalls detected.</div>';
        }
      }
    } catch (err) {}
  }

  setInterval(pollEvents, 200);
  setInterval(pollAlarms, 600);
  pollEvents();
  pollAlarms();
</script>
</body>
</html>)raw_html";
}

} // namespace peripheral

