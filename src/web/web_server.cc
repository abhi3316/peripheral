#include "peripheral/web/web_server.hpp"

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
        << "  },\n"
        << "  \"upload\": {\n"
        << "    \"storage\": \"connected\",\n"
        << "    \"lastContactUs\": " << now_us << ",\n"
        << "    \"backlogObjects\": 0,\n"
        << "    \"backlogBytes\": 0,\n"
        << "    \"oldestPendingUs\": null\n"
        << "  },\n";

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

    oss << "  \"alarms\": []\n"
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

} // namespace peripheral
