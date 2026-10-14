// A panorama around the camera, made one time for each resolution. A frame is a
// crop of it, row by row, so that 1080p at 60 frames/s is cheap to make.

import { writeTruthStamp } from '../protocol/stamp.ts';

export const FOV_H_DEG = 48;

interface Landmark {
  az: number;
  el: number;
  r: number;
  value: number;
}

const LANDMARKS: Landmark[] = [
  { az: 0, el: 0, r: 2.5, value: 255 },
  { az: 45, el: 12, r: 4, value: 15 },
  { az: -90, el: 5, r: 5, value: 245 },
  { az: 120, el: 35, r: 6, value: 210 },
  { az: -140, el: -12, r: 4, value: 25 },
  { az: 160, el: 70, r: 5, value: 240 },
  { az: -45, el: 50, r: 3, value: 20 },
];

export class Panorama {
  readonly pxPerDeg: number;
  readonly fovV: number;
  readonly texW: number;
  readonly texH: number;
  readonly elTop: number; // elevation of texture row 0
  readonly tex: Buffer;

  constructor(
    readonly width: number,
    readonly height: number,
  ) {
    this.pxPerDeg = width / FOV_H_DEG;
    this.fovV = height / this.pxPerDeg;
    this.texW = Math.round(360 * this.pxPerDeg);
    this.elTop = 90 + this.fovV / 2 + 1;
    const elBottom = -30 - this.fovV / 2 - 1;
    this.texH = Math.ceil((this.elTop - elBottom) * this.pxPerDeg);
    this.tex = Buffer.allocUnsafe(this.texW * this.texH);
    this.paint();
  }

  private paint(): void {
    const { texW, texH, pxPerDeg, tex } = this;
    const gridAz = Math.round(15 * pxPerDeg);
    const gridEl = Math.round(10 * pxPerDeg);
    for (let y = 0; y < texH; y++) {
      const el = this.elTop - y / pxPerDeg;
      const row = y * texW;
      const horizon = Math.abs(el) < 0.3;
      const elLine = Math.round(el * pxPerDeg) % gridEl === 0;
      const base = el > 0 ? 150 + Math.round(60 * (el / 90)) : 70;
      for (let x = 0; x < texW; x++) {
        let v = base;
        if (el <= 0) v += ((x >> 4) + (y >> 4)) & 1 ? 12 : -12; // ground checker
        else v += Math.round(12 * Math.sin(x / (pxPerDeg * 3.2)));
        if (horizon) v = 0;
        else if (elLine || x % gridAz === 0) v = el > 0 ? 200 : 110;
        tex[row + x] = v;
      }
    }
    for (const m of LANDMARKS) {
      const cx = ((((m.az + 180) % 360) + 360) % 360) * pxPerDeg; // texture x 0 = azimuth -180
      const cy = (this.elTop - m.el) * pxPerDeg;
      const r = m.r * pxPerDeg;
      for (let y = Math.max(0, Math.floor(cy - r - 2)); y < Math.min(texH, cy + r + 2); y++) {
        for (let dx = -Math.ceil(r + 2); dx <= r + 2; dx++) {
          const d = Math.hypot(dx, y - cy);
          if (d > r + 1) continue;
          const x = (((Math.round(cx) + dx) % texW) + texW) % texW;
          tex[y * texW + x] = d < r ? m.value : 128;
        }
      }
    }
  }

  // Write the frame for a camera pose into `out` (width × height bytes).
  render(panDeg: number, tiltDeg: number, frameSeq: number, out: Buffer): void {
    const { width, height, texW, texH, pxPerDeg, tex } = this;
    const az0 = panDeg - FOV_H_DEG / 2 + 180; // texture x 0 = azimuth -180
    let x0 = Math.round(az0 * pxPerDeg) % texW;
    if (x0 < 0) x0 += texW;
    let y0 = Math.round((this.elTop - (tiltDeg + this.fovV / 2)) * pxPerDeg);
    y0 = Math.max(0, Math.min(texH - height, y0));
    const first = Math.min(width, texW - x0);
    for (let r = 0; r < height; r++) {
      const src = (y0 + r) * texW;
      const dst = r * width;
      tex.copy(out, dst, src + x0, src + x0 + first);
      if (first < width) tex.copy(out, dst + first, src, src + (width - first));
    }
    // frame_seq as 32 black/white pixels at the bottom right
    const base = (height - 1) * width + (width - 32);
    for (let b = 0; b < 32; b++) out[base + b] = (frameSeq >>> (31 - b)) & 1 ? 255 : 0;
    writeTruthStamp(out, { panDeg, tiltDeg, frameSeq });
  }
}
