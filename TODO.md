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
| **Unit & Functional Test Suite** | **Complete** | 9/9 CTest unit & functional test suites pass 100% |
| **Cloud Storage Uplink Worker** | **Pending** | Background worker streaming completed chunks to port 7100 with SHA-256 and retries |
| **Offline Capture Dataset Validation** | **Pending** | Replay harness against `utils/data/capture/` verifying sequence, pose, and fate matching |
| **Live Alarms & Event Log Streaming** | **Pending** | Populating `alarms` and `events` arrays in `/api/state` for viewer alerts |
| **Automatic Local Disk Pruning** | **Pending** | Pruning confirmed uploaded chunks when approaching the 2 GB disk quota |
| **GitHub Actions CI/CD Pipeline** | **Complete** | Automated multi-platform builds, unit tests, and artifact uploads on push / manual dispatch |

---

## Detailed Pending Tasks

### 1. Cloud Storage Uplink Worker (`src/storage/cloud_uploader.cc`)
**Objective**: Reliably stream completed chunk files (`part-0001.bin`, `part-0002.bin`, `manifest.json`) from local disk to the mock cloud storage service running at `http://127.0.0.1:7100`.

- [ ] **HTTP PUT Client**:
  - Implement a lean, non-blocking HTTP PUT client using `TcpSocket` or `libcurl` to upload objects.
  - Endpoint: `PUT http://127.0.0.1:7100/v1/servers/{server_id}/objects/{rec_id}/{chunk_id}`
  - Required Headers:
    - `Authorization: Bearer dev-token`
    - `Content-Length: <file_size>`
    - `X-Content-SHA256: <hex_sha256_hash>`
- [ ] **Streaming SHA-256 Checksum Calculation**:
  - Compute SHA-256 over chunk files before upload to populate `X-Content-SHA256`.
- [ ] **Upload State Machine & Error Handling**:
  - `201 Created` / `200 OK`: Mark chunk as confirmed uploaded.
  - `409 Conflict`: Object already exists with different contents; log warning and mark as uploaded.
  - `422 Unprocessable`: Checksum mismatch; re-read and re-attempt.
  - `503 Service Unavailable`: Cloud storage busy; apply exponential backoff (100 ms up to 5 s).
  - Network disconnect / drop: Idempotently retry the chunk upload.
- [ ] **Bandwidth & Resource Throttling**:
  - Respect the 250 Mb/s (~31.25 MB/s) maximum link bandwidth limit without starving CPU or network buffers.
- [ ] **Backlog Telemetry in `/api/state`**:
  - Connect upload state to the viewer's `UploadPanel`:
    ```json
    "upload": {
      "storage": "connected",
      "lastContactUs": 123456789,
      "backlogObjects": 2,
      "backlogBytes": 21251088,
      "oldestPendingUs": 123400000
    }
    ```
  - Update `RecordingState.upload` from `"pending"` -> `"partial"` -> `"uploaded"` once all chunks and `manifest.json` are confirmed.

---

### 2. Offline Capture Dataset Validation Suite (`tests/test_offline_capture.cc`)
**Objective**: Validate protocol parsing, clock recovery, and pose interpolation against the 20-second historical capture in `utils/data/capture/`.

- [ ] **Replay Harness**:
  - Parse `utils/data/capture/video-reads.csv` and feed variable-sized byte chunks from `video.bin` into `VideoFramer`.
  - Parse `utils/data/capture/control-reads.csv` and feed slices from `control.bin` into `ControlFramer`.
- [ ] **Assertion Benchmarks**:
  - Compare estimated `exposure_start_us` against `truth.csv`.
  - Compare interpolated `pan_deg` and `tilt_deg` against `truth.csv` at each mid-exposure.
  - Assert that classified frame fates (`sent`, `corrupt`, `cut`, `dropped_by_camera`, `no_client`) match `truth.csv` with 100% precision.
  - Assert that anomalous events (clock wrap at 5s, corrupt frames at 7s, 5000 garbage bytes at 9s, camera reboot at 14s) match `events.csv`.

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
