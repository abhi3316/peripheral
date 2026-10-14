#!/usr/bin/env python3
"""
Virtual Camera Simulator for Pan-Tilt Capture Rig
Emulates cam0 and cam1 TCP video and control streams according to specification:
 - Video: 1000x625 greyscale frames with 32-byte VFRM header + CRC32 at 25 fps.
 - First 12 bytes contain the truth stamp (pan, tilt, frame_seq).
 - Remaining pixels generate cycling shades of blue (cam0) / green (cam1).
 - Control: 16-byte PTCL header with HELLO, 200 Hz POSE, 2 Hz STATUS, TIME_RESP, and ACK.
 - Admin HTTP REST API on port 7000 (cam0) / 7010 (cam1):
    * GET  /admin/stats  - Real-time stream metrics, camera state, pose, clock counter
    * GET  /admin/truth  - Historical ground truth frames for verification
    * POST /admin/fault  - Injects faults (corrupt, garbage, video_down, control_down, reboot, stall, unstall)
    * POST /admin/play   - Controls motion playback ({"stop": true} or {})
"""

import argparse
import http.server
import json
import math
import os
import select
import socket
import struct
import sys
import threading
import time
import urllib.parse
import zlib
from typing import Any, Dict, List, Optional, Set, Tuple

VFRM_START_MARKER = 0x4D524656  # 'VFRM' little-endian
PTCL_START_MARKER = 0x4C435450  # 'PTCL' little-endian

MSG_HELLO = 0x01
MSG_POSE = 0x02
MSG_TIME_RESP = 0x03
MSG_ACK = 0x04
MSG_STATUS = 0x05
MSG_TIME_REQ = 0x81
MSG_CMD = 0x82

LIMITS = {
    "pan_min_deg": -170.0,
    "pan_max_deg": 170.0,
    "tilt_min_deg": -30.0,
    "tilt_max_deg": 90.0,
    "pan_max_deg_per_s": 100.0,
    "tilt_max_deg_per_s": 50.0,
}

HOMING_SPEED_DEG_PER_S = 30.0
JOG_VALID_SECONDS = 0.300  # 300 ms jog timeout
TWO32 = 1 << 32


def make_vfrm_frame(
    frame_seq: int,
    exposure_start_us: int,
    boot_id: int,
    width: int,
    height: int,
    pan_deg: float,
    tilt_deg: float,
    shade_val: int,
    corrupt: bool = False,
) -> Tuple[bytes, int]:
    """Constructs a compliant 32-byte header + width*height pixel frame."""
    total_pixels = width * height

    # First 12 bytes: Truth Stamp (pan_milli, tilt_milli, frame_seq)
    pan_milli = int(round(pan_deg * 1000.0))
    tilt_milli = int(round(tilt_deg * 1000.0))
    truth_stamp = struct.pack("<iiI", pan_milli, tilt_milli, frame_seq)

    # Remaining pixels: Generate cycling shades / horizontal gradient pattern
    row_bytes = bytearray((shade_val + (x * 40 // width)) % 256 for x in range(width))
    full_image = bytearray(total_pixels)
    full_image[0:12] = truth_stamp

    idx = 12
    while idx < total_pixels:
        chunk = min(len(row_bytes), total_pixels - idx)
        full_image[idx : idx + chunk] = row_bytes[0:chunk]
        idx += chunk

    # Compute uncorrupted payload CRC
    payload_crc = zlib.crc32(full_image) & 0xFFFFFFFF

    # If corruption requested: damage image payload after calculating CRC
    if corrupt:
        mid_idx = total_pixels // 2
        full_image[mid_idx] ^= 0xFF

    # 32-byte Header (bytes 0..27 before header_crc32)
    hdr_part1 = struct.pack(
        "<IIIHHH6sI",
        VFRM_START_MARKER,
        frame_seq,
        exposure_start_us & 0xFFFFFFFF,
        boot_id,
        width,
        height,
        b"\x00" * 6,
        payload_crc,
    )
    hdr_crc = zlib.crc32(hdr_part1) & 0xFFFFFFFF
    header = hdr_part1 + struct.pack("<I", hdr_crc)

    return header + bytes(full_image), payload_crc


def make_ptcl_message(msg_type: int, msg_seq: int, payload: bytes) -> bytes:
    """Constructs a compliant 16-byte header + payload control message."""
    version = 1
    payload_len = len(payload)
    hdr_part1 = struct.pack(
        "<IBBHII",
        PTCL_START_MARKER,
        version,
        msg_type,
        payload_len,
        msg_seq,
        0,  # crc placeholder
    )
    # CRC32 is calculated over header bytes 0..11 followed by payload
    crc_data = hdr_part1[0:12] + payload
    crc = zlib.crc32(crc_data) & 0xFFFFFFFF
    header = hdr_part1[0:12] + struct.pack("<I", crc)
    return header + payload


class VirtualCamera:
    def __init__(
        self,
        name: str,
        host: str,
        ctrl_port: int,
        vid_port: int,
        admin_port: int,
        width: int = 1000,
        height: int = 625,
        fps: int = 25,
        color_theme: str = "blue",
        counter_start: int = 0,
        ppm: float = 0.0,
        autoplay: bool = True,
    ):
        self.name = name
        self.host = host
        self.ctrl_port = ctrl_port
        self.vid_port = vid_port
        self.admin_port = admin_port
        self.width = width
        self.height = height
        self.fps = fps
        self.color_theme = color_theme

        self.lock = threading.Lock()
        self.running = False
        self.boot_id = 1
        self.frame_seq = 0
        self.msg_seq = 0
        self.dropped_by_camera = 0
        self.frames_captured = 0
        self.frames_sent = 0
        self.bytes_sent = 0

        # Rate estimation
        self.rate_window: List[Tuple[float, int]] = []

        # Clock model
        self.boot_mono = time.monotonic()
        self.counter_start = counter_start
        self.ppm = ppm

        # State & motion
        self.homed = True
        self.motion_state = "idle"  # idle, homing, moving, jogging, stopping, faulted
        self.playing = autoplay
        self.stalled = False
        self.fault: Optional[str] = None  # None, "stall_pan", "stall_tilt"

        self.pan_deg = 0.0
        self.tilt_deg = 10.0
        self.target_pan = 0.0
        self.target_tilt = 10.0

        # Jogging state
        self.jog_pan_speed = 0.0
        self.jog_tilt_speed = 0.0
        self.jog_expires = 0.0

        # Fault injection state
        self.corrupt_next = 0
        self.garbage_next = 0
        self.video_refuse_until = 0.0
        self.control_refuse_until = 0.0
        self.booting_until = 0.0

        # Duplicate command detection: remembers last 100 commands
        self.acks: Dict[int, int] = {}
        self.acks_order: List[int] = []

        # Active socket references
        self.active_video_sock: Optional[socket.socket] = None
        self.active_ctrl_sock: Optional[socket.socket] = None

        # Ground truth history (keeps last 10,000 frames)
        self.truth_entries: List[Dict[str, Any]] = []

        # Server threads
        self.vid_thread: Optional[threading.Thread] = None
        self.ctrl_thread: Optional[threading.Thread] = None
        self.motion_thread: Optional[threading.Thread] = None
        self.admin_httpd: Optional[http.server.HTTPServer] = None
        self.admin_thread: Optional[threading.Thread] = None

    def get_counter_at(self, mono_time: float) -> int:
        elapsed_us = (mono_time - self.boot_mono) * 1_000_000.0
        drifted_us = elapsed_us * (1.0 + self.ppm * 1e-6)
        return int(self.counter_start + drifted_us) & 0xFFFFFFFF

    def get_mono_us(self, mono_time: float) -> int:
        return int(mono_time * 1_000_000.0)

    def start(self):
        self.running = True

        # Pre-bind TCP servers so ports are listening immediately
        self.vid_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.vid_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.vid_srv.bind((self.host, self.vid_port))
        self.vid_srv.listen(5)
        self.vid_srv.settimeout(0.5)

        self.ctrl_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.ctrl_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ctrl_srv.bind((self.host, self.ctrl_port))
        self.ctrl_srv.listen(5)
        self.ctrl_srv.settimeout(0.5)

        # 1. Start Motion Physics Thread (100 Hz simulation step)
        self.motion_thread = threading.Thread(target=self._motion_loop, daemon=True)
        self.motion_thread.start()

        # 2. Start Video Stream Server
        self.vid_thread = threading.Thread(target=self._video_server, daemon=True)
        self.vid_thread.start()

        # 3. Start Control Stream Server
        self.ctrl_thread = threading.Thread(target=self._control_server, daemon=True)
        self.ctrl_thread.start()

        # 4. Start Admin HTTP Server
        self._start_admin_server()

        print(
            f"[{self.name}] Virtual Camera started ({self.color_theme.upper()} shades):\n"
            f"  Video:   tcp://{self.host}:{self.vid_port}\n"
            f"  Control: tcp://{self.host}:{self.ctrl_port}\n"
            f"  Admin:   http://{self.host}:{self.admin_port}/admin/stats"
        )

    def stop(self):
        self.running = False
        with self.lock:
            if self.active_video_sock:
                try:
                    self.active_video_sock.close()
                except Exception:
                    pass
            if self.active_ctrl_sock:
                try:
                    self.active_ctrl_sock.close()
                except Exception:
                    pass
        if hasattr(self, "vid_srv"):
            try:
                self.vid_srv.close()
            except Exception:
                pass
        if hasattr(self, "ctrl_srv"):
            try:
                self.ctrl_srv.close()
            except Exception:
                pass
        if self.admin_httpd:
            try:
                self.admin_httpd.shutdown()
                self.admin_httpd.server_close()
            except Exception:
                pass

    # -------------------------------------------------------------------------
    # Admin HTTP Server & Fault Injection Handlers
    # -------------------------------------------------------------------------
    def _start_admin_server(self):
        cam_ref = self

        class AdminHandler(http.server.BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass  # Suppress default request logging to avoid terminal clutter

            def send_json(self, status: int, data: Any):
                body = json.dumps(data).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
                self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization")
                self.end_headers()
                self.wfile.write(body)

            def do_OPTIONS(self):
                self.send_response(204)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
                self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization")
                self.end_headers()

            def do_GET(self):
                parsed = urllib.parse.urlparse(self.path)
                if parsed.path == "/admin/stats":
                    self.send_json(200, cam_ref.get_stats())
                elif parsed.path == "/admin/truth":
                    qs = urllib.parse.parse_qs(parsed.query)
                    from_us = int(qs.get("fromUs", ["0"])[0]) if "fromUs" in qs else None
                    to_us = int(qs.get("toUs", [str(sys.maxsize)])[0]) if "toUs" in qs else None
                    boot_id = int(qs.get("bootId", [str(cam_ref.boot_id)])[0]) if "bootId" in qs else None
                    from_seq = int(qs.get("fromSeq", ["0"])[0]) if "fromSeq" in qs else None
                    to_seq = int(qs.get("toSeq", [str(from_seq + 9999)])[0]) if from_seq is not None else None

                    frames = cam_ref.get_truth(from_us=from_us, to_us=to_us, boot_id=boot_id, from_seq=from_seq, to_seq=to_seq)
                    self.send_json(200, {"frames": frames})
                else:
                    self.send_json(404, {"error": "not found"})

            def do_POST(self):
                parsed = urllib.parse.urlparse(self.path)
                length = int(self.headers.get("Content-Length", 0))
                body = {}
                if length > 0:
                    try:
                        body = json.loads(self.rfile.read(length).decode("utf-8"))
                    except Exception:
                        pass

                if parsed.path == "/admin/fault":
                    err = cam_ref.inject_fault(body)
                    if err:
                        self.send_json(400, {"error": err})
                    else:
                        self.send_json(200, {"ok": True})
                elif parsed.path == "/admin/play":
                    stop_play = body.get("stop", False)
                    with cam_ref.lock:
                        cam_ref.playing = not stop_play
                        if cam_ref.playing:
                            cam_ref.motion_state = "moving"
                        else:
                            cam_ref.motion_state = "idle"
                    self.send_json(200, {"ok": True, "playing": cam_ref.playing})
                else:
                    self.send_json(404, {"error": "not found"})

        try:
            self.admin_httpd = http.server.ThreadingHTTPServer((self.host, self.admin_port), AdminHandler)
            self.admin_thread = threading.Thread(target=self.admin_httpd.serve_forever, daemon=True)
            self.admin_thread.start()
        except Exception as e:
            print(f"[{self.name}] Warning: Failed to start Admin HTTP server on port {self.admin_port}: {e}")

    def get_stats(self) -> Dict[str, Any]:
        with self.lock:
            now = time.monotonic()
            # Clean rate window older than 1.0s
            cutoff = now - 1.0
            self.rate_window = [(t, b) for (t, b) in self.rate_window if t >= cutoff]
            bytes_in_sec = sum(b for (_, b) in self.rate_window)
            mbit_per_s = (bytes_in_sec * 8.0) / 1e6

            motion = self.motion_state
            if self.playing and motion == "idle":
                motion = "moving"

            return {
                "cameraId": self.name,
                "bootId": self.boot_id,
                "stream": {"width": self.width, "height": self.height, "fps": self.fps},
                "frameBytes": self.width * self.height + 32,
                "framesCaptured": self.frames_captured,
                "framesSent": self.frames_sent,
                "droppedByCamera": self.dropped_by_camera,
                "bytesSent": self.bytes_sent,
                "sendBytesPerS": bytes_in_sec,
                "sendMbitPerS": round(mbit_per_s, 2),
                "videoClient": self.active_video_sock is not None,
                "controlClient": self.active_ctrl_sock is not None,
                "homed": self.homed,
                "motion": motion,
                "playing": self.playing,
                "fault": self.fault,
                "pose": {"panDeg": round(self.pan_deg, 4), "tiltDeg": round(self.tilt_deg, 4)},
                "counter": self.get_counter_at(now),
                "ppm": self.ppm,
                "monoUs": self.get_mono_us(now),
            }

    def get_truth(
        self,
        from_us: Optional[int] = None,
        to_us: Optional[int] = None,
        boot_id: Optional[int] = None,
        from_seq: Optional[int] = None,
        to_seq: Optional[int] = None,
    ) -> List[Dict[str, Any]]:
        with self.lock:
            res = []
            for entry in self.truth_entries:
                if from_us is not None:
                    if entry["exposureStartMonoUs"] < from_us or entry["exposureStartMonoUs"] > (to_us or sys.maxsize):
                        continue
                elif boot_id is not None:
                    if entry["bootId"] != boot_id:
                        continue
                    if from_seq is not None and entry["frameSeq"] < from_seq:
                        continue
                    if to_seq is not None and entry["frameSeq"] > to_seq:
                        continue
                res.append(entry)
            return res

    def inject_fault(self, f: Dict[str, Any]) -> Optional[str]:
        kind = f.get("kind")
        now = time.monotonic()
        with self.lock:
            if kind == "corrupt":
                count = max(1, int(f.get("count", 1)))
                self.corrupt_next += count
                print(f"[{self.name}] Fault: Corrupting next {count} frames.")
                return None

            elif kind == "garbage":
                bytes_count = max(1, min(1_000_000, int(f.get("bytes", 1000))))
                self.garbage_next = bytes_count
                print(f"[{self.name}] Fault: Injecting {bytes_count} garbage bytes before next frame.")
                return None

            elif kind == "video_down":
                ms = float(f.get("ms", 3000))
                self.video_refuse_until = now + (ms / 1000.0)
                if self.active_video_sock:
                    try:
                        self.active_video_sock.close()
                    except Exception:
                        pass
                    self.active_video_sock = None
                print(f"[{self.name}] Fault: Video link brought down for {ms} ms.")
                return None

            elif kind == "control_down":
                ms = float(f.get("ms", 3000))
                self.control_refuse_until = now + (ms / 1000.0)
                if self.active_ctrl_sock:
                    try:
                        self.active_ctrl_sock.close()
                    except Exception:
                        pass
                    self.active_ctrl_sock = None
                print(f"[{self.name}] Fault: Control link brought down for {ms} ms.")
                return None

            elif kind == "reboot":
                start = int(f.get("counterStart", int(os.urandom(4).hex(), 16)))
                ppm = float(f.get("ppm", self.ppm))
                self.booting_until = now + 1.0  # 1s reboot blackout
                self.boot_id += 1
                self.counter_start = start
                self.ppm = ppm
                self.boot_mono = now + 1.0
                self.frame_seq = 0
                self.dropped_by_camera = 0
                self.frames_captured = 0
                self.frames_sent = 0
                self.homed = False
                self.motion_state = "idle"
                self.playing = False
                self.stalled = False
                self.fault = None
                self.acks.clear()
                self.acks_order.clear()

                if self.active_video_sock:
                    try:
                        self.active_video_sock.close()
                    except Exception:
                        pass
                    self.active_video_sock = None
                if self.active_ctrl_sock:
                    try:
                        self.active_ctrl_sock.close()
                    except Exception:
                        pass
                    self.active_ctrl_sock = None

                print(f"[{self.name}] Fault: Camera rebooting! New boot_id={self.boot_id}, counter_start={start}, ppm={ppm}")
                return None

            elif kind == "stall":
                axis = f.get("axis", "pan")
                self.stalled = True
                self.fault = f"stall_{axis}"
                self.motion_state = "faulted"
                print(f"[{self.name}] Fault: Axis '{axis}' stalled!")
                return None

            elif kind == "unstall":
                self.stalled = False
                self.fault = None
                self.motion_state = "idle"
                print(f"[{self.name}] Fault: Axis unstalled.")
                return None

            else:
                return "kind must be one of: corrupt | garbage | video_down | control_down | reboot | stall | unstall"

    # -------------------------------------------------------------------------
    # Physics & Motion Step
    # -------------------------------------------------------------------------
    def _motion_loop(self):
        dt = 0.01  # 100 Hz simulation
        while self.running:
            time.sleep(dt)
            now = time.monotonic()
            with self.lock:
                if self.stalled:
                    self.motion_state = "faulted"
                    continue

                # 1. Jogging expiration check (300 ms timeout)
                if self.motion_state == "jogging":
                    if now > self.jog_expires:
                        self.jog_pan_speed = 0.0
                        self.jog_tilt_speed = 0.0
                        self.motion_state = "idle"
                    else:
                        self.pan_deg = max(LIMITS["pan_min_deg"], min(LIMITS["pan_max_deg"], self.pan_deg + self.jog_pan_speed * dt))
                        self.tilt_deg = max(LIMITS["tilt_min_deg"], min(LIMITS["tilt_max_deg"], self.tilt_deg + self.jog_tilt_speed * dt))
                        continue

                # 2. Homing
                if self.motion_state == "homing":
                    pan_diff = 0.0 - self.pan_deg
                    tilt_diff = 0.0 - self.tilt_deg
                    step_p = math.copysign(min(abs(pan_diff), HOMING_SPEED_DEG_PER_S * dt), pan_diff)
                    step_t = math.copysign(min(abs(tilt_diff), HOMING_SPEED_DEG_PER_S * dt), tilt_diff)
                    self.pan_deg += step_p
                    self.tilt_deg += step_t

                    if abs(self.pan_deg) < 0.05 and abs(self.tilt_deg) < 0.05:
                        self.pan_deg = 0.0
                        self.tilt_deg = 0.0
                        self.homed = True
                        self.motion_state = "idle"
                    continue

                # 3. GoTo movement
                if self.motion_state == "moving" and not self.playing:
                    pan_diff = self.target_pan - self.pan_deg
                    tilt_diff = self.target_tilt - self.tilt_deg
                    step_p = math.copysign(min(abs(pan_diff), LIMITS["pan_max_deg_per_s"] * dt), pan_diff)
                    step_t = math.copysign(min(abs(tilt_diff), LIMITS["tilt_max_deg_per_s"] * dt), tilt_diff)
                    self.pan_deg += step_p
                    self.tilt_deg += step_t

                    if abs(pan_diff) < 0.05 and abs(tilt_diff) < 0.05:
                        self.pan_deg = self.target_pan
                        self.tilt_deg = self.target_tilt
                        self.motion_state = "idle"
                    continue

                # 4. Automated Lissajous Playback
                if self.playing:
                    t = now - self.boot_mono
                    self.pan_deg = 45.0 * math.sin(t * 0.8)
                    self.tilt_deg = 15.0 + 20.0 * math.cos(t * 0.6)

    # -------------------------------------------------------------------------
    # Video Streaming Server
    # -------------------------------------------------------------------------
    def _video_server(self):
        srv = self.vid_srv
        interval = 1.0 / self.fps

        while self.running:
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            except Exception:
                break

            now = time.monotonic()
            with self.lock:
                if now < self.video_refuse_until or now < self.booting_until:
                    conn.close()
                    continue
                self.active_video_sock = conn

            print(f"[{self.name}] Video client connected from {addr}")
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

            next_frame_time = time.monotonic()
            while self.running:
                now = time.monotonic()
                with self.lock:
                    if now < self.video_refuse_until or now < self.booting_until or self.active_video_sock != conn:
                        break

                if now < next_frame_time:
                    time.sleep(max(0, next_frame_time - now))

                next_frame_time += interval

                with self.lock:
                    self.frame_seq += 1
                    self.frames_captured += 1
                    seq = self.frame_seq
                    boot_id = self.boot_id
                    pan = self.pan_deg
                    tilt = self.tilt_deg
                    exp_start_mono = now
                    exp_end_mono = now + 0.010  # 10 ms exposure
                    exp_start_counter = self.get_counter_at(exp_start_mono)
                    exp_end_counter = self.get_counter_at(exp_end_mono)

                    # Shades cycling
                    t = now - self.boot_mono
                    if self.color_theme == "blue":
                        shade = int(120 + 80 * math.sin(t * 1.5))
                    else:
                        shade = int(140 + 70 * math.cos(t * 1.5))

                    is_corrupt = False
                    if self.corrupt_next > 0:
                        self.corrupt_next -= 1
                        is_corrupt = True

                    garbage_len = 0
                    if self.garbage_next > 0:
                        garbage_len = self.garbage_next
                        self.garbage_next = 0

                # 1. Send garbage bytes if scheduled
                if garbage_len > 0:
                    try:
                        conn.sendall(os.urandom(garbage_len))
                    except Exception:
                        break

                # 2. Build and send frame
                frame_data, calc_crc = make_vfrm_frame(
                    frame_seq=seq,
                    exposure_start_us=exp_start_counter,
                    boot_id=boot_id,
                    width=self.width,
                    height=self.height,
                    pan_deg=pan,
                    tilt_deg=tilt,
                    shade_val=shade,
                    corrupt=is_corrupt,
                )

                fate = "corrupt" if is_corrupt else "sent"
                try:
                    conn.sendall(frame_data)
                    with self.lock:
                        self.frames_sent += 1
                        self.bytes_sent += len(frame_data) + garbage_len
                        self.rate_window.append((now, len(frame_data) + garbage_len))
                except (BrokenPipeError, ConnectionResetError, OSError):
                    fate = "cut"
                    print(f"[{self.name}] Video client disconnected (fate=cut).")
                    with self.lock:
                        self._record_truth(boot_id, seq, calc_crc, exp_start_mono, exp_end_mono, exp_start_counter, exp_end_counter, pan, tilt, fate)
                    break

                with self.lock:
                    self._record_truth(boot_id, seq, calc_crc, exp_start_mono, exp_end_mono, exp_start_counter, exp_end_counter, pan, tilt, fate)

            with self.lock:
                if self.active_video_sock == conn:
                    self.active_video_sock = None
            try:
                conn.close()
            except Exception:
                pass
        srv.close()

    def _record_truth(self, boot_id, seq, crc, exp_start_mono, exp_end_mono, exp_start_counter, exp_end_counter, pan, tilt, fate):
        entry = {
            "bootId": boot_id,
            "frameSeq": seq,
            "crc32": crc,
            "exposureStartMonoUs": self.get_mono_us(exp_start_mono),
            "exposureEndMonoUs": self.get_mono_us(exp_end_mono),
            "exposureStartCounter": exp_start_counter,
            "exposureEndCounter": exp_end_counter,
            "panDeg": round(pan, 4),
            "tiltDeg": round(tilt, 4),
            "width": self.width,
            "height": self.height,
            "fate": fate,
        }
        self.truth_entries.append(entry)
        if len(self.truth_entries) > 10000:
            self.truth_entries.pop(0)

    # -------------------------------------------------------------------------
    # Control Streaming Server
    # -------------------------------------------------------------------------
    def _control_server(self):
        srv = self.ctrl_srv
        while self.running:
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            except Exception:
                break

            now = time.monotonic()
            with self.lock:
                if now < self.control_refuse_until or now < self.booting_until:
                    conn.close()
                    continue
                self.active_ctrl_sock = conn

            print(f"[{self.name}] Control client connected from {addr}")
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            conn.setblocking(False)

            # Send HELLO (0x01)
            with self.lock:
                self.msg_seq += 1
                seq = self.msg_seq
                boot_id = self.boot_id
                homed = 1 if self.homed else 0

            cam_id_bytes = self.name.encode("ascii").ljust(16, b"\x00")
            fw_bytes = b"v1.0.0-sim\x00".ljust(16, b"\x00")
            hello_payload = struct.pack(
                "<16s16sHBB6fI",
                cam_id_bytes,
                fw_bytes,
                boot_id,
                homed,
                0,  # reserved
                LIMITS["pan_min_deg"],
                LIMITS["pan_max_deg"],
                LIMITS["tilt_min_deg"],
                LIMITS["tilt_max_deg"],
                LIMITS["pan_max_deg_per_s"],
                LIMITS["tilt_max_deg_per_s"],
                10000,  # exposure_us = 10 ms
            )
            hello_msg = make_ptcl_message(MSG_HELLO, seq, hello_payload)
            try:
                conn.sendall(hello_msg)
            except Exception:
                conn.close()
                continue

            pose_interval = 0.005  # 200 Hz = 5 ms
            status_interval = 0.5  # 2 Hz
            next_pose_time = time.monotonic()
            next_status_time = time.monotonic()

            recv_buf = bytearray()

            while self.running:
                now = time.monotonic()
                with self.lock:
                    if now < self.control_refuse_until or now < self.booting_until or self.active_ctrl_sock != conn:
                        break

                # 1. Read incoming commands from server
                r, _, _ = select.select([conn], [], [], 0.001)
                if r:
                    try:
                        data = conn.recv(1024)
                        if not data:
                            print(f"[{self.name}] Control client closed connection.")
                            break
                        recv_buf.extend(data)

                        while len(recv_buf) >= 16:
                            (marker, ver, mtype, plen, ctl_seq, crc) = struct.unpack("<IBBHII", recv_buf[0:16])
                            if marker != PTCL_START_MARKER:
                                del recv_buf[0]
                                continue
                            if len(recv_buf) < 16 + plen:
                                break
                            cmd_payload = bytes(recv_buf[16 : 16 + plen])
                            del recv_buf[0 : 16 + plen]

                            # Handle TIME_REQ (0x81)
                            if mtype == MSG_TIME_REQ and len(cmd_payload) >= 8:
                                echo_val = struct.unpack("<Q", cmd_payload[0:8])[0]
                                with self.lock:
                                    t2 = self.get_counter_at(now)
                                    t3 = self.get_counter_at(time.monotonic())
                                    self.msg_seq += 1
                                    seq = self.msg_seq
                                resp_payload = struct.pack("<QII", echo_val, t2, t3)
                                conn.sendall(make_ptcl_message(MSG_TIME_RESP, seq, resp_payload))

                            # Handle CMD (0x82)
                            elif mtype == MSG_CMD and len(cmd_payload) >= 16:
                                (cmd_id, cmd, _, a, b) = struct.unpack("<IB3sff", cmd_payload[0:16])

                                with self.lock:
                                    # Check duplicate command ID
                                    if cmd_id in self.acks:
                                        result = self.acks[cmd_id]
                                        duplicate = 1
                                    else:
                                        duplicate = 0
                                        result = self._execute_command(cmd, a, b, now)
                                        self.acks[cmd_id] = result
                                        self.acks_order.append(cmd_id)
                                        if len(self.acks_order) > 100:
                                            oldest = self.acks_order.pop(0)
                                            self.acks.pop(oldest, None)

                                    self.msg_seq += 1
                                    seq = self.msg_seq

                                ack_payload = struct.pack("<IBBH", cmd_id, result, duplicate, 0)
                                conn.sendall(make_ptcl_message(MSG_ACK, seq, ack_payload))

                    except (BrokenPipeError, ConnectionResetError, OSError):
                        break

                # 2. Send 200 Hz POSE message
                if now >= next_pose_time:
                    next_pose_time += pose_interval
                    with self.lock:
                        self.msg_seq += 1
                        seq = self.msg_seq
                        cam_counter = self.get_counter_at(now)
                        pan_counts = int(round(self.pan_deg * 10.0))
                        tilt_counts = int(round(self.tilt_deg * 10.0))

                        # motion codes: 0 idle, 1 homing, 2 moving, 3 jogging, 4 stopping, 5 faulted
                        motion_codes = {"idle": 0, "homing": 1, "moving": 2, "jogging": 3, "stopping": 4, "faulted": 5}
                        motion_val = motion_codes.get(self.motion_state, 0)
                        if self.playing and motion_val == 0:
                            motion_val = 2

                        homed_val = 1 if self.homed else 0
                        fault_val = 0
                        if self.fault == "stall_pan":
                            fault_val = 1
                        elif self.fault == "stall_tilt":
                            fault_val = 2

                    pose_payload = struct.pack("<IiiBBBB", cam_counter, pan_counts, tilt_counts, motion_val, homed_val, fault_val, 0)
                    try:
                        conn.sendall(make_ptcl_message(MSG_POSE, seq, pose_payload))
                    except Exception:
                        break

                # 3. Send 2 Hz STATUS message
                if now >= next_status_time:
                    next_status_time += status_interval
                    with self.lock:
                        self.msg_seq += 1
                        seq = self.msg_seq
                        status_payload = struct.pack(
                            "<HHIIIIII",
                            self.boot_id,
                            self.fps,
                            self.frames_captured,
                            self.frames_sent,
                            self.dropped_by_camera,
                            self.width,
                            self.height,
                            0,
                        )
                    try:
                        conn.sendall(make_ptcl_message(MSG_STATUS, seq, status_payload))
                    except Exception:
                        break

            with self.lock:
                if self.active_ctrl_sock == conn:
                    self.active_ctrl_sock = None
                    if self.motion_state == "jogging":
                        self.jog_pan_speed = 0.0
                        self.jog_tilt_speed = 0.0
                        self.motion_state = "idle"
            try:
                conn.close()
            except Exception:
                pass
        srv.close()

    def _execute_command(self, cmd: int, a: float, b: float, now: float) -> int:
        # Results: 0 accepted, 1 not homed, 2 out of limits, 3 faulted, 4 invalid
        if cmd == 4:  # stop
            self.playing = False
            self.jog_pan_speed = 0.0
            self.jog_tilt_speed = 0.0
            if self.motion_state != "faulted":
                self.motion_state = "idle"
            return 0

        elif cmd == 5:  # clear_fault
            if self.motion_state == "faulted":
                self.fault = None
                self.stalled = False
                self.motion_state = "idle"
            return 0

        elif cmd == 1:  # home
            if self.stalled or self.fault:
                return 3
            self.playing = False
            self.homed = False
            self.motion_state = "homing"
            return 0

        elif cmd == 2:  # goto
            if self.stalled or self.fault:
                return 3
            if not self.homed:
                return 1
            if not (LIMITS["pan_min_deg"] <= a <= LIMITS["pan_max_deg"] and LIMITS["tilt_min_deg"] <= b <= LIMITS["tilt_max_deg"]):
                return 2
            self.playing = False
            self.target_pan = a
            self.target_tilt = b
            self.motion_state = "moving"
            return 0

        elif cmd == 3:  # jog
            if self.stalled or self.fault:
                return 3
            if not self.homed:
                return 1
            self.playing = False
            pan_spd = max(-LIMITS["pan_max_deg_per_s"], min(LIMITS["pan_max_deg_per_s"], a))
            tilt_spd = max(-LIMITS["tilt_max_deg_per_s"], min(LIMITS["tilt_max_deg_per_s"], b))
            if pan_spd == 0.0 and tilt_spd == 0.0:
                self.motion_state = "idle"
                return 0
            self.jog_pan_speed = pan_spd
            self.jog_tilt_speed = tilt_spd
            self.jog_expires = now + JOG_VALID_SECONDS
            self.motion_state = "jogging"
            return 0

        else:
            return 4


def main():
    parser = argparse.ArgumentParser(description="Virtual Camera Simulator (cam0: Blue, cam1: Green)")
    parser.add_argument("--host", default="127.0.0.1", help="Bind IP address (default 127.0.0.1)")
    parser.add_argument("--cam0", action="store_true", help="Start only cam0 (blue shades)")
    parser.add_argument("--cam1", action="store_true", help="Start only cam1 (green shades)")
    parser.add_argument("--width", type=int, default=1000, help="Frame width (default 1000 pixels)")
    parser.add_argument("--height", type=int, default=625, help="Frame height (default 625 pixels)")
    parser.add_argument("--fps", type=int, default=25, help="Frame rate (default 25 fps)")
    parser.add_argument("--duration", type=float, default=0, help="Run duration in seconds (0 = infinite)")
    parser.add_argument("--counter-start", type=int, default=0, help="Initial clock counter value")
    parser.add_argument("--ppm", type=float, default=0.0, help="Clock drift in PPM")
    parser.add_argument("--no-motion", action="store_true", help="Start camera stationary and not homed")
    args = parser.parse_args()

    cameras = []
    run_all = not (args.cam0 or args.cam1)

    if run_all or args.cam0:
        cam0 = VirtualCamera(
            name="cam0",
            host=args.host,
            ctrl_port=7001,
            vid_port=7002,
            admin_port=7000,
            width=args.width,
            height=args.height,
            fps=args.fps,
            color_theme="blue",
            counter_start=args.counter_start,
            ppm=args.ppm,
            autoplay=not args.no_motion,
        )
        cameras.append(cam0)

    if run_all or args.cam1:
        cam1 = VirtualCamera(
            name="cam1",
            host=args.host,
            ctrl_port=711 if False else 7011,
            vid_port=7012,
            admin_port=7010,
            width=args.width,
            height=args.height,
            fps=args.fps,
            color_theme="green",
            counter_start=args.counter_start,
            ppm=args.ppm,
            autoplay=not args.no_motion,
        )
        cameras.append(cam1)

    for cam in cameras:
        cam.start()

    print("\nVirtual cameras active.")
    print("Press Ctrl+C to stop.")

    try:
        if args.duration > 0:
            time.sleep(args.duration)
        else:
            while True:
                time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nStopping virtual cameras...")
    finally:
        for cam in cameras:
            cam.stop()
        print("Virtual cameras stopped cleanly.")


if __name__ == "__main__":
    main()
