// A recorded motion of the camera (data/motion.csv): pan and tilt in degrees at
// fixed steps. The simulator plays it in a loop; the end joins the start.

import { readFileSync } from 'node:fs';

export interface MotionPoint {
  pan: number;
  tilt: number;
  panVel: number; // degrees per second
  tiltVel: number;
}

export class MotionTrack {
  readonly durationUs: number;

  constructor(
    private readonly tUs: Float64Array,
    private readonly pan: Float64Array,
    private readonly tilt: Float64Array,
  ) {
    if (tUs.length < 2) throw new Error('a motion needs at least 2 samples');
    this.durationUs = tUs[tUs.length - 1]! - tUs[0]!;
  }

  static fromCsv(text: string): MotionTrack {
    const rows = text.trim().split('\n');
    const header = rows.shift() ?? '';
    if (header.trim() !== 't_ms,pan_deg,tilt_deg') throw new Error(`motion CSV: unexpected header "${header}"`);
    const t = new Float64Array(rows.length);
    const p = new Float64Array(rows.length);
    const l = new Float64Array(rows.length);
    rows.forEach((r, i) => {
      const [a, b, c] = r.split(',').map(Number);
      if (!Number.isFinite(a) || !Number.isFinite(b) || !Number.isFinite(c)) throw new Error(`motion CSV: bad row ${i + 2}`);
      t[i] = a! * 1000;
      p[i] = b!;
      l[i] = c!;
    });
    return new MotionTrack(t, p, l);
  }

  static load(path: string): MotionTrack {
    return MotionTrack.fromCsv(readFileSync(path, 'utf8'));
  }

  // The pose `us` after the start of the track, in a loop.
  at(us: number): MotionPoint {
    const { tUs, pan, tilt } = this;
    const t = tUs[0]! + (((us % this.durationUs) + this.durationUs) % this.durationUs);
    let lo = 0;
    let hi = tUs.length - 1;
    while (hi - lo > 1) {
      const mid = (lo + hi) >> 1;
      if (tUs[mid]! <= t) lo = mid;
      else hi = mid;
    }
    const dt = (tUs[hi]! - tUs[lo]!) / 1e6;
    const f = dt === 0 ? 0 : (t - tUs[lo]!) / 1e6 / dt;
    const panVel = dt === 0 ? 0 : (pan[hi]! - pan[lo]!) / dt;
    const tiltVel = dt === 0 ? 0 : (tilt[hi]! - tilt[lo]!) / dt;
    return {
      pan: pan[lo]! + (pan[hi]! - pan[lo]!) * f,
      tilt: tilt[lo]! + (tilt[hi]! - tilt[lo]!) * f,
      panVel,
      tiltVel,
    };
  }
}
