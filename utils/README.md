# Pan-Tilt Server Take-Home

This folder has everything you need to build and test your server on your own
computer, without any hardware. The assignment is in the PDF. This README explains
what we give you and how it works.

- [What's in this folder](#whats-in-this-folder)
- [Getting started](#getting-started)
- [How the pieces fit together](#how-the-pieces-fit-together)
- [Your server](#your-server)
- [The cameras](#the-cameras)
- [The camera simulator](#the-camera-simulator)
- [The cloud storage](#the-cloud-storage)
- [The viewer](#the-viewer)
- [The example data](#the-example-data)
- [Reference: message formats](#reference-message-formats)

## What's in this folder

| Path | What it is |
|------|------------|
| `Pan-Tilt Server Take-Home Assignment.pdf` | The assignment |
| `README.md` | This guide |
| `tools/` | The camera simulator and the cloud storage, which you run while you develop |
| `viewer/` | The operator's web page, finished except for the connection to your server |
| `data/motion.csv` | A recorded camera motion that the simulator plays |
| `data/capture/` | A 20-second recording of what a server received from the camera, with the correct answers, for testing your code offline |

## Getting started

You need Linux and Node.js 22 or later. Node is only for the tools and the viewer;
your server is C++.

Run each of these in its own terminal, from this folder:

```bash
cd tools && npm install && npm run camera      # the two simulated cameras, 125 Mb/s each
cd tools && npm run storage                    # the cloud storage, behind a 250 Mb/s link
cd viewer && npm install && npm run dev        # the viewer: open http://localhost:5173/?demo
```

The `?demo` page runs the viewer on a fake server, so you can see what a working
system looks like before you write any code.

## How the pieces fit together

```
+------------------+     +------------------+
|       cam0       |     |       cam1       |
+------------------+     +------------------+
    |         ^              |         ^
    | video   | control      | video   | control
    | 7002    | 7001         | 7012    | 7011
    v         v              v         v
+-------------------------------------------+
|                your server                |
+-------------------------------------------+
         ^                        |
         |  your design           |  uploads, HTTP 7100
         v                        v
+------------------+     +------------------+
|      viewer      |     |  cloud storage   |
+------------------+     +------------------+
```

- Each **camera** sends video over one TCP connection, and position, time and
  status messages over a second one. The server sends commands back on the
  second one.
- Your **server** receives all four connections, records the video, uploads the
  recordings to the **cloud storage**, and serves the **viewer**.
- How the server and the viewer talk to each other is up to you.

Any of these connections can drop at any time, and the cameras never wait for you.

## Your server

- **Language:** C++26, the latest version of C++. Where your compiler doesn't
  support something you need yet, C++23 is fine. Use any compiler and any
  libraries, and list what your build needs in your README.
- **Limits:** 1 CPU core, 250 MB of memory and 2 GB of disk, and we enforce them
  when we run your server. All of it is yours: nothing else on the computer uses
  it. The memory limit covers everything your server's processes use (exclude simulation tools), including
  the kernel's cache of the files they write.

## The cameras

There are two cameras, `cam0` and `cam1`. The simulator behaves like two real
pan-tilt cameras on Ethernet, and they are independent of each other: each has its
own connections, its own clock and its own motion, and one can fail while the other
keeps going.

Each camera:

- streams raw greyscale video at 125 Mb/s: 1000 × 625 images, 25 frames a second
- reports its pan and tilt 200 times a second, to 0.1°
- has its own clock, which is not your server's clock
- moves when your server tells it to: home, go to an angle, jog, or stop

| Camera | Control port | Video port | Test interface |
|--------|--------------|------------|----------------|
| `cam0` | 7001 | 7002 | 7000 |
| `cam1` | 7011 | 7012 | 7010 |

Pan goes from −170° to 170° (positive is to the right) at up to 100 °/s. Tilt goes
from −30° to 90° (positive is up) at up to 50 °/s. Each exposure lasts 10 ms.

### Video

The video connection carries frames back to back. Every frame starts with a small
header, followed by the image itself: one byte per pixel, row by row from the top.
The header tells you which frame this is, when its exposure started, how big the
image is, and two checksums: one for the header and one for the image. The exact
layout is in the [reference](#video-frames).

The camera does not wait for your server. If you don't read fast enough, it keeps a
few frames, then starts dropping new ones. It counts the dropped frames in its
`STATUS` message, and they are gone for good.

Your server has to cope with these problems. A real camera has them without
warning. The simulator only has them when you trigger them, from the viewer's Test
panel or its test interface (see [the camera simulator](#the-camera-simulator)):

- A frame arrives with a good header but a damaged image
- Random garbage bytes appear between frames, so a header can't be trusted until
  its checksum passes
- The connection closes in the middle of a frame
- Frames captured while no server was connected are never sent; you see them as a
  jump in the frame number

### Control

The control connection carries small messages in both directions:

| Message | Direction | What it's for |
|---------|-----------|---------------|
| `HELLO` | camera → server | Sent when you connect and after the camera restarts: its name, firmware, limits, exposure time and whether it is homed |
| `POSE` | camera → server | Pan and tilt, 200 times a second, stamped with the camera's clock |
| `STATUS` | camera → server | Twice a second: frames captured, sent and dropped, and the current image size |
| `TIME_REQ` | server → camera | Asks for the camera's clock |
| `TIME_RESP` | camera → server | The camera's clock when it received your request and when it replied |
| `CMD` | server → camera | Home, go to, jog, stop or clear a fault |
| `ACK` | camera → server | Whether a command was accepted, and if not, why |

A few rules about commands:

- Each command carries an ID that you choose. If the camera sees the same ID again,
  it does not repeat the command; it repeats its first answer.
- A jog only lasts 300 ms. To keep the camera moving, keep sending jogs with new
  IDs. If the jogs stop, so does the camera.
- If the control connection closes, the camera stops any jog straight away.
- The camera must be homed before it accepts "go to" or "jog". "Stop" is always
  accepted.

### Time

The camera stamps every frame and every position sample with its own clock: a
32-bit microsecond counter. That clock drifts by up to 100 parts per million against
your server's clock, wraps back to 0 after it reaches its maximum, and starts again
from an unknown value whenever the camera restarts.

To relate the two clocks, send `TIME_REQ` messages. Each reply tells you the
camera's clock when the request arrived and when the reply left, and you know your
own clock when you sent the request and when the reply arrived.

### The truth stamp

The simulator hides the true pan and tilt in the first 12 pixels of every frame,
measured at the middle of the exposure. It is there so that you can check your own
numbers. A real camera has no such stamp, so your server must work out each
frame's pose without it.

## The camera simulator

```bash
npm run camera                    # both cameras, playing the recorded motion
npm run camera -- --cameras 1     # only cam0, if you want to start with one
npm run camera -- --no-motion     # start still and not homed, like a real camera after power-on;
                                  # "Play motion" in the Test panel starts the motion later
```

When they start, the simulated cameras are homed and play the motion in
`data/motion.csv` over and over, each from a different point. As soon as your
server sends a camera a motion command, that camera stops the playback and follows
your commands, just as it would for an operator. After a restart a camera sits
still and is not homed.

Each camera also has a test interface (port 7000 for `cam0`, 7010 for `cam1`), so
that you can break things on purpose. The viewer's Test panel has buttons for most
of it. Your server must not use these interfaces.

| Request | What it does |
|---------|--------------|
| `GET /admin/stats` | Shows the stream, bytes sent, frames dropped and the camera's motion |
| `POST /admin/play` `{}` | Plays the recorded motion again (`{"stop":true}` stops it) |
| `POST /admin/fault` `{"kind":"corrupt","count":10}` | Damages the next 10 frames |
| `POST /admin/fault` `{"kind":"garbage","bytes":5000}` | Sends 5000 random bytes before the next frame |
| `POST /admin/fault` `{"kind":"video_down","ms":3000}` | Closes the video connection and refuses it for 3 seconds |
| `POST /admin/fault` `{"kind":"control_down","ms":3000}` | Does the same to the control connection |
| `POST /admin/fault` `{"kind":"reboot"}` | Restarts the camera. Its clock starts again from a random value; add `"counterStart":4290000000` to choose the value (for example just before the wrap) and `"ppm":80` to change the drift |
| `POST /admin/fault` `{"kind":"stall","axis":"pan"}` | Jams the pan motor (`{"kind":"unstall"}` frees it) |
| `GET /admin/truth?fromUs=…&toUs=…` | Gives the true exposure time, pose and fate of every frame in a time window, on this computer's `CLOCK_MONOTONIC`. The fates mean the same as in [the example data](#datacapture) |

For example, for `cam0`:

```bash
curl -s localhost:7000/admin/stats
curl -s -X POST localhost:7000/admin/fault -d '{"kind":"video_down","ms":3000}'
```

## The cloud storage

This tool stands in for the cloud storage that keeps your recordings. Its link to
your server runs at 250 Mb/s, shared by all uploads (`--uplink-mbps` changes it
while you test). It behaves as if it is far away on a poor network: it can be
slower, refuse connections or fail halfway through an upload.

It stores **objects**. An object is a name (the key) and some bytes. You decide what
an object is, for example a whole recording or a piece of one, and how big it is.
Every request needs the header `Authorization: Bearer dev-token`.

To upload, send a `PUT` with the SHA-256 of the body:

```bash
curl -X PUT http://127.0.0.1:7100/v1/servers/my-server/objects/rec-001/part-0001 \
  -H "Authorization: Bearer dev-token" \
  -H "X-Content-SHA256: $(sha256sum part-0001 | cut -d' ' -f1)" \
  --data-binary @part-0001
```

| Answer | Meaning |
|--------|---------|
| `201` | Stored |
| `200` | Already stored, with the same content |
| `409` | Already stored, with different content; the cloud storage keeps the first version |
| `422` | The body does not match its SHA-256, so nothing was stored |
| `503` | The cloud storage is busy; try again later |
| `401` | Wrong token |

Some things to know:

- **A key is for ever.** The cloud storage never forgets one, even after your server
  restarts or gets a new disk, and it never accepts different content for it. It
  keeps its keys in `tools/storage-data/keys.jsonl`, so they also survive a restart
  of the cloud storage. While you develop, `npm run storage -- --forget` starts
  from nothing.
- If the connection drops before you get an answer, you can't know whether the
  object was stored. Sending the same key with the same bytes again is always safe.
- `HEAD` on the same URL tells you whether the cloud storage has an object, with its
  size and SHA-256.
- `POST /v1/servers/{server}/status` accepts any JSON of up to 64 KB, if you want
  to report your server's health.
- Keys use letters, digits and `._/-`, up to 200 characters. Server names use
  lower-case letters, digits, `_` and `-`, up to 16 characters.
- Uploads must have a `Content-Length`; chunked transfer is not accepted.

The cloud storage has a test interface on port 7101: `POST /admin/fault` with
`{"down":true}`, `{"bytesPerSecond":1250000}` (10 Mb/s), `{"errorRate":0.3}`,
`{"dropAfterBytes":2000000,"count":3}` or `{"clear":true}`. `GET /admin/objects`
lists what it holds.

## The viewer

The viewer is the page an operator uses. It shows the cameras and their state, a
live preview, the camera's position, the motion controls, the data pipeline,
uploads, recordings with any missing frames, alarms and events.

```bash
cd viewer && npm run dev    # http://localhost:5173/?demo
```

The page is finished, apart from one function. **Connecting it to your server, and
serving it from your server, are part of the assignment.** Build the page with
`npm run build` and serve the files in `viewer/dist/`, so that an operator only
needs your server's address. Use `npm run dev` while you work on it.

To connect it, write `useYourServer()` in `src/server/useServer.ts`.
It returns a `ServerLink` (defined in `src/server/link.ts`) built from the types
in `src/server/types.ts`. The rest of the page only uses that interface, so you
shouldn't need to change anything else, although you may.

How the server and the page talk (the transport, the messages, the URLs) is your
design. You can add a proxy to `vite.config.ts` for `npm run dev` if it helps.

**Start recording** starts one recording of both cameras, which lasts until
**Stop recording**.

The Test panel at the bottom of the page uses the test interfaces of the camera
simulator and the cloud storage, so you can break things and watch what the page
shows. Pick the camera you want to break first. It works under `npm run dev` and
when your server serves the page, as long as the tools run on the same computer.

If your previews include the frame's truth stamp (`truthStamp`), the page shows how
far your pose is from the true pose. The viewer calculates that error itself.

## The example data

### `data/motion.csv`

What the simulator plays: 60 seconds of an operator moving the camera, sampled
every 10 ms, with fast pans, a slow pan and pauses. It ends where it starts, so it
loops smoothly.

```
t_ms,pan_deg,tilt_deg
0,-60.0000,10.0000
10,-60.0000,10.0000
```

### `data/capture/`

A 20-second recording of both connections of one camera, exactly as a server read
them, together with what really happened. The images are small (200 × 100) to
keep the files small. Use it to test your parsing, your clock and your pose maths
before you connect to anything.

| File | What it holds |
|------|---------------|
| `video.bin` | Every byte read from the video connection |
| `video-reads.csv` | Where each `read()` started, how many bytes it returned, and when |
| `control.bin` | Every byte read from the control connection |
| `control-reads.csv` | The same, for `control.bin` |
| `truth.csv` | For every frame the camera captured: what happened to it, its true exposure time and its true pan and tilt |
| `events.csv` | What went wrong, and when |

The reads files have the columns `connection,offset,bytes,rx_us`. `rx_us` is the
server's `CLOCK_MONOTONIC` in microseconds when the read returned, and
`connection` goes up by one each time the server reconnected. A new connection
starts a fresh byte stream, so don't carry half a frame from one connection into
the next.

`truth.csv` uses the same clock, so its `exposure_start_us` is the answer your
clock model should give for each frame. Its `pan_deg` and `tilt_deg` are the true
angles at the middle of the exposure. Its `fate` column says what happened to the
frame:

| Fate | Meaning |
|------|---------|
| `sent` | In `video.bin`, intact |
| `corrupt` | In `video.bin`, with a damaged image |
| `cut` | The connection closed partway through it |
| `dropped_by_camera` | The camera dropped it because the server didn't read fast enough |
| `no_client` | Captured while no server was connected, so never sent |

What happens during the recording:

| Time | Event |
|------|-------|
| 0 s | The server connects; the camera is playing the recorded motion |
| 4 s | The server stops reading for 200 ms, and the camera drops two frames |
| 5 s | The camera's clock wraps back to 0 |
| 7 s | Three frames arrive with damaged images |
| 9 s | 5000 garbage bytes arrive between two frames |
| 11 s | The video connection drops in the middle of a frame, and comes back at 12 s |
| 14 s | The camera restarts: new boot number, new clock, not homed |
| 16 s | The server tells the camera to home |

During the recording the server sent a `TIME_REQ` four times a second, with its own
`CLOCK_MONOTONIC` in microseconds as the echo value. It also sent one home command.
What the server sent is not in the files, but the camera's replies are.

To read the capture the way a server would, feed each read to your parser in
order:

```python
import csv

video = open("data/capture/video.bin", "rb").read()
for row in csv.DictReader(open("data/capture/video-reads.csv")):
    start, size = int(row["offset"]), int(row["bytes"])
    chunk = video[start:start + size]   # what one read() returned at row["rx_us"]
    # give `chunk` to your parser for connection row["connection"]
```

Things to keep in mind:

- The `.bin` files are raw byte streams, not lists of records. A frame or a message
  can be split across reads, and one read can hold several control messages.
- Times inside the camera's messages are on the camera's clock. Times in the CSV
  files are on the server's clock.
- Match frames across files by boot number and frame number, never by position.

## Reference: message formats

All numbers are little-endian, and `f32` is a 32-bit IEEE float. Checksums are
CRC-32 with the standard polynomial, the same as zlib's `crc32()` starting from 0.

### Video frames

Each frame is a 32-byte header followed by `width × height` bytes of image.

| Offset | Type | Field | Meaning |
|--------|------|-------|---------|
| 0 | u32 | `start_marker` | Always `0x4D524656`, the bytes `VFRM`; marks the start of a frame |
| 4 | u32 | `frame_seq` | Frame number since the camera started; goes up by one for every frame captured, including the ones that were never sent |
| 8 | u32 | `exposure_start` | The camera's clock, in µs, when the exposure started |
| 12 | u16 | `boot_id` | Goes up by one each time the camera starts |
| 14 | u16 | `width` | Image width in pixels |
| 16 | u16 | `height` | Image height in pixels |
| 18 | 6 bytes | reserved | Zero |
| 24 | u32 | `payload_crc32` | Checksum of the image |
| 28 | u32 | `header_crc32` | Checksum of header bytes 0 to 27 |

The first 12 bytes of the image are the truth stamp: the true pan and the true tilt
as `i32` in thousandths of a degree, then the `frame_seq` as a `u32`.

### Control messages

Each message is a 16-byte header followed by its payload.

| Offset | Type | Field | Meaning |
|--------|------|-------|---------|
| 0 | u32 | `start_marker` | Always `0x4C435450`, the bytes `PTCL`; marks the start of a message |
| 4 | u8 | `version` | Always 1 |
| 5 | u8 | `type` | Which message this is (see the table below) |
| 6 | u16 | `payload_bytes` | Length of the payload |
| 8 | u32 | `msg_seq` | The sender's message counter, which goes up by one per message |
| 12 | u32 | `crc32` | Checksum of header bytes 0 to 11 followed by the payload |

| Type | Message | Payload size |
|------|---------|--------------|
| `0x01` | `HELLO` | 64 bytes |
| `0x02` | `POSE` | 16 bytes |
| `0x03` | `TIME_RESP` | 16 bytes |
| `0x04` | `ACK` | 8 bytes |
| `0x05` | `STATUS` | 24 bytes |
| `0x81` | `TIME_REQ` | 8 bytes |
| `0x82` | `CMD` | 16 bytes |

**`HELLO`**

| Offset | Type | Field |
|--------|------|-------|
| 0 | 16 chars | `camera_id`, padded with zero bytes |
| 16 | 16 chars | `firmware_version`, padded with zero bytes |
| 32 | u16 | `boot_id` |
| 34 | u8 | `homed`: 1 if homed |
| 35 | u8 | reserved |
| 36 | 6 × f32 | Limits: `pan_min_deg`, `pan_max_deg`, `tilt_min_deg`, `tilt_max_deg`, `pan_max_deg_per_s`, `tilt_max_deg_per_s` |
| 60 | u32 | `exposure_us`: how long each exposure lasts |

**`POSE`**

| Offset | Type | Field |
|--------|------|-------|
| 0 | u32 | `counter`: the camera's clock when the position was measured |
| 4 | i32 | `pan_counts`: pan in tenths of a degree |
| 8 | i32 | `tilt_counts`: tilt in tenths of a degree |
| 12 | u8 | `motion`: 0 idle, 1 homing, 2 moving, 3 jogging, 4 stopping, 5 faulted |
| 13 | u8 | `homed`: 1 if homed; until then, angles are measured from where the camera was when it powered on |
| 14 | u8 | `fault`: 0 none, 1 pan motor stalled, 2 tilt motor stalled |
| 15 | u8 | reserved |

**`TIME_REQ` and `TIME_RESP`**

`TIME_REQ` holds one `u64` value of your choice, which the camera sends back.

| Offset | Type | `TIME_RESP` field |
|--------|------|-------------------|
| 0 | u64 | `echo`: the value from your request |
| 8 | u32 | `t2`: the camera's clock when your request arrived |
| 12 | u32 | `t3`: the camera's clock when it sent this reply |

**`CMD` and `ACK`**

| Offset | Type | `CMD` field |
|--------|------|-------------|
| 0 | u32 | `cmd_id`: any ID you choose; the camera remembers the last 100 |
| 4 | u8 | `cmd`: 1 home, 2 go to, 3 jog, 4 stop, 5 clear fault |
| 5 | 3 bytes | reserved |
| 8 | f32 | `a`: pan in degrees for go to, or pan speed in degrees per second for jog |
| 12 | f32 | `b`: tilt in degrees for go to, or tilt speed in degrees per second for jog |

| Offset | Type | `ACK` field |
|--------|------|-------------|
| 0 | u32 | `cmd_id` of the command it answers |
| 4 | u8 | `result`: 0 accepted, 1 not homed, 2 out of limits, 3 faulted, 4 invalid |
| 5 | u8 | `duplicate`: 1 if the camera had already seen this `cmd_id` |
| 6 | u16 | reserved |

**`STATUS`**

| Offset | Type | Field |
|--------|------|-------|
| 0 | u16 | `boot_id` |
| 2 | u16 | `fps`: frames per second |
| 4 | u32 | `frames_captured` since the camera started |
| 8 | u32 | `frames_sent` |
| 12 | u32 | `frames_dropped`: captured but not sent, because the server did not keep up |
| 16 | u16 | `width` |
| 18 | u16 | `height` |
| 20 | u32 | reserved |
