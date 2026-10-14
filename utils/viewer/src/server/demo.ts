// A fake server with two cameras, so that the viewer runs without one (open
// the page with ?demo). It shows the kind of state the viewer expects. Its
// numbers are made up.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { ServerLink } from './link.ts';
import type { ActionResult, CameraCommand, CameraState, Gap, Preview, RecordingState, ServerEvent, ServerState } from './types.ts';

const LIMITS = { panMinDeg: -170, panMaxDeg: 170, tiltMinDeg: -30, tiltMaxDeg: 90, panMaxDegPerS: 100, tiltMaxDegPerS: 50 };
const RATE = 15_625_000; // bytes per second for one camera: 125 Mb/s
const FPS = 25;
const PW = 320;
const PH = 200;
// Bright and dark discs in the scene: azimuth, elevation, radius (degrees), grey value.
const LANDMARKS: [number, number, number, number][] = [
  [0, 0, 3, 255],
  [45, 20, 5, 20],
  [-90, 10, 6, 245],
  [100, 45, 7, 30],
  [-40, 50, 4, 250],
];

const MISSING: ActionResult = { ok: false, reason: 'demo not running' };

const nowUs = () => Math.round(performance.now() * 1000);
const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, v));
const wall = (us: number) => new Date(Date.now() - (nowUs() - us) / 1000).toISOString();

type Mode =
  | { kind: 'sweep' }
  | { kind: 'idle' }
  | { kind: 'homing' }
  | { kind: 'goto'; pan: number; tilt: number }
  | { kind: 'jog'; pan: number; tilt: number; untilUs: number };

class FakeCamera {
  pan = 0;
  tilt = 0;
  homed = true;
  frameSeq: number;
  private vel = { pan: 0, tilt: 0 };
  private mode: Mode = { kind: 'sweep' };

  constructor(
    readonly cameraId: string,
    private readonly phase: number, // where in its sweep this camera starts
    frameSeq: number,
    private readonly clock: { offsetUs: number; driftPpm: number },
  ) {
    this.frameSeq = frameSeq;
  }

  step(t: number, dt: number, started: number): number {
    const prev = { pan: this.pan, tilt: this.tilt };
    const m = this.mode;
    if (m.kind === 'sweep') {
      const s = (t - started) / 1e6 + this.phase;
      this.pan = 100 * Math.sin(s / 4);
      this.tilt = 20 + 20 * Math.sin(s / 3);
    } else if (m.kind === 'goto' || m.kind === 'homing') {
      const to = m.kind === 'goto' ? m : { pan: 0, tilt: 0 };
      this.pan += clamp(to.pan - this.pan, -LIMITS.panMaxDegPerS * dt, LIMITS.panMaxDegPerS * dt);
      this.tilt += clamp(to.tilt - this.tilt, -LIMITS.tiltMaxDegPerS * dt, LIMITS.tiltMaxDegPerS * dt);
      if (this.pan === to.pan && this.tilt === to.tilt) {
        if (m.kind === 'homing') this.homed = true;
        this.mode = { kind: 'idle' };
      }
    } else if (m.kind === 'jog') {
      if (t > m.untilUs) this.mode = { kind: 'idle' };
      else {
        this.pan = clamp(this.pan + m.pan * dt, LIMITS.panMinDeg, LIMITS.panMaxDeg);
        this.tilt = clamp(this.tilt + m.tilt * dt, LIMITS.tiltMinDeg, LIMITS.tiltMaxDeg);
      }
    }
    this.vel = { pan: (this.pan - prev.pan) / dt, tilt: (this.tilt - prev.tilt) / dt };
    const frames = Math.round(FPS * dt);
    this.frameSeq += frames;
    return frames;
  }

  render(t: number, recorded: boolean): Preview {
    const px = new Uint8Array(PW * PH);
    const pxPerDeg = PW / 48;
    for (let y = 0; y < PH; y++) {
      const el = this.tilt + (PH / 2 - y) / pxPerDeg;
      for (let x = 0; x < PW; x++) {
        const az = this.pan + (x - PW / 2) / pxPerDeg;
        let v = el > 0 ? 150 + Math.round(el) : 80 + (((Math.floor(az / 5) + Math.floor(el / 5)) & 1) * 20);
        if (Math.abs(((az % 15) + 15) % 15) < 0.2) v = 220;
        if (Math.abs(el) < 0.3) v = 0;
        for (const [laz, lel, r, lv] of LANDMARKS) if (Math.hypot(az - laz, el - lel) < r) v = lv;
        px[y * PW + x] = v;
      }
    }
    // The fake truth stamp: the "true" pose, a little different from the reported one.
    const stamp = new Uint8Array(12);
    const v = new DataView(stamp.buffer);
    v.setInt32(0, Math.round((this.pan + 0.03) * 1000), true);
    v.setInt32(4, Math.round((this.tilt - 0.02) * 1000), true);
    v.setUint32(8, this.frameSeq, true);
    return {
      cameraId: this.cameraId,
      bootId: 3,
      frameSeq: this.frameSeq,
      width: PW,
      height: PH,
      pixels: px,
      sourceWidth: 1000,
      sourceHeight: 625,
      exposureStartUs: t - 18_000,
      exposureEndUs: t - 8_000,
      pose: { panDeg: this.pan, tiltDeg: this.tilt },
      homed: this.homed,
      recorded,
      truthStamp: stamp,
    };
  }

  state(t: number): CameraState {
    const m = this.mode;
    return {
      cameraId: this.cameraId,
      link: 'connected',
      videoLink: 'connected',
      lastFrameUs: t - 20_000,
      bootId: 3,
      firmwareVersion: '2.4.0',
      homed: this.homed,
      motion: m.kind === 'sweep' || m.kind === 'goto' ? 'moving' : m.kind === 'jog' ? 'jogging' : m.kind,
      fault: null,
      pose: { panDeg: this.pan, tiltDeg: this.tilt, atUs: t - 3_000 },
      velocity: { panDegPerS: this.vel.pan, tiltDegPerS: this.vel.tilt },
      target: m.kind === 'goto' ? { panDeg: m.pan, tiltDeg: m.tilt } : null,
      limits: LIMITS,
      stream: { width: 1000, height: 625, fps: FPS },
      clock: { ...this.clock, uncertaintyUs: 40 },
      counters: {
        framesReceived: this.frameSeq - 7_500,
        framesDamaged: 3,
        framesMissing: 28,
        framesDroppedByCamera: 2,
        framesDroppedInServer: 0,
        resyncBytes: 5_000,
      },
    };
  }

  command(c: CameraCommand): ActionResult {
    if (c.command === 'stop') this.mode = { kind: 'idle' };
    else if (c.command === 'home') this.mode = { kind: 'homing' };
    else if (c.command === 'goto') {
      if (!this.homed) return { ok: false, reason: 'not_homed' };
      if (c.panDeg < LIMITS.panMinDeg || c.panDeg > LIMITS.panMaxDeg || c.tiltDeg < LIMITS.tiltMinDeg || c.tiltDeg > LIMITS.tiltMaxDeg) {
        return { ok: false, reason: 'out_of_limits' };
      }
      this.mode = { kind: 'goto', pan: c.panDeg, tilt: c.tiltDeg };
    }
    return { ok: true };
  }

  jog(pan: number, tilt: number): void {
    if (!this.homed) return;
    this.mode = pan === 0 && tilt === 0 ? { kind: 'idle' } : { kind: 'jog', pan, tilt, untilUs: nowUs() + 300_000 };
  }
}

class DemoServer {
  private cameras = [
    new FakeCamera('cam0', 0, 18_000, { offsetUs: 1_234_567, driftPpm: 49.6 }),
    new FakeCamera('cam1', 40, 9_000, { offsetUs: -2_345_678, driftPpm: -30.4 }),
  ];
  private lastUs = nowUs();
  private buffer = { used: 0, high: 0 };
  private diskUsed = 600_000_000;
  private backlog = { objects: 0, bytes: 0, oldestUs: null as number | null };
  private storageUp = true;
  private lastContactUs = nowUs();
  private recording: RecordingState | null = null;
  private history: RecordingState[];
  private events: ServerEvent[] = [];
  private eventId = 0;
  private watching: { cameraId: string | null; maxFps: number } = { cameraId: null, maxFps: 0 };
  private lastPreviewUs = 0;
  private preview: { image: Preview; receivedAtMs: number } | null = null;
  private timer: ReturnType<typeof setInterval>;
  private started = nowUs();

  constructor(private readonly onChange: () => void) {
    const t = nowUs();
    const old = (id: string, agoS: number, frames: number, complete: boolean, gaps: Gap[]): RecordingState => ({
      recordingId: id,
      state: 'stopped',
      startedAtUs: t - agoS * 1e6,
      startedAtWall: wall(t - agoS * 1e6),
      stoppedAtUs: t - agoS * 1e6 + (frames / FPS / 2) * 1e6,
      frames,
      bytes: frames * (RATE / FPS),
      gaps,
      complete,
      upload: 'uploaded',
    });
    this.history = [
      old('r-0042', 900, 3000, true, [{ cameraId: 'cam0', bootId: 3, fromSeq: 16_110, toSeq: 16_111, reason: 'damaged' }]),
      old('r-0041', 3600, 4500, false, [{ cameraId: 'cam1', bootId: 3, fromSeq: 4_800, toSeq: 4_826, reason: 'link_down' }]),
    ];
    this.event('info', 'demo server started');
    this.timer = setInterval(() => this.tick(), 100);
  }

  stop(): void {
    clearInterval(this.timer);
  }

  private event(level: ServerEvent['level'], message: string, cameraId: string | null = null): void {
    this.events = [{ id: ++this.eventId, atUs: nowUs(), level, cameraId, message }, ...this.events].slice(0, 100);
  }

  private camera(id: string): FakeCamera | undefined {
    return this.cameras.find((c) => c.cameraId === id);
  }

  // ---- the fake cameras and pipeline ------------------------------------------------

  private tick(): void {
    const t = nowUs();
    const dt = (t - this.lastUs) / 1e6;
    this.lastUs = t;
    let frames = 0;
    for (const c of this.cameras) frames += c.step(t, dt, this.started);

    this.buffer.used = Math.max(0, Math.round(this.buffer.used * 0.6 + Math.random() * 4_000_000));
    this.buffer.high = Math.max(this.buffer.high, this.buffer.used);

    // The cloud storage is away for 10 s in every 60 s.
    const storageUp = Math.floor((t - this.started) / 1e6) % 60 < 50;
    if (storageUp !== this.storageUp) {
      this.storageUp = storageUp;
      this.event(storageUp ? 'info' : 'warning', storageUp ? 'cloud storage connected' : 'cloud storage unreachable; uploads wait');
    }
    if (this.storageUp) {
      this.lastContactUs = t;
      const n = Math.min(this.backlog.objects, 1);
      this.backlog.objects -= n;
      this.backlog.bytes = Math.max(0, this.backlog.bytes - n * 50_000_000);
      this.diskUsed = Math.max(600_000_000, this.diskUsed - n * 50_000_000);
      if (this.backlog.objects === 0) this.backlog.oldestUs = null;
    }

    const r = this.recording;
    if (r) {
      r.frames += frames;
      r.bytes += frames * (RATE / FPS);
      this.diskUsed += frames * (RATE / FPS);
      if (Math.random() < 0.01) {
        const c = this.cameras[Math.floor(Math.random() * this.cameras.length)]!;
        r.gaps.push({ cameraId: c.cameraId, bootId: 3, fromSeq: c.frameSeq, toSeq: c.frameSeq, reason: 'dropped_by_camera' });
        this.event('warning', `${c.cameraId}: frame ${c.frameSeq} dropped by the camera`, c.cameraId);
      }
      if (Math.random() < 0.06) {
        this.backlog.objects++;
        this.backlog.bytes += 50_000_000;
        this.backlog.oldestUs ??= t;
      }
    }
    for (const h of this.history) if (h.upload === 'pending' && this.storageUp && this.backlog.objects === 0) h.upload = 'uploaded';

    const w = this.watching;
    const watched = w.cameraId ? this.camera(w.cameraId) : undefined;
    if (watched && w.maxFps > 0 && t - this.lastPreviewUs >= 1e6 / w.maxFps - 5_000) {
      this.lastPreviewUs = t;
      this.preview = { image: watched.render(t, this.recording !== null), receivedAtMs: performance.now() };
    }
    this.onChange();
  }

  // ---- what the page reads ----------------------------------------------------------

  view(): { state: ServerState; events: ServerEvent[]; preview: DemoServer['preview'] } {
    const t = nowUs();
    const alarms: ServerState['alarms'] = this.storageUp
      ? []
      : [{ code: 'storage_unreachable', severity: 'warning', cameraId: null, message: `Cloud storage unreachable: ${this.backlog.objects} objects wait`, sinceUs: this.lastContactUs }];
    const rate = RATE * this.cameras.length;
    return {
      state: {
        serverId: 'demo',
        measuredAtUs: t,
        cameras: this.cameras.map((c) => c.state(t)),
        pipeline: {
          ingestBytesPerS: rate * (0.99 + Math.random() * 0.02),
          ingestFramesPerS: FPS * this.cameras.length,
          buffer: { capacityBytes: 64_000_000, usedBytes: this.buffer.used, highWaterBytes: this.buffer.high },
          disk: { quotaBytes: 2_000_000_000, usedBytes: this.diskUsed, writeBytesPerS: this.recording ? rate : 0 },
          memoryBytes: 120_000_000,
        },
        upload: {
          storage: this.storageUp ? 'connected' : 'disconnected',
          lastContactUs: this.lastContactUs,
          backlogObjects: this.backlog.objects,
          backlogBytes: this.backlog.bytes,
          oldestPendingUs: this.backlog.oldestUs,
        },
        recording: this.recording ? { ...this.recording, gaps: [...this.recording.gaps] } : null,
        alarms,
      },
      events: this.events,
      preview: this.preview,
    };
  }

  // ---- actions ------------------------------------------------------------------------

  watch(cameraId: string | null, maxFps: number): void {
    this.watching = { cameraId, maxFps };
    if (cameraId === null || this.preview?.image.cameraId !== cameraId) this.preview = null;
  }

  command(cameraId: string, c: CameraCommand): ActionResult {
    const cam = this.camera(cameraId);
    if (!cam) return { ok: false, reason: 'unknown camera' };
    const r = cam.command(c);
    if (r.ok) this.event('info', `${cameraId}: ${c.command} accepted`, cameraId);
    return r;
  }

  jog(cameraId: string, pan: number, tilt: number): void {
    this.camera(cameraId)?.jog(pan, tilt);
  }

  startRecording(): ActionResult {
    if (this.recording) return { ok: false, reason: 'already recording' };
    const t = nowUs();
    const id = `r-${String(43 + this.history.length - 2).padStart(4, '0')}`;
    this.recording = { recordingId: id, state: 'recording', startedAtUs: t, startedAtWall: wall(t), stoppedAtUs: null, frames: 0, bytes: 0, gaps: [], complete: false, upload: 'pending' };
    this.event('info', `recording ${id} started`);
    return { ok: true };
  }

  stopRecording(id: string): ActionResult {
    const r = this.recording;
    if (!r || r.recordingId !== id) return { ok: false, reason: 'not recording' };
    r.state = 'stopped';
    r.stoppedAtUs = nowUs();
    r.complete = true;
    this.history = [r, ...this.history];
    this.recording = null;
    this.event('info', `recording ${id} stopped`);
    return { ok: true };
  }

  recordings(): RecordingState[] {
    return [...(this.recording ? [this.recording] : []), ...this.history].map((r) => ({ ...r, gaps: [...r.gaps] }));
  }
}

export function useDemoServer(enabled: boolean): ServerLink {
  const ref = useRef<DemoServer | null>(null);
  const [view, setView] = useState<ReturnType<DemoServer['view']> | null>(null);
  const [stateAtMs, setStateAtMs] = useState<number | null>(null);

  useEffect(() => {
    if (!enabled) return;
    const d = new DemoServer(() => {
      setView(d.view());
      setStateAtMs(performance.now());
    });
    ref.current = d;
    return () => {
      d.stop();
      ref.current = null;
    };
  }, [enabled]);

  const watch = useCallback((h: string | null, f: number) => ref.current?.watch(h, f), []);
  const command = useCallback(async (h: string, c: CameraCommand) => ref.current?.command(h, c) ?? MISSING, []);
  const jog = useCallback((h: string, p: number, t: number) => ref.current?.jog(h, p, t), []);
  const startRecording = useCallback(async () => ref.current?.startRecording() ?? MISSING, []);
  const stopRecording = useCallback(async (id: string) => ref.current?.stopRecording(id) ?? MISSING, []);
  const listRecordings = useCallback(async () => ref.current?.recordings() ?? [], []);

  return useMemo(
    () => ({
      problem: enabled ? null : 'demo off',
      state: view?.state ?? null,
      stateAtMs,
      events: view?.events ?? [],
      preview: view?.preview ?? null,
      watch,
      command,
      jog,
      startRecording,
      stopRecording,
      listRecordings,
    }),
    [enabled, view, stateAtMs, watch, command, jog, startRecording, stopRecording, listRecordings],
  );
}
