# Peripheral Pan-Tilt Gateway - Engineering Task Backlog & Implementation Roadmap

This document outlines the architectural status of the **Peripheral Camera Gateway Server**, tracking completed subsystems against the original take-home specifications and detailing all remaining engineering tasks.

---

## Subsystem Status Overview

| Subsystem / Feature | Status | Notes |
|---|---|---|
| **Non-blocking TCP Socket Transport** | **Complete** | Full-duplex `read_mutex_` / `write_mutex_`, millisecond timeouts, `SO_RCVBUF` 2 MB |
| **Stream Capture & Worker Threading** | **Complete** | Background threads, auto-reconnect with exponential backoff on camera restarts |
| **Preallocated 64-Slot Slab Pool** | **Complete** | Zero-copy slot handles (`FrameSlotHandle`), 0 MB/s heap churn, 40.6 MB footprint |
| **Protocol Framing & Wire Deframers** | **Complete** | IEEE 802.3 CRC32, 5000-byte garbage recovery, fate classification (`Sent`, `Corrupt`, `Cut`) |
| **PTP Clock Synchronization** | **Complete** | 4-timestamp software filter (`TimeSyncFilter`), 32-bit roll-over unwrapping, RTT tracking |
| **200 Hz Pose Interpolator** | **Complete** | Ring buffer with linear interpolation at mid-exposure ($<0.1^\circ$ precision against truth stamp) |
| **Dual-Camera Synchronized Recording** | **Complete** | `DiskRecorder`, rolling 10 MB chunks in `record/rec-<id>/`, `manifest.json`, gap accounting |
| **Web Server & REST Telemetry API** | **Complete** | Embedded HTTP server on port 8080, `/api/state`, `/api/preview`, `/api/command`, `/api/recording/*` |
| **Official Operator React Viewer** | **Complete** | Full integration with `utils/viewer` on port 5173 and bundled at 8080 |
| **One-Shot Runner Script** | **Complete** | `./run.sh` orchestrates simulator, server, and viewer with graceful `Ctrl+C` teardown |
| **Docker Containerization** | **Complete** | `Dockerfile` & `docker-compose.yml` on Ubuntu 24.04 (GCC 14, C++26, Node 22, 1 CPU / 250 MB RAM) |
| **Unit & Functional Test Suite** | **Complete** | 11/11 CTest unit & functional test suites pass 100% |
| **Cloud Storage Uplink Worker** | **Complete** | Background worker streaming completed chunks to port 7100 with SHA-256 and retries |
| **Offline Capture Dataset Validation** | **Complete** | Replay harness against `utils/data/capture/` verifying sequence, pose, and fate matching |
| **Live Alarms & Event Log Streaming** | **Pending** | Populating `alarms` and `events` arrays in `/api/state` for viewer alerts |
| **Automatic Local Disk Pruning** | **Pending** | Pruning confirmed uploaded chunks when approaching the 2 GB disk quota |
| **GitHub Actions CI/CD Pipeline** | **Complete** | Automated multi-platform builds, unit tests, and artifact uploads on push / manual dispatch |

---

## Detailed Pending Tasks

### 1. Cloud Storage Uplink Worker (`src/storage/cloud_uploader.cc`)
**Status**: **COMPLETE** (Implemented in `src/storage/cloud_uploader.cc`, `include/peripheral/crypto/sha256.hpp`, unit tested in `tests/test_cloud_uploader.cc`).

- [x] **HTTP PUT Client**:
  - Implemented lean, non-blocking HTTP PUT client using `TcpSocket` with keep-alive socket reuse.
  - Endpoint: `PUT /v1/servers/{server_id}/objects/{rec_id}/{chunk_id}`
  - Required Headers:
    - `Authorization: Bearer dev-token`
    - `Content-Length: <file_size>`
    - `X-Content-SHA256: <hex_sha256_hash>`
- [x] **Streaming SHA-256 Checksum Calculation**:
  - Zero-heap FIPS 180-4 streaming SHA-256 implementation (`Sha256::hash_file`) in 64 KB buffers.
- [x] **Upload State Machine & Error Handling**:
  - `201 Created` / `200 OK`: Mark chunk as confirmed uploaded (creates `.uploaded` marker).
  - `409 Conflict`: Object already exists with different contents; log warning and mark as uploaded.
  - `422 Unprocessable`: Checksum mismatch; re-read and re-attempt.
  - `503 Service Unavailable`: Cloud storage busy; apply exponential backoff (100 ms up to 3 s).
  - Network disconnect / drop: Reconnects and retries chunk upload idempotently.
- [x] **Startup Crash Recovery**:
  - Scans `record/rec-*/` on startup and re-enqueues any chunks or manifests missing `.uploaded` markers.
- [x] **Backlog Telemetry in `/api/state`**:
  - Dynamic telemetry feed to viewer's `UploadPanel`:
    ```json
    "upload": {
      "storage": "connected",
      "lastContactUs": 123456789,
      "backlogObjects": 2,
      "backlogBytes": 21251088,
      "oldestPendingUs": 123400000
    }
    ```
  - Dynamic `RecordingState.upload` calculation (`pending` -> `partial` -> `uploaded`).

---

### 2. Offline Capture Dataset Validation Suite (`tests/test_offline_capture.cc`)
**Status**: **COMPLETE** (Implemented in `tests/test_offline_capture.cc`, registered as `OfflineCaptureTest` in CTest with 11/11 suites passing).
**Objective**: Validate protocol parsing, clock recovery, and pose interpolation against the 20-second historical capture in `utils/data/capture/`.

- [x] **Replay Harness**:
  - Parsed `utils/data/capture/video-reads.csv` (1,299 reads) and `utils/data/capture/control-reads.csv` (4,020 reads) into `VideoFramer` and `ControlFramer`.
  - Chronologically interleaved 5,319 network read events by `rx_us` monotonic timestamp.
- [x] **Assertion Benchmarks**:
  - `exposure_start_us` recovery: **Max error 62 µs** (spec limit: < 100 µs), mean error 24.20 µs across 438 synchronized frames.
  - Mid-exposure pose interpolation: **Max pan error 0.0456°**, **max tilt error 0.0460°** (spec limit: < 0.1°), mean pan error 0.0017°, mean tilt error 0.0044°.
  - Embedded truth stamp extraction: **100.0% precision** (444/444 valid/corrupt frames matched `truth.csv` exactly).
  - Classified frame fates: **100.0% precision** across all 475 truth frames (441 `sent`, 3 `corrupt`, 1 `cut`, 2 `dropped_by_camera`, 28 `no_client`).
  - Historical anomaly events: Confirmed all 6 milestones from `events.csv` (32-bit clock wrap at 5s, corrupt frames at 7s, 5000 garbage bytes with false VFRM marker at 9s, wire drop / cut frame at 11s, camera reboot at 14s, home command at 16s).
  - Registered as Test #10 (`OfflineCaptureTest`) in CTest with 100% pass rate.

---

### 3. Live Alarms & Event Log Streaming
**Objective**: Surface runtime anomalies, motor faults, and network status changes into the viewer's `AlarmList` and `EventLog` components.

- [ ] **Alarm Management**:
  - Raise `warning` or `alarm` entries in `/api/state` when:
    - Camera control or video link drops (`camera_link_down`).
    - Pan or tilt motor stalls (`motor_stalled`).
    - Clock drift exceeds 100 ppm or clock synchronization is lost.
    - Disk usage exceeds 85% of 2 GB quota (`disk_near_full`).
  - Clear alarms automatically when conditions normalize.
- [ ] **Server Event Log Ring Buffer**:
  - Maintain a circular buffer of recent events (last 50 events) with monotonic timestamps:
    - Camera connected / reconnected.
    - Recording session started / stopped.
    - Chunk file rotated (`part-XXXX.bin`).
    - Chunk uploaded to cloud storage.
  - Expose in `/api/state` or `/api/events` so `utils/viewer` displays them in the `Events` panel.

---

### 4. Automatic Local Disk Reclamation Policy
**Objective**: Ensure the gateway never exceeds the 2 GB local disk limit under continuous recording.

- [ ] **Eviction Logic**:
  - Monitor total disk usage in `record/`.
  - When usage crosses 1.6 GB (80% of quota):
    - Identify historical recording sessions where `upload == "uploaded"`.
    - Delete verified binary chunks starting with the oldest session, keeping `manifest.json` intact.
  - If usage crosses 1.9 GB (95% of quota) and unuploaded chunks cannot be cleared:
    - Safely trigger `disk_full` gap accounting rather than corrupting active chunk files.

---

### 5. Automated CI/CD & Artifacts Pipeline (`.github/workflows/build-and-release.yml`)
**Status**: **Complete**

- [x] **GitHub Actions Workflow**:
  - Builds Linux x86_64 binaries with GCC 14 (C++26) on Ubuntu 24.04.
  - Builds macOS ARM64 binaries on Apple Silicon.
  - Runs 100% of unit/functional tests (`ctest`) on each platform.
  - Compiles the React Operator Viewer bundle (`npm run build`).
  - Verifies containerized execution with `docker build` and smoke tests.
  - Packages and uploads downloadable tarballs (`peripheral-linux-x86_64.tar.gz` and `peripheral-macos-arm64.tar.gz`) to GitHub Actions Artifacts.
  - Supports manual on-demand triggers via `workflow_dispatch`.
