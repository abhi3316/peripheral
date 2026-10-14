# AGENT CONTEXT & ARCHITECTURE STATUS

## Project Overview
High-throughput, resource-constrained distributed capture and edge-storage gateway in C++26 on Linux:
- Two pan-tilt cameras (`cam0`, `cam1`) on Ethernet.
- Video stream: Raw greyscale at 125 Mb/s per camera (25 fps, 1000x625, ~15.625 MB/s each, combined 250 Mb/s).
- Telemetry: 200 Hz position (`POSE`), 2 Hz `STATUS`, `TIME_REQ`/`TIME_RESP` clock synchronization.
- Resource limits: 1 CPU core, 250 MB RAM, 2 GB disk.
- Cloud uplink: 250 Mb/s HTTP object storage (port 7100).
- Web viewer: Operator interface with preview, telemetry, controls, and recording management.
- Test harness compatibility: Works 100% with the provided `utils/` tools without any modifications to `utils/`.

---

## Architecture Components & Folder Layout

The project follows a subsystem/component-based architecture under `include/peripheral/` and `src/`:

### 1. Transport Subsystem (`include/peripheral/transport/`, `src/transport/`)
- `socket.hpp` / `socket_types.hpp` / `socket.cc`: Pure non-blocking TCP socket primitives with socket option tuning (`SO_RCVBUF`, `TCP_NODELAY`, `SO_NOSIGPIPE`).
- `stream_capture.hpp` / `stream_capture.cc`: Background thread TCP stream capture with auto-reconnection and direct zero-copy slot streaming hooks (`writable_span_provider`).

### 2. Memory Subsystem (`include/peripheral/memory/`, `src/memory/`)
- `stream_buffer.hpp` / `stream_buffer.cc`: Thread-safe contiguous ring buffer with peek/read support.
- `frame_pool.hpp` / `frame_pool.cc`: Preallocated circular slab pool (`FrameMemoryPool`, `FramePoolPartition`, `FrameSlot`, intrusive RAII `FrameSlotHandle`) eliminating all runtime heap allocations during ingress ($0\text{ MB/s}$ churn).

### 3. Protocol & Framing Subsystem (`include/peripheral/protocol/`, `src/protocol/`)
- `protocol.hpp` / `crc32.hpp`: 32-byte `VFRM`, 16-byte `PTCL`, 12-byte `TruthStamp`, and CRC-32 calculator.
- `video_frame.hpp`: High-level domain frame backed by `FrameSlotHandle`.
- `video_framer.hpp` / `video_framer.cc`: 5,000-byte garbage recovery, CRC validation, direct slot ingress, and frame fate classification.
- `control_framer.hpp` / `control_framer.cc`: Telemetry message framing (`HELLO`, `POSE`, `TIME_RESP`, `STATUS`, `ACK`) and command serialization (`TIME_REQ`, `CMD`).

### 4. Camera Coordination Subsystem (`include/peripheral/camera/`, `src/camera/`)
- `camera_endpoint.hpp`: Endpoint specifications and ports (`cam0`, `cam1`).
- `time_sync.hpp`: PTP 4-timestamp software filter (`TimeSyncFilter`) with two's complement 32-bit roll-over unwrapping, jitter outlier rejection, and explicit production PTP hardware alternative architectural note.
- `pose_interpolator.hpp`: High-frequency 200 Hz camera pose ring buffer and mid-exposure linear interpolator attaching estimated pan/tilt angles with $<0.1^\circ$ precision to `VideoFrame`.
- `camera_session.hpp` / `camera_session.cc`: Domain coordinator managing dual streams, dedicated endpoint pool partition, 4 Hz periodic `TIME_REQ` sync ticker thread, control commands, and frame enrichment.

### 5. Storage Subsystem (`include/peripheral/storage/`, `src/storage/`)
- `disk_recorder.hpp` / `disk_recorder.cc`: High-throughput dual-camera recording engine:
  - Root directory: `record/` at the workspace root (`record/rec-<id>/`).
  - Rolling binary chunks: `part-0001.bin`, `part-0002.bin`, ... partitioned deterministically at 10 MB per chunk.
  - Zero-heap allocation writes: Writes wire `VideoFrameHeader` (32 bytes) + frame payload directly from preallocated pool slots.
  - Gap tracking: Monitors frame sequence jumps, corrupt frames, socket disconnects, and disk full limits (`dropped_by_camera`, `damaged`, `link_down`, `disk_full`).
  - Manifest generation: Writes crash-safe `manifest.json` on start and finalizes on stop.
  - Quota enforcement & Automatic Reclamation:
    - Enforces 2 GB strict disk quota with configurable high-watermark thresholds (`reclamation_threshold_bytes` = 1.6 GB / 80%, `emergency_quota_bytes` = 1.9 GB / 95%).
    - Automatic reclamation: When crossing 1.6 GB, identifies historical sessions where chunks are confirmed uploaded (`.uploaded` receipt marker), evicts binary chunks (`part-XXXX.bin`) starting with oldest sessions, while preserving `manifest.json` and `.uploaded` markers for cloud sync state integrity.
    - Active chunk protection: Skips the currently written chunk in the active session during reclamation.
    - Emergency quota protection: If usage exceeds 1.9 GB without uploaded chunks to evict, safely records `disk_full` gap accounting rather than corrupting active chunks.
- `cloud_uploader.hpp` / `cloud_uploader.cc`: High-performance background cloud storage uplink worker:
  - Target: Mock cloud object storage on port 7100 (`PUT /v1/servers/peripheral-1/objects/{rec_id}/{chunk_id}`).
  - Non-blocking FIFO work queue with condition-variable background processing thread.
  - Zero RAM chunk churn: Streams files from disk in 64 KB buffers with keep-alive socket reuse.
  - Streaming SHA-256 calculator (`crypto/sha256.hpp`) computes FIPS 180-4 hashes for `X-Content-SHA256` headers.
  - Dynamic status transitions: `201`/`200` marked uploaded, `409` handled, `503` exponential backoff (100 ms to 3 s).
  - Restart resilience: Scans `record/` on startup and re-queues un-uploaded chunks.
  - Real-time telemetry: Powers `/api/state` upload section (`storage`, `lastContactUs`, `backlogObjects`, `backlogBytes`, `oldestPendingUs`).

### 6. Web & Operator Serving Subsystem (`include/peripheral/web/`, `src/web/`)
- `web_server.hpp` / `web_server.cc`: Embedded REST API server on port 8080 and static file server for `utils/viewer/dist`.
- Exposes recording control endpoints: `POST /api/recording/start`, `POST /api/recording/stop`, `GET /api/recordings`.
- Surfaces active recording state, disk pipeline metrics, and live cloud upload telemetry in `/api/state`.

### 7. Master Umbrella Headers (`include/peripheral.hpp` & `include/peripheral/peripheral.hpp`)
- Provides a single, clean generic entry point for consumers: `#include "peripheral/peripheral.hpp"` or `#include <peripheral.hpp>`.
- Aggregates all subsystems (`transport`, `memory`, `protocol`, `camera`, `storage`, `web`).
- Completely eliminates redundant loose header files in `include/peripheral/`.

### 8. Application Gateway Entrypoint (`src/main.cc`)
- Uses single umbrella include `#include "peripheral/peripheral.hpp"`.
- CLI flag parsing (`--pool-mb`, `--pool-slots`, `--timeout-ms`), slab memory pool initialization, endpoint partitioning, disk recorder lifecycle, and session lifecycle.
  - CLI Flags in `src/main.cc`:
    - `--pool-mb <N>`: Configure total memory pool size in Megabytes (e.g. `--pool-mb 48`).
    - `--pool-slots <N>`: Configure total number of slots directly (e.g. `--pool-slots 64`).

---

## Build System (`CMakeLists.txt`)
- Targets:
  - `peripheral_socket`: Core library (includes `frame_pool.cc`, `disk_recorder.cc`).
  - `peripheral_server`: Main gateway server executable.
  - `test_socket`: Socket unit tests.
  - `test_stream_capture`: Stream capture tests.
  - `test_stream_buffer`: StreamBuffer tests.
  - `test_camera_session`: Session coordination and thread safety tests.
  - `test_framers`: Video and control framing and garbage recovery tests.
  - `test_frame_pool`: Slab pool sizing, partitioning, intrusive handle ref-counting, and zero-allocation framer tests.
  - `test_time_sync`: PTP 4-timestamp clock offset/delay, 32-bit roll-over, jitter rejection, and 200 Hz pose interpolator unit tests.
  - `test_disk_recorder`: Unit tests for dual-camera chunking, gap detection, manifest persistence, automatic 80% disk reclamation, unuploaded chunk protection, and 95% emergency quota gap accounting.
  - `test_cloud_uploader`: Unit tests for streaming SHA-256 calculation, HTTP PUT object upload, and backlog telemetry.
  - `test_offline_capture`: Offline capture dataset validation suite benchmarking clock recovery (<100 µs), pose interpolation (<0.1°), 100% frame fate precision, and all 6 milestone events against `utils/data/capture/`.
  - `FunctionalCameraTest` (`tests/test_functional_camera.py`): End-to-end Python functional test suite.
- All tests pass 100% (11/11 tests passing).
- Verified against `utils/tools` (`npm run camera`): Real-time synchronization active, Δt offset tracking, <300 µs network RTT, 25 fps per-camera frame delivery.

---

## Standard Run & Operational Procedure (CRITICAL — ALWAYS FOLLOW)

**Rule:** NEVER use `scripts/virtual_camera.py`. ALWAYS utilize the official simulator in `utils/tools`.

### 1. Docker Container Runner (Zero Host Dependencies)
Run the entire stack in an isolated Linux environment (Ubuntu 24.04, GCC 14, Node 22) with resource enforcement (1 CPU, 250 MB RAM):
```bash
docker compose up --build
```
- Completely eliminates host OS and compiler dependencies.
- Mounts `./record` volume to persist all recordings to host disk.
- Exposes port 8080 (Gateway/Viewer), 5173 (Dev Viewer), 7000/7010 (Admin), and 7001-7012 (Camera streams).

### 2. Local One-Shot Runner (`./run.sh`)
Run locally without Docker:
```bash
./run.sh
```
- Automatically builds the C++ server and compiles the React viewer bundle.
- Starts the official simulator (`utils/tools`), gateway server (`:8080`), and React viewer (`:5173`).
- Passes through any server CLI flags (e.g. `./run.sh --timeout-ms 5000` or `./run.sh --pool-mb 48`).
- Cleans up all child processes gracefully on `Ctrl+C`.

### 3. Manual Component Startup
If running components individually in separate terminals:
- **Simulator**: `cd utils/tools && npm run camera`
- **Server**: `./build/peripheral_server`
- **Viewer**: `cd utils/viewer && npm run dev` (or open `http://localhost:8080/` for built bundle)

### 3. Monitoring Time & Clock Sync Telemetry
- **Terminal Console Heartbeat**: Logs every second:
  `[Heartbeat] cam0: SYNCED (Δt=+..., RTT=...µs, Frames=...) | cam1: SYNCED (...)`
- **Operator Web Interface**: Open `http://localhost:8080` in browser.
- **REST Telemetry State**: `curl -s http://localhost:8080/api/state | jq '.cameras[].clock'`
- **Live Frame Preview & Pose**: `curl -s http://localhost:8080/api/preview?camera=cam0 | jq`

### 4. Camera Motion Behavior
- The simulator starts in auto-playback mode playing `utils/data/motion.csv` in a loop.
- Any manual motion command (D-Pad jog, "Home", "Stop", "Go To") stops automated playback and sets camera state to `idle` or user-commanded motion.
- To resume continuous motion playback anytime, click the **"▶ Play Motion"** button in the Web Viewer or send `POST /api/command {"command":"play"}`.

### 5. Official React Viewer Integration (`utils/viewer`)
- Connected via `useYourServer()` in `utils/viewer/src/server/useServer.ts`.
- Relative API paths (`/api/...`) ensure seamless operation across both Vite dev server (`http://localhost:5173/`) via Vite proxy and direct production serving (`http://localhost:8080/`).
- Defensive schema sanitization in `useServer.ts` and backend `web_server.cc` ensures all expected fields are populated to prevent React render-time type errors.
- Production bundle compiled into `utils/viewer/dist/` (`npm run build`).
- `peripheral_server` serves the compiled React application directly on `http://localhost:8080/`.
- Alternatively runnable in Vite dev mode: `cd utils/viewer && npm run dev` (proxies `/api` to port 8080).

### 6. Dual-Camera Recording in Official Viewer (`utils/viewer`)
- **UI Location**: Left sidebar `Recording` panel (`Panels.tsx`).
- **Start Recording**:
  - Clicking "Start recording" sends `POST /api/recording/start`.
  - Server creates directory `record/rec-<id>/` with rolling `part-0001.bin`, `part-0002.bin`, ... (10 MB chunks).
  - The viewer button switches to red "Stop recording", shows `REC rec-<id>` badge, live elapsed time, frame count, byte tally, and gap count.
  - Video preview canvas flashes the red `REC` status pill on incoming frames.
- **Stop Recording**:
  - Clicking "Stop recording" sends `POST /api/recording/stop`.
  - Server flushes active chunk, finalizes `manifest.json`, and appends the session to `completed_recordings_`.
### 7. GitHub Actions CI/CD & Artifact Packaging (`.github/workflows/build-and-release.yml`)
- Triggers automatically on push/PR to `master` and `main`, or manually via GitHub's "Run workflow" button (`workflow_dispatch`).
- **Linux Job**: Runs on `ubuntu-24.04` with GCC 14 (C++26), compiles binary, executes all 9 unit tests, builds React viewer bundle, and uploads `peripheral-linux-x86_64.tar.gz`.
- **macOS Job**: Runs on `macos-14` (Apple Silicon), compiles binary, runs tests, and uploads `peripheral-macos-arm64.tar.gz`.
- **Docker Job**: Builds container image with automated smoke tests, saves it to `peripheral-gateway-docker.tar.gz`, and uploads `peripheral-gateway-docker-image`.
- Downloadable artifacts are preserved under the Actions run tab for 30 days.
