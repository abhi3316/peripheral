// The truth stamp: the simulated camera writes the TRUE pan, tilt and frame_seq at
// mid-exposure into the first 12 pixels of every frame. A real camera has none.

export const TRUTH_STAMP_PIXELS = 12;

export interface TruthStamp {
  panDeg: number;
  tiltDeg: number;
  frameSeq: number;
}

const toMilli = (deg: number): number => Math.round(deg * 1000);

export function writeTruthStamp(pixels: Uint8Array, t: TruthStamp): void {
  const v = new DataView(pixels.buffer, pixels.byteOffset, TRUTH_STAMP_PIXELS);
  v.setInt32(0, toMilli(t.panDeg), true);
  v.setInt32(4, toMilli(t.tiltDeg), true);
  v.setUint32(8, t.frameSeq >>> 0, true);
}

export function readTruthStamp(pixels: Uint8Array): TruthStamp | null {
  if (pixels.length < TRUTH_STAMP_PIXELS) return null;
  const v = new DataView(pixels.buffer, pixels.byteOffset, TRUTH_STAMP_PIXELS);
  return { panDeg: v.getInt32(0, true) / 1000, tiltDeg: v.getInt32(4, true) / 1000, frameSeq: v.getUint32(8, true) };
}
