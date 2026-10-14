# Peripheral Camera Gateway Server

A high-performance, resource-constrained distributed capture and edge-storage gateway built in **C++26** for Linux.

The server captures raw greyscale video from two pan-tilt cameras at **125 Mb/s each** (combined 250 Mb/s) and high-frequency position telemetry at **200 Hz**, serves an operator web viewer for live viewing and steering, persists crash-safe recordings with synchronized pose coordinates, and streams completed recordings to cloud storage over a 250 Mb/s link.

---

## Hardware & System Constraints

The software is engineered to operate strictly within the following hardware limits:
- **CPU**: **1 physical core** (zero hardware parallelism; lock contention, unnecessary copies, and thread context switches must be strictly minimized).
- **RAM**: **250 MB** (enforced limit covering userland processes and kernel page cache for written files).
- **Disk**: **2 GB** (enforced limit; requires deterministic rolling chunk management and quota enforcement).
- **Cloud Uplink**: **250 Mb/s** (~31.25 MB/s).

---

## Memory & Buffer Pool Architecture: Why ~40 MB?

Under a **250 MB total RAM ceiling**, memory management must be mathematically deterministic. Dynamically allocating heap memory (`malloc`/`new`) 50 times per second causes allocator lock contention and memory fragmentation that leads to Out-Of-Memory (OOM) termination.

### 1. Frame Mathematics
- **Dimensions**: $1000 \times 625$ pixels, 8-bit greyscale (1 byte/pixel) = **$625,000$ bytes**.
- **Frame Header**: $32$ bytes (`VFRM` header, sequence, hardware timestamps, boot ID, CRCs).
- **Total Size per Frame**: $625,032\text{ bytes} \approx \mathbf{625\text{ KB}}$.

### 2. Ingress Throughput
Both cameras capture at **25 frames per second**:
$$\text{Total Frame Rate} = 2 \text{ cameras} \times 25\text{ fps} = \mathbf{50\text{ frames/second}}$$
$$\text{Data Ingress Rate} = 50 \times 625\text{ KB} \approx \mathbf{31.25\text{ MB/s}}\ (250\text{ Mb/s})$$

### 3. Sizing the 64-Slot (~40 MB) Buffer Pool
$$\text{Buffer Memory} = 64\text{ slots} \times 625\text{ KB} \approx \mathbf{40\text{ MB}}$$
$$\text{Buffer Duration} = \frac{64\text{ frames}}{50\text{ frames/second}} \approx \mathbf{1.28\text{ seconds}}$$

### 4. Trade-Off Analysis
* **Why not smaller (e.g. 5–10 MB / ~200 ms)?**  
  On a single-core system, disk operations (`write()`, `fsync()`, or chunk rotation) can experience transient latency spikes of 100–300 ms. If the buffer is only 200 ms deep, any momentary I/O delay starves the buffer pool. The socket stops reading, kernel socket buffers overflow, and the camera drops frames permanently.
* **Why not larger (e.g. 150 MB / ~5 seconds)?**  
  The 250 MB RAM ceiling covers everything, including the Linux kernel's dirty page cache of files being written to disk, socket buffers (`SO_RCVBUF`), network stack overhead, and the web viewer server. Allocating >100 MB to frame buffers would trigger the OOM killer under heavy write loads.
* **The Sweet Spot**:  
  At **40 MB**, the buffer pool consumes only **16% of total system RAM**, while providing a solid **1.28-second shock absorber** against thread scheduling and disk I/O jitter.

---

## Project Structure & Current Implementation

```
peripheral/
├── CMakeLists.txt                      # C++26 / C++23 build configuration
├── README.md                           # Project documentation & design decisions
├── AGENT_CONTEXT.md                    # Architecture status and synchronization notes
├── include/
│   └── peripheral/
│       ├── camera_endpoint.hpp         # Camera network endpoint configs (cam0, cam1, ..., camN)
│       ├── camera_session.hpp          # Domain coordinator with session_mutex_ synchronization
│       ├── socket_types.hpp            # Socket states, configs, metrics types
│       ├── socket.hpp                  # Lean, non-blocking TcpSocket transport primitive
│       ├── stream_capture.hpp          # capture_stream API & StreamCapture worker thread
│       └── stream_buffer.hpp           # Thread-safe stream accumulator (mutex, cv, peek)
├── src/
│   ├── socket.cc                       # Zero-overhead TcpSocket implementation
│   ├── camera_session.cc               # CameraSession lifecycle & command dispatch
│   ├── stream_capture.cc               # Background stream capture loop & auto-reconnect logic
│   ├── stream_buffer.cc                # Thread-safe StreamBuffer compaction and SIMD marker search
│   └── main.cc                         # Server application entrypoint
└── tests/
    ├── test_socket.cc                  # Unit tests: connection timeouts, loopback, non-blocking I/O
    ├── test_stream_capture.cc          # Unit tests: background capture & multi-camera concurrency
    ├── test_stream_buffer.cc           # Unit tests: thread-safe producer/consumer/peeker multi-threading
    └── test_camera_session.cc          # Unit tests: session-level mutex coordination and commands
```

---

## What Has Been Implemented So Far

### 1. `peripheral_socket` Static Library
* **Zero-Mutex Full-Duplex Architecture**: Utilizes atomic file descriptors and kernel-separated read/write buffers, enabling concurrent reads (`recv_some` / `recv_exact`) and writes (`send_all`) with zero application mutex overhead.
* **High-Throughput Ingress Tuning**: Configures `SO_RCVBUF` to 2 MB by default, disabling Nagle's algorithm (`TCP_NODELAY`), and handling `SO_NOSIGPIPE` / `MSG_NOSIGNAL` to prevent process termination on socket disconnects.
* **Non-Blocking Connect & Timeouts**: Robust `poll()`-based I/O multiplexing with millisecond-precision timeouts.
* **Graceful Cancellation**: `close()` issues `shutdown(SHUT_RDWR)` before closing file descriptors, immediately unblocking any in-flight reads in worker threads.

### 2. Extensible `CameraEndpoint` Configuration
* Defined in `include/peripheral/camera_endpoint.hpp`.
* Encapsulates connection parameters (`name`, `host`, `control_port`, `video_port`) and provides standard simulator presets (`cam0`: 7001/7002, `cam1`: 7011/7012).
* Eliminates hardcoded structures from `main.cc` and enables seamless scaling to any number of cameras.

### 3. `StreamCapture` & `capture_stream` Background Threading API
* Defined in `include/peripheral/stream_capture.hpp` and `src/stream_capture.cc`.
* Spawns a dedicated background worker thread per TCP stream.
* **Auto-Reconnection**: Transparently retries connections with configurable backoff when cameras drop or reboot (`video_down`, `reboot` faults).
* **Asynchronous Callbacks**: Passes received network slices to registered `DataCallback` functions (`std::span<const uint8_t>`) and connection state changes to `StateCallback`.

### 4. `StreamBuffer` Thread-Safe Contiguous Stream Accumulator
* Defined in `include/peripheral/stream_buffer.hpp` and `src/stream_buffer.cc`.
* **Fully Thread-Safe Synchronization**: Protected by internal `std::mutex` and `std::condition_variable` to safely support concurrent Producer (Socket Receiver), Consumer (Cloud Uploader / Disk Recorder), and Peeker (Web Viewer) threads.
* **Non-Destructive Peeking (`peek()`)**: Enables the Web Viewer thread to grab preview frames without consuming or stealing data from the Cloud Uploader or Disk Recorder pipelines.
* **Thread-Safe Draining (`read()`, `consume()`)**: Atomically copies and advances read pointers.
* **Condition Variable Blocking (`wait_readable()`)**: Allows consumers to sleep without burning CPU cycles until new bytes arrive (`notify_all`).
* **Zero-Copy Network Ingress**: `writable_span()` passes contiguous slices directly into `socket.recv_some()`, eliminating intermediate heap allocations and buffer copies.
* **Automatic Compaction**: Shifts unconsumed bytes to index 0 with `std::memmove` only when tail space runs low, resetting pointers to 0 whenever fully consumed.
* **Fast Marker Search**: Provides `find_marker()` using `std::memchr` to scan for 32-bit little-endian frame markers (`0x4D524656` `'VFRM'` and `0x4C435450` `'PTCL'`) across up to 5,000 injected garbage bytes.

### 5. `CameraSession` Domain Coordinator
* Defined in `include/peripheral/camera_session.hpp` and `src/camera_session.cc`.
* **Session-Level Synchronization (`session_mutex_`)**: Directly owns the synchronization mutex that coordinates background receiver worker threads (video + control) with outgoing operator commands (`send_control`) and lifecycle transitions.
* **Separation of Concerns**: Leaves `TcpSocket` as a zero-overhead I/O transport, while managing high-level camera lifecycle and stream callbacks in the domain layer.

### 6. Server Entrypoint (`src/main.cc`)
* Iterates through `CameraEndpoint::default_endpoints()` and initializes clean `CameraSession` instances.
* Configures video, control, and state callbacks with clean startup and graceful shutdown.

### 7. Automated Test Suite
* Built into CMake and executable via `ctest`:
  * `SocketTest`: Verifies connection failure handling, loopback transfer integrity, and lean non-blocking I/O.
  * `StreamCaptureTest`: Verifies background stream reception, callback dispatch, and concurrent multi-camera stream processing.
  * `StreamBufferTest`: Verifies contiguous reads/writes, auto-compaction without corruption, garbage-resilient marker search, and concurrent producer/consumer/peeker multi-threading.
  * `CameraSessionTest`: Verifies session-level mutex coordination between active streaming and concurrent command dispatch.
  * `FunctionalCameraTest` (`tests/test_functional_camera.py`): End-to-end Python test verifying full protocol compliance against simulated cameras.

### 8. Virtual Camera Simulator (`scripts/virtual_camera.py`)
* Fully emulates both physical camera rigs:
  * **`cam0`**: Video on port `7002`, Control on port `7001` (generates cycling **blue shades** pattern).
  * **`cam1`**: Video on port `7012`, Control on port `7011` (generates cycling **green shades** pattern).
* Streams 1000×625 greyscale frames with:
  * 32-byte `VFRM` header with little-endian values and CRC32 checks.
  * First 12 bytes containing the truth stamp (`pan_counts`, `tilt_counts`, `frame_seq`).
  * 25 fps precise frame pacing ($125\text{ Mb/s}$ per camera).
* Emulates full 16-byte `PTCL` control connection:
  * Sends `HELLO` on client connect.
  * Streams 200 Hz `POSE` telemetry with smooth Lissajous pan/tilt movement.
  * Streams 2 Hz `STATUS` packet reporting captured and sent frame counts.
  * Responds to `TIME_REQ` (`0x81`) with `TIME_RESP` (`0x03`) and `CMD` (`0x82`) with `ACK` (`0x04`).

---

### 9. Framers & Wire Protocol Subsystem
* `include/peripheral/crc32.hpp`: IEEE 802.3 CRC32 calculation matching camera hardware and `utils/` simulator checksums.
* `include/peripheral/protocol.hpp` & `include/peripheral/video_frame.hpp`: Wire layout definitions for 32-byte `VFRM` and 16-byte `PTCL` headers, payloads, and 12-byte truth stamps.
* `include/peripheral/video_framer.hpp` & `src/video_framer.cc`: Robust deframing engine that parses raw frames, validates CRC checksums, extracts truth stamps, classifies fates (`Sent`, `Corrupt`, `Cut`), and automatically recovers from up to 5,000 bytes of garbage.
* `include/peripheral/control_framer.hpp` & `src/control_framer.cc`: Parses telemetry (`HELLO`, `POSE`, `STATUS`, `TIME_RESP`, `ACK`) and serializes outgoing motion commands (`TIME_REQ`, `CMD`).

### 10. WebServer & Official Operator Viewer (`utils/viewer`)
* `include/peripheral/web/web_server.hpp` & `src/web/web_server.cc`: Embedded HTTP server on port `8080`.
* Downsamples incoming 1000×625 (625 KB) raw frames down to 320×200 (~64 KB) in real time (<0.1 ms latency).
* Serves REST APIs (`/api/state`, `/api/preview`, `/api/command`, `/api/jog`, `/api/recording/start`, `/api/recording/stop`, `/api/recordings`).
* Directly serves the official React Operator Viewer bundle (`utils/viewer/dist/`) on `http://localhost:8080/`, or interfaces with the Vite dev server on `http://localhost:5173/`.

### 11. Dual-Camera Storage & Recording Subsystem (`record/`)
* `include/peripheral/storage/disk_recorder.hpp` & `src/storage/disk_recorder.cc`:
  - Synchronized dual-camera capture writing into `record/rec-<id>/` at the project root.
  - Deterministic chunking: Splits stream into `part-0001.bin`, `part-0002.bin`, ... (10 MB per chunk).
  - Exact wire format: Back-to-back 32-byte `VideoFrameHeader` + image payload with truth stamps.
  - Zero heap allocation churn during streaming.
  - Gap accounting tracking missing sequences, corruptions, and disconnects.
  - Auto-generated `manifest.json` describing recording metadata and state.

---

## Running the Complete Stack

### Option A: Zero-Dependency Docker Container (Cross-Platform)
Run the entire stack in an isolated Linux environment (Ubuntu 24.04, GCC 14, Node 22) on any OS (macOS, Linux, Windows) with strict resource limits (1 CPU core, 250 MB RAM):

```bash
docker compose up --build
```
Or run directly with `docker run`:
```bash
docker build -t peripheral-gateway .
docker run --rm -it \
  --cpus=1.0 -m 250m \
  -p 8080:8080 -p 5173:5173 \
  -p 7000:7000 -p 7010:7010 \
  -p 7001:7001 -p 7002:7002 \
  -p 7011:7011 -p 7012:7012 \
  -v "$(pwd)/record:/app/record" \
  peripheral-gateway
```
* **Operator Viewer (Dev)**: `http://localhost:5173/`
* **Operator Viewer (Production)**: `http://localhost:8080/`
* **Recordings**: Automatically persisted to `./record/` on your host.

---

### Option B: Local Native One-Shot Runner (`./run.sh`)
Run locally without Docker on your native machine:
```bash
./run.sh
```
This builds the C++ binaries, prepares the viewer, spawns all background services, and opens the viewer on ports 5173 and 8080. Press `Ctrl+C` to cleanly stop all services.

---

### Alternative: Step-by-Step Manual Startup

#### Step 1: Start the Official Camera Simulator
Run the official simulator from `utils/tools`:
```bash
cd utils/tools && npm run camera
```
This spawns:
- `cam0`: Control `:7001`, Video `:7002`, Admin `http://127.0.0.1:7000/admin/stats`
- `cam1`: Control `:7011`, Video `:7012`, Admin `http://127.0.0.1:7010/admin/stats`

#### Step 2: Build and Start the Gateway Server
In another terminal, build and run `peripheral_server`:
```bash
cmake -B build
cmake --build build
./build/peripheral_server
```

#### Step 3: Open the Official Viewer
Run the Vite development server in `utils/viewer`:
```bash
cd utils/viewer
npm run dev
```
Open:
```
http://localhost:5173/
```
(Or open `http://localhost:8080/` which serves the precompiled production bundle `utils/viewer/dist`).

In the Official Viewer:
* **Recording Panel**: Click **Start recording** in the left sidebar.
  - Turns into a red **Stop recording** button.
  - Live readout of recording ID, elapsed time, frame count, byte count, and gap count.
  - Live video preview displays the blinking `REC` indicator.
  - Chunks and manifest are written directly to `record/rec-<id>/`.
* **Stop Recording**: Click **Stop recording**.
  - Finalizes `manifest.json` on disk.
  - Appears immediately in the completed recordings table.
  - Click on the recording ID to inspect gap details and frame metadata.
* **Camera Controls**: D-Pad jogging, homing, and go-to target positions.
* **Pipeline Metrics**: Monitor ingest rate (250 Mb/s), buffer utilization, and disk write throughput.

