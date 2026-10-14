// One axis of the simulated pan-tilt camera: an ideal motor with speed and
// acceleration limits, plus a stall fault. Positions are TRUE positions in
// degrees; the encoder model lives in model.ts.

export interface AxisLimits {
  minDeg: number;
  maxDeg: number;
  maxDegPerS: number;
  maxDegPerS2: number;
}

const SNAP_DEG = 0.0005;

function approach(v: number, want: number, maxStep: number): number {
  if (want > v) return Math.min(want, v + maxStep);
  return Math.max(want, v - maxStep);
}

export class Axis {
  pos: number;
  vel = 0; // commanded velocity; the true velocity is 0 while stalled
  stalled = false;

  constructor(
    readonly lim: AxisLimits,
    pos: number,
  ) {
    this.pos = pos;
  }

  // The fastest speed toward `target` from which the axis can still stop on it.
  gotoVelocity(target: number): number {
    const err = target - this.pos;
    return Math.sign(err) * Math.min(this.lim.maxDegPerS, Math.sqrt(2 * this.lim.maxDegPerS2 * Math.abs(err)));
  }

  // Cap a wanted velocity so that the axis can always stop before a limit.
  private capForLimits(want: number): number {
    const a2 = 2 * this.lim.maxDegPerS2;
    if (want > 0) return Math.min(want, Math.sqrt(a2 * Math.max(0, this.lim.maxDeg - this.pos)));
    if (want < 0) return Math.max(want, -Math.sqrt(a2 * Math.max(0, this.pos - this.lim.minDeg)));
    return 0;
  }

  // Advance by dt seconds toward a wanted velocity. Returns true when a
  // `snapTo` target was reached this step.
  step(dt: number, wantVel: number, snapTo: number | null = null): boolean {
    const want = this.capForLimits(Math.max(-this.lim.maxDegPerS, Math.min(this.lim.maxDegPerS, wantVel)));
    const v0 = this.vel;
    this.vel = approach(v0, want, this.lim.maxDegPerS2 * dt);
    if (!this.stalled) {
      this.pos += ((v0 + this.vel) / 2) * dt;
      this.pos = Math.max(this.lim.minDeg, Math.min(this.lim.maxDeg, this.pos));
    }
    if (snapTo !== null && !this.stalled) {
      if (Math.abs(snapTo - this.pos) < SNAP_DEG && Math.abs(this.vel) <= this.lim.maxDegPerS2 * dt * 1.5) {
        this.pos = snapTo;
        this.vel = 0;
        return true;
      }
    }
    return false;
  }

  cut(): void {
    this.vel = 0;
  }

  trueVel(): number {
    return this.stalled ? 0 : this.vel;
  }
}

// The stopping distance from speed v with the axis acceleration: v² / 2a.
export function stoppingDistanceDeg(v: number, lim: AxisLimits): number {
  return (v * v) / (2 * lim.maxDegPerS2);
}
