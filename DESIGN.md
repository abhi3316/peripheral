# Peripheral Pan-Tilt Camera Gateway & Operator Viewer
## System Architecture Design Document & Build Procedure

**Target Platform:** Embedded Linux Gateway Server (Ubuntu 24.04 LTS / Debian 12)  
**Language Standard:** Modern C++26 (`-std=c++26`) / ISO C++23 compatible  
**Build System:** CMake 3.28+ with Ninja / GCC 14 / Clang 18  
**Document Revision:** 2.0.0 (Production Release)  

---

## 1. Executive Architecture Overview

The **Peripheral Camera Gateway** is a high-performance, resource-constrained distributed capture, edge-storage, and operator-telemetry server engineered to operate under strict embedded hardware limits.

### Core Deliverables
* **Dual Camera Ingress**: Simultaneously ingests raw greyscale video from two pan-tilt camera rigs (`cam0` and `cam1`) over Ethernet at **125 Mb/s each** (**250 Mb/s combined**, 50 fps, 1000×625 resolution).
* **High-Frequency Telemetry**: Ingests continuous **200 Hz position telemetry** (`POSE`), 2 Hz status heartbeats (`STATUS`), and hardware clock samples (`TIME_RESP`).
* **Zero-Allocation Slab Pool**: 64-slot preallocated memory pool providing zero runtime heap allocations ($0\text{ MB/s}$ churn) and a **1.28-second shock absorber** against thread scheduling and disk I/O jitter.
* **PTP Clock Synchronization**: 4-timestamp software filter with two's complement 32-bit roll-over unwrapping, achieving $<62\ \mu\text{s}$ time alignment against ground truth.
* **Mid-Exposure Pose Interpolation**: High-frequency ring buffer interpolating camera pan/tilt angles at frame mid-exposure timestamps with $<0.05^\circ$ precision ($<0.1^\circ$ requirement).
* **Deterministic Disk Recording**: Writes 10 MB rolling binary chunks (`part-XXXX.bin`) with crash-safe `manifest.json` and sequence gap tracking (`dropped_by_camera`, `damaged`, `link_down`, `disk_full`).
* **Automatic Local Disk Reclamation**: Enforces a strict 2 GB quota by automatically evicting verified uploaded chunks once disk usage crosses 80% (1.6 GB) while preserving manifests and receipt markers.
* **Cloud Storage Uplink Worker**: Non-blocking background worker streaming chunks to mock cloud storage (port 7100) using incremental FIPS 180-4 SHA-256 calculation and keep-alive socket reuse.
* **Lock-Free Live Alarms & Event Streaming**: Wait-free atomic ring buffer with level classification (`DEBUG`, `INFO`, `WARN`, `ALARM`, `ERROR`) and automated alarm normalization.
* **Operator Web Interface**: Real-time downsampling (1000×625 $\to$ 320×200), dedicated `/events` live dashboard, and embedded React Operator Viewer bundle on port 8080.

---

## 2. Hardware Constraints & Resource Budget Mathematics

```
Hardware Ceilings Enforced by Assignment:
┌────────────────────────────────────────────────────────┐
│  CPU: 1 Physical Core (Zero Hardware Parallelism)       │
│  RAM: 250 MB Maximum Total Memory                      │
│  Disk: 2 GB Maximum Local Persistent Storage           │
│  Cloud Link: 250 Mb/s Shared Uplink Bandwidth          │
└────────────────────────────────────────────────────────┘
```

### 2.1 Frame Ingress Mathematics
* **Frame Dimensions**: $1000\times 625$ pixels, 8 bits/pixel greyscale = $625,000\text{ bytes}$.
* **Frame Wire Header**: 32 bytes (`VFRM` header, sequence numbers, exposure timestamps, CRC32).
* **Total Frame Size**: $625,032\text{ bytes} \approx 625\text{ KB}$.
* **Ingress Throughput**:
  $$\text{Frame Rate} = 2\text{ cameras} \times 25\text{ fps} = \mathbf{50\text{ frames/second}}$$
  $$\text{Ingress Throughput} = 50 \times 625,032\text{ B} = \mathbf{31,251,600\text{ B/s}} \approx \mathbf{31.25\text{ MB/s}}\ (250.01\text{ Mb/s})$$

### 2.2 Sizing the Preallocated 64-Slot Slab Pool (~40.6 MB)
Under a **250 MB total RAM ceiling**, dynamic allocations (`malloc`/`new`) 50 times per second cause memory fragmentation, allocator lock contention, and rapid Out-Of-Memory (OOM) termination.

* **Slot Size**: 665,600 bytes (650 KB per slot; comfortably holds 625,032-byte frames).
* **Slot Partitioning**: 64 slots total = 32 slots dedicated per camera.
* **Total Pool Footprint**:
  $$\text{Pool Size} = 64\text{ slots} \times 650\text{ KB} = \mathbf{40.625\text{ MB}}$$
* **Temporal Cushion**:
  $$\text{Buffer Duration} = \frac{32\text{ slots}}{25\text{ fps}} = \mathbf{1.28\text{ seconds per camera}}$$

#### Why 40.6 MB is the Mathematically Optimal Size
1. **Why not smaller (e.g. 5–10 MB / ~200 ms)?**  
   On a single-core system, disk operations (`write()`, `fsync()`, or chunk rotation) can experience transient latency spikes of 100–300 ms. If the buffer is only 200 ms deep, any momentary I/O delay starves the buffer pool. The socket stops reading, kernel socket buffers overflow, and the camera drops frames permanently.
2. **Why not larger (e.g. 150 MB / ~5 seconds)?**  
   The Linux kernel maintains dirty file pages in RAM when writing to disk. Under sustained 31.25 MB/s disk recording, kernel writeback caches consume 60–100 MB. Allocating >100 MB to frame buffers would trigger the Linux kernel OOM killer under heavy write loads.
3. **The Sweet Spot**:  
   At **40.6 MB**, the pool consumes only **16.25% of total system RAM**, leaving $>200\text{ MB}$ for kernel writeback cache, socket buffers, and the web server.

### 2.3 System RAM Budget Allocation Table

| Subsystem Component | Allocated RAM | % of 250 MB Ceiling | Purpose |
|---|:---:|:---:|---|
| **Preallocated Slab Pool** | **40.6 MB** | 16.2% | Contiguous 64 slots @ 650 KB for zero-copy ingress |
| **Atomic Event Ring Buffer** | **0.1 MB** | 0.04% | 256 preallocated POD event slots for lock-free logging |
| **TCP Socket Buffers** | **8.0 MB** | 3.2% | 4 streams $\times$ 2 MB `SO_RCVBUF` kernel buffers |
| **Server Binary & Stacks** | **6.0 MB** | 2.4% | C++26 code segment, static constants, 8 thread stacks |
| **Embedded Web Server & Cache** | **4.0 MB** | 1.6% | HTTP server, REST JSON serialization, static bundle |
| **Kernel Page Cache (Dirty I/O)**| **150.0 MB** | 60.0% | Linux OS buffer cache for sustained 31.25 MB/s disk I/O |
| **Safety Headroom** | **41.3 MB** | 16.5% | Headroom against transient memory spikes |
| **TOTAL HARD CEILING** | **250.0 MB** | **100.0%** | **Strictly enforced via Docker cgroups v2** |

---

## 3. Subsystem Architecture

```
                  ┌───────────────────────────────────────────────┐
                  │          Dual Camera Rigs (Ethernet)          │
                  │   cam0 (7001/7002)     cam1 (7011/7012)       │
                  └───────┬──────────────┬──────────────┬─────────┘
                          │ Video        │ Control      │ Video
                          ▼ (125 Mb/s)   ▼ (200 Hz)     ▼ (125 Mb/s)
    ┌────────────────────────────────────────────────────────────────────────┐
    │                        PERIPHERAL GATEWAY SERVER                       │
    │                                                                        │
    │  ┌───────────────────────┐             ┌────────────────────────────┐  │
    │  │  peripheral_socket    │             │   TimeSyncFilter (4 Hz)    │  │
    │  │  Full-Duplex TCP I/O  │             │   PTP Software Clock Sync  │  │
    │  └──────────┬────────────┘             └─────────────┬──────────────┘  │
    │             │                                        │                 │
    │             ▼                                        ▼                 │
    │  ┌──────────────────────────────────────────────────────────────────┐  │
    │  │ VideoFramer & Wire Deframers (5000-byte noise recovery, CRC32)   │  │
    │  └──────────┬────────────────────────────────────────┬──────────────┘  │
    │             │                                        │                 │
    │             ▼                                        ▼                 │
    │  ┌───────────────────────┐             ┌────────────────────────────┐  │
    │  │ FrameMemoryPool (Slab)│             │  PoseInterpolator (200 Hz) │  │
    │  │ 64 Slots / 40.6 MB    │             │  Mid-Exposure Angles       │  │
    │  └──────────┬────────────┘             └─────────────┬──────────────┘  │
    │             │                                        │                 │
    │             ├───────────────────┬────────────────────┤                 │
    │             ▼                   ▼                    ▼                 │
    │  ┌─────────────────────┐ ┌──────────────┐ ┌─────────────────────────┐  │
    │  │ DiskRecorder        │ │ CloudUploader│ │ AtomicEventRingBuffer   │  │
    │  │ Rolling 10 MB Chunks│ │ HTTP PUT     │ │ Lock-Free Wait-Free     │  │
    │  │ 80% Reclamation     │ │ Port 7100    │ │ Level Classification    │  │
    │  └──────────┬──────────┘ └──────┬───────┘ └──────────┬──────────────┘  │
    │             │                   │                    │                 │
    │             ▼                   ▼                    ▼                 │
    │  ┌──────────────────────────────────────────────────────────────────┐  │
    │  │ Embedded WebServer (Port 8080) & React Operator UI Bundle       │  │
    │  │ REST APIs (/api/state, /api/preview) + Live Streaming (/events)  │  │
    │  └──────────────────────────────────────────────────────────────────┘  │
    └────────────────────────────────────────────────────────────────────────┘
```

### 3.1 Transport Subsystem (`peripheral_socket`)
* **Thread-Safe Full-Duplex Separation**: Dedicated `read_mutex_` and `write_mutex_` allow concurrent reads (`recv_some`/`recv_exact`) and writes (`send_all`) without lock contention.
* **Ingress Socket Tuning**: `SO_RCVBUF` set to 2 MB, `TCP_NODELAY` enabled, and `SO_NOSIGPIPE`/`MSG_NOSIGNAL` handled.
* **Graceful Teardown**: `close()` issues `shutdown(SHUT_RDWR)` before closing file descriptors, unblocking worker threads immediately.

### 3.2 Protocol Deframers & Wire Recovery
* **IEEE 802.3 CRC32 Validation**: Hardware-matching CRC table validates header and payload integrity.
* **5,000-Byte Garbage Recovery**: `VideoFramer` scans contiguous buffers with `std::memchr` for the `VFRM` magic bytes, automatically re-synchronizing after arbitrary wire noise and false markers.
* **Frame Fate Classification**: Classifies all frames into `Sent`, `Corrupt`, `Cut`, `DroppedByCamera`, or `NoClient`.

### 3.3 Clock Synchronization & 200 Hz Pose Interpolation
* **PTP 4-Timestamp Software Filter**: Measures round-trip time (RTT) and calculates one-way transit delay using monotonic timestamps and camera hardware counters.
* **32-Bit Roll-Over Unwrapping**: Unwraps 32-bit microsecond counter roll-overs (occurring every ~71.5 minutes) via signed modular arithmetic.
* **Linear Pose Interpolator**: Interpolates 200 Hz pan/tilt positions at frame mid-exposure:
  $$t_{\text{mid}} = t_{\text{start}} + \frac{t_{\text{exposure}}}{2}$$
  Achieves max error of **$0.0456^\circ$ pan** and **$0.0460^\circ$ tilt** (spec limit: $<0.1^\circ$).

### 3.4 Deterministic Disk Recording & Automatic Reclamation
* **10 MB Rolling Chunks**: Records continuous dual-camera video to `record/rec-<id>/part-XXXX.bin` with `manifest.json`.
* **High-Watermark Reclamation Policy**:
  * **$< 1.6\text{ GB}$ (80%)**: Retains chunks locally as a high-speed playback cache.
  * **$\ge 1.6\text{ GB}$ (80%)**: Deletes `.bin` files that have verified `.uploaded` receipt markers starting with the oldest session, down below 1.6 GB.
  * **Preserves Receipts**: `manifest.json` and 65-byte `.uploaded` receipt markers are permanently preserved.
  * **$\ge 1.9\text{ GB}$ (95% Emergency Brake)**: Gracefully rejects frames with `disk_full` gap accounting rather than corrupting files.

### 3.5 Cloud Storage Uplink Worker
* **Streaming SHA-256**: Zero-heap FIPS 180-4 incremental hasher computes hashes using 64 KB buffers.
* **HTTP PUT Pipeline**: Streams chunks to `PUT /v1/servers/peripheral-1/objects/{rec_id}/{chunk_id}` with keep-alive socket reuse and exponential backoff retry.
* **Crash Recovery**: Automatically scans `record/` on startup and re-enqueues un-uploaded chunks.

### 3.6 Lock-Free Live Alarms & Event Log Streaming
* **Wait-Free Ticket Generation**: Uses `write_ticket_.fetch_add(1, std::memory_order_relaxed)` with release/acquire sequencing on 256 preallocated slots. Zero mutex locks on hot path.
* **Dynamic Log Level Classification**: Supports `DEBUG`, `INFO`, `WARN`, `ALARM`, and `ERROR` via CLI `--log-level` and API query parameters.
* **Automated Alarm State Machine**: Automatically raises and clears alarms for `camera_link_down`, `motor_stalled`, `clock_sync_lost`, and `disk_near_full`.
* **Dedicated Live Dashboard**: Available at `/events` and `/logs`.

---

## 4. Build, Execution & Deployment Procedure

### 4.1 System Prerequisites
* **Compiler**: GCC 14 (`gcc-14`, `g++-14`) or Clang 18 supporting modern C++26 / C++23.
* **Build Tools**: CMake 3.28+, Ninja, `lsof`, `jq`.
* **Runtime**: Node.js 22 LTS (for simulator and React viewer).
* **Containerization**: Docker Engine 24+ and Docker Compose v2.

### 4.2 Native Compilation & Testing
```bash
# 1. Configure Release build with C++26
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja

# 2. Compile the peripheral server and test executables
cmake --build build -j

# 3. Execute all 12 CTest unit and functional test suites
ctest --test-dir build --output-on-failure
```
**Expected Output:** `100% tests passed, 0 tests failed out of 12` (Total time: ~1.7s).

### 4.3 Running the Complete Stack Natively (`./run.sh`)
```bash
./run.sh
```
This orchestrates the C++ server, dual-camera simulator, cloud storage simulator, and React viewer with clean `Ctrl+C` teardown.

---

## 5. Docker Deployment Under Strict Constraints (1 CPU, 250 MB RAM)

The production Docker setup in [docker-compose.yml](file:///Users/achu/Jobs/peripheral/docker-compose.yml) enforces the hardware limits:

```yaml
services:
  simulator:
    build: .
    entrypoint: ["sh", "-c", "(cd utils/tools && npm run camera -- --host 0.0.0.0) & (cd utils/tools && npm run storage -- --host 0.0.0.0) & wait"]
    ports: ["7001-7002:7001-7002", "7011-7012:7011-7012", "7100-7101:7100-7101"]

  gateway:
    build: .
    entrypoint: ["./build/peripheral_server"]
    command: ["--cam-host", "simulator", "--storage-host", "simulator"]
    ports: ["8080:8080"]
    volumes: ["./record:/app/record"]
    cpus: 1.0
    mem_limit: 250m
    memswap_limit: 250m
    deploy:
      resources:
        limits:
          cpus: '1.0'
          memory: 250M
```

### Launch and Verify:
```bash
# Start containerized deployment
docker compose up -d

# Verify real-time resource consumption
docker stats --no-stream
```

### Verified Benchmark Output:
```
CONTAINER ID   NAME                 CPU %     MEM USAGE / LIMIT     MEM %     NET I/O
774b0e9ca391   peripheral-gateway   25.89%    46.68MiB / 250MiB     18.67%    834MB / 655kB
```

* **RAM Footprint**: **46.7 – 49.0 MiB** (Only **~19%** of the 250 MiB limit!).
* **CPU Consumption**: **25% – 40%** of a single core.
* **Throughput**: Dual 125 Mb/s streams ingested with **0 frame drops** and **0 corruptions**.

---

## 6. Verification & Test Suite Summary

```
 1/12 Test  #1: SocketTest .......................   Passed    0.11 sec
 2/12 Test  #2: StreamCaptureTest ................   Passed    0.33 sec
 3/12 Test  #3: StreamBufferTest .................   Passed    0.04 sec
 4/12 Test  #4: CameraSessionTest ................   Passed    0.21 sec
 5/12 Test  #5: FramersTest ......................   Passed    0.01 sec
 6/12 Test  #6: FramePoolTest ....................   Passed    0.01 sec
 7/12 Test  #7: TimeSyncTest .....................   Passed    0.00 sec
 8/12 Test  #8: DiskRecorderTest .................   Passed    0.02 sec
 9/12 Test  #9: CloudUploaderTest ................   Passed    0.01 sec
10/12 Test #10: OfflineCaptureTest ...............   Passed    0.07 sec
11/12 Test #11: EventRingTest ....................   Passed    0.00 sec
12/12 Test #12: FunctionalCameraTest .............   Passed    0.92 sec

100% tests passed, 0 tests failed out of 12 (Total time: 1.74s)
```
