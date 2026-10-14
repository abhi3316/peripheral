// The camera simulator: acts as a pan-tilt camera on TCP 7001 (control) and
// 7002 (video), with the admin API on HTTP 7000 for faults and the truth.
// README.md, "The camera simulator", describes it.

import { createServer as createHttp, type IncomingMessage, type ServerResponse } from 'node:http';
import { createServer as createTcp, type Server, type Socket } from 'node:net';
import { randomBytes } from 'node:crypto';
import {
  ControlParser,
  MSG,
  crc32,
  decodeCmd,
  encodeAck,
  encodeControl,
  encodeFrameHeader,
  encodeHello,
  encodePose,
  encodeStatus,
  encodeTimeResp,
  TWO32,
  VIDEO_HEADER_BYTES,
} from '../protocol/wire.ts';
import { CameraModel, LIMITS, MOTION_CODE } from './model.ts';
import type { MotionTrack } from './motion.ts';
import { Panorama } from './texture.ts';

export const nowUs = (): number => Number(process.hrtime.bigint() / 1000n);

// The camera's stream: 1000 × 625 at 25 frames/s, 125 Mb/s of video.
export const STREAM = { width: 1000, height: 625, fps: 25 };

export interface SimulatorOptions {
  host: string;
  controlPort: number;
  videoPort: number;
  adminPort: number;
  cameraId: string;
  width: number;
  height: number;
  fps: number;
  counterStart: number;
  ppm: number;
  exposureUs: number;
  motion: MotionTrack | null; // played in a loop until a command
  autoplay: boolean; // play the motion at start, or wait for /admin/play
  motionOffsetUs: number; // where in the motion to start
  quiet: boolean;
}

// cut: the connection closed before the camera had finished sending the frame,
// so the server got part of it or nothing.
export type Fate = 'sent' | 'dropped_by_camera' | 'corrupt' | 'no_client' | 'cut';

export interface TruthEntry {
  bootId: number;
  frameSeq: number;
  crc32: number;
  exposureStartMonoUs: number;
  exposureEndMonoUs: number;
  exposureStartCounter: number;
  exposureEndCounter: number;
  panDeg: number;
  tiltDeg: number;
  width: number;
  height: number;
  fate: Fate;
}

const POSE_PERIOD_US = 5_000; // 200 Hz
const STATUS_PERIOD_US = 500_000;
const READOUT_US = 2_000;
const BOOT_US = 1_000_000;
const TRUTH_KEEP = 40_000;
// The camera discards a new frame when this many frames wait in its send queue.
const SEND_QUEUE_FRAMES = 3;

interface Refuse {
  untilUs: number;
}

export class CameraSimulator {
  readonly model: CameraModel;
  private pano: Panorama;
  private width: number;
  private height: number;
  private fps: number;
  private exposureUs: number;
  private frameSeq = 0;
  private droppedByCamera = 0;
  private framesCaptured = 0;
  private framesSent = 0;
  private bytesSent = 0;
  private nextExposureUs: number;
  private nextPoseUs: number;
  private nextStatusUs: number;
  private video: Socket | null = null;
  private control: Socket | null = null;
  private videoRefuse: Refuse = { untilUs: 0 };
  private controlRefuse: Refuse = { untilUs: 0 };
  private bootingUntilUs = 0;
  private corruptNext = 0;
  private garbageNext = 0;
  private ctlSeq = 0;
  private acks = new Map<number, number>();
  private truth: TruthEntry[] = [];
  private unsent = new Map<Socket, Set<TruthEntry>>();
  private servers: Server[] = [];
  private http: ReturnType<typeof createHttp> | null = null;
  private timer: NodeJS.Timeout | null = null;
  private rateWindow: { us: number; bytes: number }[] = [];

  constructor(readonly opts: SimulatorOptions) {
    const t = nowUs();
    const first = opts.motion?.at(opts.motionOffsetUs);
    this.model = new CameraModel(t, { counterStart: opts.counterStart, ppm: opts.ppm, pan: first?.pan, tilt: first?.tilt });
    this.width = opts.width;
    this.height = opts.height;
    this.fps = opts.fps;
    this.exposureUs = opts.exposureUs;
    this.pano = new Panorama(this.width, this.height);
    this.nextExposureUs = t;
    this.nextPoseUs = t;
    this.nextStatusUs = t;
  }

  private log(s: string): void {
    if (!this.opts.quiet) console.log(`[camera] ${s}`);
  }

  // ---- lifecycle --------------------------------------------------------------

  async start(): Promise<{ control: number; video: number; admin: number }> {
    const listen = (s: Server | ReturnType<typeof createHttp>, port: number) =>
      new Promise<number>((resolve, reject) => {
        s.once('error', reject);
        s.listen(port, this.opts.host, () => {
          const a = s.address();
          resolve(typeof a === 'object' && a ? a.port : port);
        });
      });
    const ctl = createTcp((s) => this.onControl(s));
    const vid = createTcp((s) => this.onVideo(s));
    this.servers.push(ctl, vid);
    this.http = createHttp((req, res) => void this.admin(req, res).catch((e: unknown) => this.json(res, 500, { error: String(e) })));
    const ports = {
      control: await listen(ctl, this.opts.controlPort),
      video: await listen(vid, this.opts.videoPort),
      admin: await listen(this.http, this.opts.adminPort),
    };
    if (this.opts.motion && this.opts.autoplay) this.model.play(this.opts.motion, this.opts.motionOffsetUs);
    this.timer = setInterval(() => this.tick(), 1);
    return ports;
  }

  async stop(): Promise<void> {
    if (this.timer) clearInterval(this.timer);
    this.video?.destroy();
    this.control?.destroy();
    for (const s of this.servers) s.close();
    await new Promise<void>((r) => (this.http ? this.http.close(() => r()) : r()));
  }

  // ---- connections ----------------------------------------------------------------

  private refused(r: Refuse): boolean {
    return nowUs() < Math.max(r.untilUs, this.bootingUntilUs);
  }

  private onVideo(s: Socket): void {
    if (this.refused(this.videoRefuse)) return void s.destroy();
    this.video?.destroy();
    s.setNoDelay(true);
    this.video = s;
    const unsent = new Set<TruthEntry>();
    this.unsent.set(s, unsent);
    s.on('close', () => {
      if (this.video === s) this.video = null;
      for (const e of unsent) e.fate = 'cut';
      this.unsent.delete(s);
    });
    s.on('error', () => s.destroy());
    s.on('data', () => {}); // the server must not write here; ignore
    this.log('video client connected');
  }

  private onControl(s: Socket): void {
    if (this.refused(this.controlRefuse)) return void s.destroy();
    this.control?.destroy();
    s.setNoDelay(true);
    this.control = s;
    const parser = new ControlParser();
    s.on('data', (d: Buffer) => {
      for (const m of parser.push(d)) this.onControlMessage(m.type, m.payload);
    });
    s.on('close', () => {
      if (this.control === s) {
        this.control = null;
        this.model.jogStop(); // safety: no control link, no jog
      }
    });
    s.on('error', () => s.destroy());
    this.sendHello();
    this.log('control client connected');
  }

  private sendControl(type: number, payload: Buffer): void {
    const c = this.control;
    if (!c || c.destroyed) return;
    c.write(encodeControl(type, this.ctlSeq++, payload));
  }

  private sendHello(): void {
    this.sendControl(
      MSG.HELLO,
      encodeHello({
        cameraId: this.opts.cameraId,
        firmwareVersion: 'sim-1.0.0',
        bootId: this.model.bootId,
        homed: this.model.homed,
        ...LIMITS,
        exposureUs: this.exposureUs,
      }),
    );
  }

  private onControlMessage(type: number, p: Buffer): void {
    const t = nowUs();
    if (type === MSG.TIME_REQ && p.length >= 8) {
      const t2 = this.model.counterAt(t);
      const echo = p.readBigUInt64LE(0);
      this.sendControl(MSG.TIME_RESP, encodeTimeResp(echo, t2, this.model.counterAt(nowUs())));
    } else if (type === MSG.CMD && p.length >= 16) {
      const c = decodeCmd(p);
      const prev = this.acks.get(c.cmdId);
      if (prev !== undefined) return this.sendControl(MSG.ACK, encodeAck(c.cmdId, prev, true));
      const result = this.model.command(c.cmd, c.a, c.b, t);
      this.acks.set(c.cmdId, result);
      if (this.acks.size > 100) this.acks.delete(this.acks.keys().next().value!);
      this.sendControl(MSG.ACK, encodeAck(c.cmdId, result, false));
    }
  }

  // ---- the camera's loop --------------------------------------------------------------

  private tick(): void {
    const t = nowUs();
    this.model.step(t);
    if (t < this.bootingUntilUs) return;
    while (t >= this.nextPoseUs) {
      this.sendPose(this.nextPoseUs);
      this.nextPoseUs += POSE_PERIOD_US;
    }
    const period = 1e6 / this.fps;
    // After a long pause (a texture rebuild), skip ahead rather than send a burst.
    if (t - this.nextExposureUs > 1e6) this.nextExposureUs = t;
    while (t >= this.nextExposureUs + this.exposureUs + READOUT_US) {
      this.capture(this.nextExposureUs);
      this.nextExposureUs += period;
    }
    if (t >= this.nextStatusUs) {
      this.sendControl(
        MSG.STATUS,
        encodeStatus({
          bootId: this.model.bootId,
          fps: this.fps,
          framesCaptured: this.framesCaptured,
          framesSent: this.framesSent,
          framesDropped: this.droppedByCamera,
          width: this.width,
          height: this.height,
        }),
      );
      this.nextStatusUs = t + STATUS_PERIOD_US;
    }
  }

  private sendPose(atUs: number): void {
    const enc = this.model.encoderAt(atUs);
    this.sendControl(
      MSG.POSE,
      encodePose({
        counter: this.model.counterAt(atUs),
        panCounts: enc.panCounts,
        tiltCounts: enc.tiltCounts,
        motion: MOTION_CODE[this.model.motion],
        homed: this.model.homed,
        fault: this.model.fault === 'stall_pan' ? 1 : this.model.fault === 'stall_tilt' ? 2 : 0,
      }),
    );
  }

  private capture(startUs: number): void {
    const endUs = startUs + this.exposureUs;
    const mid = this.model.truePose(startUs + this.exposureUs / 2);
    const seq = this.frameSeq++;
    this.framesCaptured++;
    const n = this.width * this.height;
    const entry: TruthEntry = {
      bootId: this.model.bootId,
      frameSeq: seq,
      crc32: 0,
      exposureStartMonoUs: startUs,
      exposureEndMonoUs: endUs,
      exposureStartCounter: this.model.counterAt(startUs),
      exposureEndCounter: this.model.counterAt(endUs),
      panDeg: mid.pan,
      tiltDeg: mid.tilt,
      width: this.width,
      height: this.height,
      fate: 'sent',
    };
    this.remember(entry);
    const v = this.video;
    if (!v || v.destroyed) {
      entry.fate = 'no_client';
      return;
    }
    if (v.writableLength > SEND_QUEUE_FRAMES * (n + VIDEO_HEADER_BYTES)) {
      this.droppedByCamera++;
      entry.fate = 'dropped_by_camera';
      return;
    }
    const payload = Buffer.allocUnsafe(n);
    this.pano.render(mid.pan, mid.tilt, seq, payload);
    const crc = crc32(payload);
    entry.crc32 = crc;
    if (this.corruptNext > 0) {
      this.corruptNext--;
      payload[n >> 1] = payload[n >> 1]! ^ 0xff;
      entry.fate = 'corrupt';
    }
    if (this.garbageNext > 0) {
      v.write(randomBytes(this.garbageNext));
      this.garbageNext = 0;
    }
    const header = encodeFrameHeader({
      frameSeq: seq,
      bootId: this.model.bootId,
      exposureStart: entry.exposureStartCounter,
      width: this.width,
      height: this.height,
      payloadCrc32: crc,
    });
    // A frame counts as sent once all of it is handed to the kernel; until then a
    // closed connection cuts it.
    const unsent = this.unsent.get(v);
    unsent?.add(entry);
    v.cork();
    v.write(header);
    v.write(payload, (err) => {
      if (!err) unsent?.delete(entry);
    });
    v.uncork();
    this.framesSent++;
    this.bytesSent += n + VIDEO_HEADER_BYTES;
    this.rateWindow.push({ us: nowUs(), bytes: n + VIDEO_HEADER_BYTES });
  }

  private remember(e: TruthEntry): void {
    this.truth.push(e);
    if (this.truth.length > TRUTH_KEEP) this.truth.splice(0, this.truth.length - TRUTH_KEEP);
  }

  // ---- controls -------------------------------------------------------------------

  fault(f: Record<string, unknown>): string | null {
    const t = nowUs();
    switch (f.kind) {
      case 'corrupt':
        this.corruptNext += Math.max(1, Number(f.count ?? 1));
        return null;
      case 'garbage':
        this.garbageNext = Math.max(1, Math.min(1_000_000, Number(f.bytes ?? 1000)));
        return null;
      case 'video_down':
        this.videoRefuse.untilUs = t + Number(f.ms ?? 3000) * 1000;
        this.video?.destroy();
        return null;
      case 'control_down':
        this.controlRefuse.untilUs = t + Number(f.ms ?? 3000) * 1000;
        this.control?.destroy();
        return null;
      case 'reboot': {
        // The camera comes back idle and not homed, as a real one does.
        // Like a real camera, the clock starts again from an unknown value.
        const start = f.counterStart === undefined ? Math.floor(Math.random() * TWO32) : Number(f.counterStart);
        const ppm = f.ppm === undefined ? this.model.ppm : Number(f.ppm);
        this.video?.destroy();
        this.control?.destroy();
        this.bootingUntilUs = t + BOOT_US;
        this.model.reboot(t + BOOT_US, start, ppm);
        this.frameSeq = 0;
        this.droppedByCamera = 0;
        this.framesCaptured = 0;
        this.framesSent = 0;
        this.acks.clear();
        this.nextExposureUs = t + BOOT_US;
        this.nextPoseUs = t + BOOT_US;
        this.log(`reboot → boot ${this.model.bootId}, counter ${start}, ${ppm} ppm`);
        return null;
      }
      case 'stall':
        if (f.axis === 'tilt') this.model.tilt.stalled = true;
        else this.model.pan.stalled = true;
        return null;
      case 'unstall':
        this.model.pan.stalled = false;
        this.model.tilt.stalled = false;
        return null;
      default:
        return 'kind: corrupt | garbage | video_down | control_down | reboot | stall | unstall';
    }
  }

  stats(): Record<string, unknown> {
    const t = nowUs();
    this.rateWindow = this.rateWindow.filter((r) => r.us > t - 1e6);
    return {
      cameraId: this.opts.cameraId,
      bootId: this.model.bootId,
      stream: { width: this.width, height: this.height, fps: this.fps },
      frameBytes: this.width * this.height + VIDEO_HEADER_BYTES,
      framesCaptured: this.framesCaptured,
      framesSent: this.framesSent,
      droppedByCamera: this.droppedByCamera,
      bytesSent: this.bytesSent,
      sendBytesPerS: this.rateWindow.reduce((s, r) => s + r.bytes, 0),
      sendMbitPerS: (this.rateWindow.reduce((s, r) => s + r.bytes, 0) * 8) / 1e6,
      videoClient: !!this.video,
      controlClient: !!this.control,
      homed: this.model.homed,
      motion: this.model.motion,
      playing: this.model.playing,
      fault: this.model.fault,
      pose: { panDeg: this.model.pan.pos, tiltDeg: this.model.tilt.pos },
      counter: this.model.counterAt(t),
      ppm: this.model.ppm,
      monoUs: t,
    };
  }

  truthRange(bootId: number, fromSeq: number, toSeq: number): TruthEntry[] {
    return this.truth.filter((e) => e.bootId === bootId && e.frameSeq >= fromSeq && e.frameSeq <= toSeq);
  }

  // ---- admin HTTP ---------------------------------------------------------------------

  private json(res: ServerResponse, status: number, body: unknown): void {
    res.writeHead(status, { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' });
    res.end(JSON.stringify(body));
  }

  private async body(req: IncomingMessage): Promise<Record<string, unknown>> {
    const chunks: Buffer[] = [];
    for await (const c of req) chunks.push(c as Buffer);
    return chunks.length ? (JSON.parse(Buffer.concat(chunks).toString()) as Record<string, unknown>) : {};
  }

  private async admin(req: IncomingMessage, res: ServerResponse): Promise<void> {
    const url = new URL(req.url ?? '/', 'http://x');
    if (req.method === 'OPTIONS') {
      res.writeHead(204, { 'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Methods': 'GET, POST', 'Access-Control-Allow-Headers': 'Content-Type' });
      return void res.end();
    }
    if (req.method === 'GET' && url.pathname === '/admin/stats') return this.json(res, 200, this.stats());
    if (req.method === 'GET' && url.pathname === '/admin/truth') {
      const q = url.searchParams;
      if (q.has('fromUs')) {
        // Every boot: frames whose exposure started in [fromUs, toUs] (host CLOCK_MONOTONIC µs).
        const fromUs = Number(q.get('fromUs'));
        const toUs = Number(q.get('toUs') ?? Number.MAX_SAFE_INTEGER);
        return this.json(res, 200, { frames: this.truth.filter((e) => e.exposureStartMonoUs >= fromUs && e.exposureStartMonoUs <= toUs) });
      }
      const bootId = Number(q.get('bootId') ?? this.model.bootId);
      const from = Number(q.get('fromSeq') ?? 0);
      const to = Number(q.get('toSeq') ?? from + 9999);
      return this.json(res, 200, { frames: this.truthRange(bootId, from, Math.min(to, from + 49_999)) });
    }
    if (req.method === 'POST' && url.pathname === '/admin/fault') {
      const err = this.fault(await this.body(req));
      return err ? this.json(res, 400, { error: err }) : this.json(res, 200, { ok: true });
    }
    if (req.method === 'POST' && url.pathname === '/admin/play') {
      const b = await this.body(req);
      if (b.stop) this.model.stopPlayback();
      else if (!this.opts.motion) return this.json(res, 400, { error: 'no motion: the camera simulator found no data/motion.csv when it started' });
      else this.model.play(this.opts.motion, this.opts.motionOffsetUs);
      return this.json(res, 200, { ok: true });
    }
    return this.json(res, 404, { error: 'not found' });
  }
}
