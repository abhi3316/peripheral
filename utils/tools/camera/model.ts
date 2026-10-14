// The camera's mechanics and clock: two axes, homing, goto, jog with a 300 ms
// timeout, playback of a recorded motion, stall detection, a 32-bit µs counter
// with drift, and a history of the TRUE pose so that the simulator can say where
// the camera pointed at any instant.

import { Axis } from './axis.ts';
import type { MotionTrack } from './motion.ts';
import { COUNTS_TO_DEG, TWO32 } from '../protocol/wire.ts';

export const LIMITS = {
  panMinDeg: -170,
  panMaxDeg: 170,
  tiltMinDeg: -30,
  tiltMaxDeg: 90,
  panMaxDegPerS: 100,
  tiltMaxDegPerS: 50,
};
export const PAN_ACCEL = 400;
export const TILT_ACCEL = 200;
export const HOMING_DEG_PER_S = 30;
export const JOG_VALID_US = 300_000;
const STALL_WINDOW_US = 100_000;
const STALL_MIN_DEG_PER_S = 5;
const HISTORY_US = 3_000_000;

// 'playing' is the simulator's own state; on the wire it is 'moving'.
export type Motion = 'idle' | 'homing' | 'moving' | 'jogging' | 'stopping' | 'faulted' | 'playing';
export const MOTION_CODE: Record<Motion, number> = { idle: 0, homing: 1, moving: 2, jogging: 3, stopping: 4, faulted: 5, playing: 2 };

interface Sample {
  us: number;
  pan: number;
  tilt: number;
}

export function interpolate(list: Sample[], us: number): { pan: number; tilt: number } | null {
  if (list.length === 0) return null;
  const first = list[0]!;
  const last = list[list.length - 1]!;
  if (us <= first.us) return { pan: first.pan, tilt: first.tilt };
  if (us >= last.us) return { pan: last.pan, tilt: last.tilt };
  let lo = 0;
  let hi = list.length - 1;
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (list[mid]!.us <= us) lo = mid;
    else hi = mid;
  }
  const a = list[lo]!;
  const b = list[hi]!;
  const f = b.us === a.us ? 0 : (us - a.us) / (b.us - a.us);
  return { pan: a.pan + (b.pan - a.pan) * f, tilt: a.tilt + (b.tilt - a.tilt) * f };
}

export class CameraModel {
  readonly pan: Axis;
  readonly tilt: Axis;
  bootId = 1;
  motion: Motion = 'idle';
  fault: 'stall_pan' | 'stall_tilt' | null = null;
  homedPan = false;
  homedTilt = false;
  target: { pan: number; tilt: number } | null = null;
  private jog: { pan: number; tilt: number; expiresUs: number } | null = null;
  // A recorded motion: first a goto to its start (pending), then playing.
  private playback: { track: MotionTrack; startUs: number | null; offsetUs: number } | null = null;
  private refPan: number; // encoder zero, in true degrees
  private refTilt: number;
  private truth: Sample[] = [];
  private lastUs: number;
  private stallRef: { pan: number; tilt: number; sinceUs: number };
  // Clock
  bootMonoUs: number;
  counterStart: number;
  ppm: number;

  constructor(nowUs: number, opts: { counterStart: number; ppm: number; pan?: number; tilt?: number }) {
    const pan = opts.pan ?? 37.5;
    const tilt = opts.tilt ?? 4;
    this.pan = new Axis({ minDeg: LIMITS.panMinDeg, maxDeg: LIMITS.panMaxDeg, maxDegPerS: LIMITS.panMaxDegPerS, maxDegPerS2: PAN_ACCEL }, pan);
    this.tilt = new Axis({ minDeg: LIMITS.tiltMinDeg, maxDeg: LIMITS.tiltMaxDeg, maxDegPerS: LIMITS.tiltMaxDegPerS, maxDegPerS2: TILT_ACCEL }, tilt);
    this.refPan = pan;
    this.refTilt = tilt;
    this.lastUs = nowUs;
    this.bootMonoUs = nowUs;
    this.counterStart = opts.counterStart >>> 0;
    this.ppm = opts.ppm;
    this.stallRef = { pan, tilt, sinceUs: nowUs };
    this.truth.push({ us: nowUs, pan, tilt });
  }

  get homed(): boolean {
    return this.homedPan && this.homedTilt;
  }

  // ---- clock ------------------------------------------------------------------

  counterExtended(monoUs: number): number {
    return this.counterStart + (monoUs - this.bootMonoUs) * (1 + this.ppm * 1e-6);
  }

  counterAt(monoUs: number): number {
    return Math.floor(this.counterExtended(monoUs)) % TWO32;
  }

  // ---- truth --------------------------------------------------------------------

  truePose(monoUs: number): { pan: number; tilt: number } {
    return interpolate(this.truth, monoUs) ?? { pan: this.pan.pos, tilt: this.tilt.pos };
  }

  // Encoder counts at an instant (relative to power-on until homed).
  encoderAt(monoUs: number): { panCounts: number; tiltCounts: number } {
    const p = this.truePose(monoUs);
    return {
      panCounts: Math.round((p.pan - this.refPan) / COUNTS_TO_DEG),
      tiltCounts: Math.round((p.tilt - this.refTilt) / COUNTS_TO_DEG),
    };
  }

  // ---- motion ---------------------------------------------------------------------

  private raise(code: 'stall_pan' | 'stall_tilt'): void {
    this.fault = code;
    this.motion = 'faulted';
    this.target = null;
    this.jog = null;
    this.playback = null;
    this.pan.cut();
    this.tilt.cut();
  }

  get playing(): boolean {
    return this.playback?.startUs != null;
  }

  private checkStall(nowUs: number): void {
    const moving = (a: Axis) => Math.abs(a.vel) > STALL_MIN_DEG_PER_S;
    if (!moving(this.pan) && !moving(this.tilt)) {
      this.stallRef = { pan: this.pan.pos, tilt: this.tilt.pos, sinceUs: nowUs };
      return;
    }
    if (nowUs - this.stallRef.sinceUs < STALL_WINDOW_US) return;
    const minMove = 2 * COUNTS_TO_DEG;
    if (moving(this.pan) && Math.abs(this.pan.pos - this.stallRef.pan) < minMove) return this.raise('stall_pan');
    if (moving(this.tilt) && Math.abs(this.tilt.pos - this.stallRef.tilt) < minMove) return this.raise('stall_tilt');
    this.stallRef = { pan: this.pan.pos, tilt: this.tilt.pos, sinceUs: nowUs };
  }

  private homeAxis(a: Axis, dt: number, which: 'pan' | 'tilt'): void {
    const before = a.pos;
    a.step(dt, before > 0 ? -HOMING_DEG_PER_S : HOMING_DEG_PER_S);
    if (a.stalled) return;
    if (Math.sign(a.pos) !== Math.sign(before) || Math.abs(a.pos) < 0.05) {
      a.pos = 0;
      a.cut();
      if (which === 'pan') {
        this.refPan = 0;
        this.homedPan = true;
      } else {
        this.refTilt = 0;
        this.homedTilt = true;
      }
    }
  }

  step(nowUs: number): void {
    const dt = Math.min(0.05, Math.max(0, (nowUs - this.lastUs) / 1e6));
    this.lastUs = nowUs;
    if (dt === 0) return;
    if (this.jog && nowUs > this.jog.expiresUs) {
      this.jog = null;
      if (this.motion === 'jogging') this.motion = 'stopping';
    }
    switch (this.motion) {
      case 'faulted':
      case 'idle':
      case 'stopping':
        this.pan.step(dt, 0);
        this.tilt.step(dt, 0);
        if (this.motion === 'stopping' && this.pan.vel === 0 && this.tilt.vel === 0) this.motion = 'idle';
        break;
      case 'homing':
        if (!this.homedPan) this.homeAxis(this.pan, dt, 'pan');
        else this.pan.step(dt, 0);
        if (!this.homedTilt) this.homeAxis(this.tilt, dt, 'tilt');
        else this.tilt.step(dt, 0);
        if (this.homed) this.motion = 'idle';
        break;
      case 'moving': {
        const t = this.target!;
        const pDone = this.pan.step(dt, this.pan.gotoVelocity(t.pan), t.pan) || (this.pan.pos === t.pan && this.pan.vel === 0);
        const tDone = this.tilt.step(dt, this.tilt.gotoVelocity(t.tilt), t.tilt) || (this.tilt.pos === t.tilt && this.tilt.vel === 0);
        if (pDone && tDone) {
          this.target = null;
          this.motion = 'idle';
          if (this.playback) {
            this.playback.startUs = nowUs;
            this.motion = 'playing';
          }
        }
        break;
      }
      case 'playing': {
        const p = this.playback!;
        const at = p.track.at(nowUs - p.startUs! + p.offsetUs);
        if (!this.pan.stalled) this.pan.pos = at.pan;
        this.pan.vel = at.panVel;
        if (!this.tilt.stalled) this.tilt.pos = at.tilt;
        this.tilt.vel = at.tiltVel;
        break;
      }
      case 'jogging':
        this.pan.step(dt, this.jog?.pan ?? 0);
        this.tilt.step(dt, this.jog?.tilt ?? 0);
        break;
    }
    if (this.motion !== 'faulted') this.checkStall(nowUs);
    this.truth.push({ us: nowUs, pan: this.pan.pos, tilt: this.tilt.pos });
    while (this.truth.length > 2 && this.truth[0]!.us < nowUs - HISTORY_US) this.truth.shift();
  }

  private goto(pan: number, tilt: number): void {
    this.target = { pan, tilt };
    this.motion = 'moving';
  }

  // ---- commands (results as in README.md, CMD and ACK) ----------------------------

  command(cmd: number, a: number, b: number, nowUs: number): number {
    const inside = (p: number, t: number) =>
      p >= LIMITS.panMinDeg && p <= LIMITS.panMaxDeg && t >= LIMITS.tiltMinDeg && t <= LIMITS.tiltMaxDeg;
    switch (cmd) {
      case 4: // stop
        this.target = null;
        this.jog = null;
        this.playback = null;
        if (this.motion !== 'faulted' && this.motion !== 'idle') this.motion = 'stopping';
        return 0;
      case 5: // clear_fault
        if (this.motion === 'faulted') {
          this.fault = null;
          this.motion = 'idle';
          this.stallRef = { pan: this.pan.pos, tilt: this.tilt.pos, sinceUs: nowUs };
        }
        return 0;
      case 1: // home
        if (this.fault) return 3;
        this.homedPan = false;
        this.homedTilt = false;
        this.jog = null;
        this.playback = null;
        this.target = null;
        this.motion = 'homing';
        return 0;
      case 2: // goto
        if (!Number.isFinite(a) || !Number.isFinite(b)) return 4;
        if (this.fault) return 3;
        if (!this.homed) return 1;
        if (!inside(a, b)) return 2;
        this.jog = null;
        this.playback = null;
        this.goto(a, b);
        return 0;
      case 3: {
        // jog
        if (!Number.isFinite(a) || !Number.isFinite(b)) return 4;
        if (this.fault) return 3;
        if (!this.homed) return 1;
        const clamp = (v: number, m: number) => Math.max(-m, Math.min(m, v));
        const pan = clamp(a, LIMITS.panMaxDegPerS);
        const tilt = clamp(b, LIMITS.tiltMaxDegPerS);
        this.playback = null;
        this.target = null;
        if (pan === 0 && tilt === 0) {
          this.jogStop();
          return 0;
        }
        this.jog = { pan, tilt, expiresUs: nowUs + JOG_VALID_US };
        this.motion = 'jogging';
        return 0;
      }
      default:
        return 4;
    }
  }

  jogStop(): void {
    if (!this.jog) return;
    this.jog = null;
    if (this.motion === 'jogging') this.motion = 'stopping';
  }

  // ---- simulator controls ----------------------------------------------------------

  // Home at once if necessary, go to the start of the track, then play it in a
  // loop until a command or a fault interrupts it.
  // offsetUs: where in the track to start.
  play(track: MotionTrack, offsetUs = 0): void {
    if (this.fault) {
      this.fault = null;
      this.motion = 'idle';
    }
    if (!this.homed) {
      this.refPan = 0;
      this.refTilt = 0;
      this.homedPan = true;
      this.homedTilt = true;
    }
    this.jog = null;
    this.playback = { track, startUs: null, offsetUs };
    const first = track.at(offsetUs);
    this.goto(first.pan, first.tilt);
  }

  stopPlayback(): void {
    if (!this.playback) return;
    this.playback = null;
    this.target = null;
    if (this.motion === 'moving' || this.motion === 'playing') this.motion = 'stopping';
  }

  reboot(nowUs: number, counterStart: number, ppm: number): void {
    this.bootId = (this.bootId + 1) & 0xffff;
    this.homedPan = false;
    this.homedTilt = false;
    this.motion = 'idle';
    this.fault = null;
    this.target = null;
    this.jog = null;
    this.playback = null;
    this.pan.cut();
    this.tilt.cut();
    this.pan.stalled = false;
    this.tilt.stalled = false;
    this.refPan = this.pan.pos;
    this.refTilt = this.tilt.pos;
    this.bootMonoUs = nowUs;
    this.counterStart = counterStart >>> 0;
    this.ppm = ppm;
  }
}
