// The camera's wire format (README.md, "Reference: message formats"): the video
// frame header and the control messages. Node only (uses zlib.crc32).

import { crc32 } from 'node:zlib';

export const VIDEO_START_MARKER = 0x4d524656; // "VFRM"
export const VIDEO_HEADER_BYTES = 32;
export const CONTROL_START_MARKER = 0x4c435450; // "PTCL"
export const CONTROL_VERSION = 1;
export const CONTROL_HEADER_BYTES = 16;
export const ENCODER_COUNTS = 3600; // 0.1° per count
export const COUNTS_TO_DEG = 360 / ENCODER_COUNTS;
export const TWO32 = 2 ** 32;

export { crc32 };

// ---- video ----------------------------------------------------------------

export interface FrameHeader {
  frameSeq: number;
  bootId: number;
  exposureStart: number; // camera clock, u32 µs
  width: number;
  height: number;
  payloadCrc32: number;
}

export function encodeFrameHeader(h: FrameHeader, out = Buffer.alloc(VIDEO_HEADER_BYTES)): Buffer {
  out.fill(0, 0, VIDEO_HEADER_BYTES);
  out.writeUInt32LE(VIDEO_START_MARKER, 0);
  out.writeUInt32LE(h.frameSeq >>> 0, 4);
  out.writeUInt32LE(h.exposureStart >>> 0, 8);
  out.writeUInt16LE(h.bootId & 0xffff, 12);
  out.writeUInt16LE(h.width, 14);
  out.writeUInt16LE(h.height, 16);
  out.writeUInt32LE(h.payloadCrc32 >>> 0, 24);
  out.writeUInt32LE(crc32(out.subarray(0, 28)), 28);
  return out;
}

export type HeaderResult = { ok: true; header: FrameHeader } | { ok: false; error: string };

// Check a header. It does not look at the pixels, which are width × height bytes.
export function decodeFrameHeader(b: Uint8Array): HeaderResult {
  if (b.length < VIDEO_HEADER_BYTES) return { ok: false, error: 'short' };
  const v = Buffer.from(b.buffer, b.byteOffset, VIDEO_HEADER_BYTES);
  if (v.readUInt32LE(0) !== VIDEO_START_MARKER) return { ok: false, error: 'start_marker' };
  if (v.readUInt32LE(28) !== crc32(v.subarray(0, 28))) return { ok: false, error: 'header_crc' };
  return {
    ok: true,
    header: {
      frameSeq: v.readUInt32LE(4),
      exposureStart: v.readUInt32LE(8),
      bootId: v.readUInt16LE(12),
      width: v.readUInt16LE(14),
      height: v.readUInt16LE(16),
      payloadCrc32: v.readUInt32LE(24),
    },
  };
}

// ---- control ----------------------------------------------------------------

export const MSG = {
  HELLO: 0x01,
  POSE: 0x02,
  TIME_RESP: 0x03,
  ACK: 0x04,
  STATUS: 0x05,
  TIME_REQ: 0x81,
  CMD: 0x82,
} as const;

export const CMD = { home: 1, goto: 2, jog: 3, stop: 4, clear_fault: 5 } as const;
export const ACK_RESULT = ['accepted', 'not_homed', 'out_of_limits', 'faulted', 'invalid'] as const;
export const MOTION = ['idle', 'homing', 'moving', 'jogging', 'stopping', 'faulted'] as const;
export const FAULT = [null, 'stall_pan', 'stall_tilt'] as const;

export function encodeControl(type: number, seq: number, payload: Buffer): Buffer {
  const out = Buffer.alloc(CONTROL_HEADER_BYTES + payload.length);
  out.writeUInt32LE(CONTROL_START_MARKER, 0);
  out.writeUInt8(CONTROL_VERSION, 4);
  out.writeUInt8(type, 5);
  out.writeUInt16LE(payload.length, 6);
  out.writeUInt32LE(seq >>> 0, 8);
  payload.copy(out, CONTROL_HEADER_BYTES);
  const c = crc32(payload, crc32(out.subarray(0, 12)));
  out.writeUInt32LE(c, 12);
  return out;
}

export interface ControlMessage {
  type: number;
  seq: number;
  payload: Buffer;
}

// Incremental parser for a control byte stream. Resyncs on a bad message.
export class ControlParser {
  private buf: Buffer = Buffer.alloc(0);
  damaged = 0;
  resyncBytes = 0;

  push(chunk: Buffer): ControlMessage[] {
    this.buf = this.buf.length ? Buffer.concat([this.buf, chunk]) : chunk;
    const out: ControlMessage[] = [];
    let i = 0;
    while (this.buf.length - i >= CONTROL_HEADER_BYTES) {
      if (this.buf.readUInt32LE(i) !== CONTROL_START_MARKER || this.buf.readUInt8(i + 4) !== CONTROL_VERSION) {
        i++;
        this.resyncBytes++;
        continue;
      }
      const n = this.buf.readUInt16LE(i + 6);
      if (n > 1024) {
        i++;
        this.resyncBytes++;
        continue;
      }
      if (this.buf.length - i < CONTROL_HEADER_BYTES + n) break;
      const payload = this.buf.subarray(i + CONTROL_HEADER_BYTES, i + CONTROL_HEADER_BYTES + n);
      const c = crc32(payload, crc32(this.buf.subarray(i, i + 12)));
      if (c !== this.buf.readUInt32LE(i + 12)) {
        this.damaged++;
        i++;
        continue;
      }
      out.push({ type: this.buf.readUInt8(i + 5), seq: this.buf.readUInt32LE(i + 8), payload: Buffer.from(payload) });
      i += CONTROL_HEADER_BYTES + n;
    }
    this.buf = this.buf.subarray(i);
    return out;
  }
}

function fixedString(s: string, n: number): Buffer {
  const b = Buffer.alloc(n);
  b.write(s.slice(0, n), 'ascii');
  return b;
}

export function readFixedString(b: Buffer, off: number, n: number): string {
  const s = b.subarray(off, off + n);
  const z = s.indexOf(0);
  return s.subarray(0, z < 0 ? n : z).toString('ascii');
}

export interface Hello {
  cameraId: string;
  firmwareVersion: string;
  bootId: number;
  homed: boolean;
  panMinDeg: number;
  panMaxDeg: number;
  tiltMinDeg: number;
  tiltMaxDeg: number;
  panMaxDegPerS: number;
  tiltMaxDegPerS: number;
  exposureUs: number;
}

export function encodeHello(h: Hello): Buffer {
  const b = Buffer.alloc(64);
  fixedString(h.cameraId, 16).copy(b, 0);
  fixedString(h.firmwareVersion, 16).copy(b, 16);
  b.writeUInt16LE(h.bootId, 32);
  b.writeUInt8(h.homed ? 1 : 0, 34);
  b.writeFloatLE(h.panMinDeg, 36);
  b.writeFloatLE(h.panMaxDeg, 40);
  b.writeFloatLE(h.tiltMinDeg, 44);
  b.writeFloatLE(h.tiltMaxDeg, 48);
  b.writeFloatLE(h.panMaxDegPerS, 52);
  b.writeFloatLE(h.tiltMaxDegPerS, 56);
  b.writeUInt32LE(h.exposureUs, 60);
  return b;
}

export function decodeHello(b: Buffer): Hello {
  return {
    cameraId: readFixedString(b, 0, 16),
    firmwareVersion: readFixedString(b, 16, 16),
    bootId: b.readUInt16LE(32),
    homed: b.readUInt8(34) === 1,
    panMinDeg: b.readFloatLE(36),
    panMaxDeg: b.readFloatLE(40),
    tiltMinDeg: b.readFloatLE(44),
    tiltMaxDeg: b.readFloatLE(48),
    panMaxDegPerS: b.readFloatLE(52),
    tiltMaxDegPerS: b.readFloatLE(56),
    exposureUs: b.readUInt32LE(60),
  };
}

export interface PoseMsg {
  counter: number;
  panCounts: number;
  tiltCounts: number;
  motion: number;
  homed: boolean;
  fault: number;
}

export function encodePose(p: PoseMsg): Buffer {
  const b = Buffer.alloc(16);
  b.writeUInt32LE(p.counter >>> 0, 0);
  b.writeInt32LE(p.panCounts, 4);
  b.writeInt32LE(p.tiltCounts, 8);
  b.writeUInt8(p.motion, 12);
  b.writeUInt8(p.homed ? 1 : 0, 13);
  b.writeUInt8(p.fault, 14);
  return b;
}

export function decodePose(b: Buffer): PoseMsg {
  return {
    counter: b.readUInt32LE(0),
    panCounts: b.readInt32LE(4),
    tiltCounts: b.readInt32LE(8),
    motion: b.readUInt8(12),
    homed: b.readUInt8(13) === 1,
    fault: b.readUInt8(14),
  };
}

export function encodeTimeReq(echo: bigint): Buffer {
  const b = Buffer.alloc(8);
  b.writeBigUInt64LE(echo, 0);
  return b;
}

export function encodeTimeResp(echo: bigint, t2: number, t3: number): Buffer {
  const b = Buffer.alloc(16);
  b.writeBigUInt64LE(echo, 0);
  b.writeUInt32LE(t2 >>> 0, 8);
  b.writeUInt32LE(t3 >>> 0, 12);
  return b;
}

export function decodeTimeResp(b: Buffer): { echo: bigint; t2: number; t3: number } {
  return { echo: b.readBigUInt64LE(0), t2: b.readUInt32LE(8), t3: b.readUInt32LE(12) };
}

export interface CmdMsg {
  cmdId: number;
  cmd: number;
  a: number;
  b: number;
}

export function encodeCmd(c: CmdMsg): Buffer {
  const b = Buffer.alloc(16);
  b.writeUInt32LE(c.cmdId >>> 0, 0);
  b.writeUInt8(c.cmd, 4);
  b.writeFloatLE(c.a, 8);
  b.writeFloatLE(c.b, 12);
  return b;
}

export function decodeCmd(b: Buffer): CmdMsg {
  return { cmdId: b.readUInt32LE(0), cmd: b.readUInt8(4), a: b.readFloatLE(8), b: b.readFloatLE(12) };
}

export function encodeAck(cmdId: number, result: number, duplicate: boolean): Buffer {
  const b = Buffer.alloc(8);
  b.writeUInt32LE(cmdId >>> 0, 0);
  b.writeUInt8(result, 4);
  b.writeUInt8(duplicate ? 1 : 0, 5);
  return b;
}

export function decodeAck(b: Buffer): { cmdId: number; result: number; duplicate: boolean } {
  return { cmdId: b.readUInt32LE(0), result: b.readUInt8(4), duplicate: b.readUInt8(5) === 1 };
}

export interface StatusMsg {
  bootId: number;
  fps: number;
  framesCaptured: number;
  framesSent: number;
  framesDropped: number;
  width: number;
  height: number;
}

export function encodeStatus(s: StatusMsg): Buffer {
  const b = Buffer.alloc(24);
  b.writeUInt16LE(s.bootId, 0);
  b.writeUInt16LE(s.fps, 2);
  b.writeUInt32LE(s.framesCaptured >>> 0, 4);
  b.writeUInt32LE(s.framesSent >>> 0, 8);
  b.writeUInt32LE(s.framesDropped >>> 0, 12);
  b.writeUInt16LE(s.width, 16);
  b.writeUInt16LE(s.height, 18);
  return b;
}

export function decodeStatus(b: Buffer): StatusMsg {
  return {
    bootId: b.readUInt16LE(0),
    fps: b.readUInt16LE(2),
    framesCaptured: b.readUInt32LE(4),
    framesSent: b.readUInt32LE(8),
    framesDropped: b.readUInt32LE(12),
    width: b.readUInt16LE(16),
    height: b.readUInt16LE(18),
  };
}

// Extend a wrapping u32 counter to a monotonic number, given the last extended value.
export function unwrap32(raw: number, lastExtended: number | null): number {
  if (lastExtended === null) return raw;
  const lastRaw = lastExtended % TWO32;
  let d = raw - lastRaw;
  if (d < -TWO32 / 2) d += TWO32;
  else if (d > TWO32 / 2) d -= TWO32;
  return lastExtended + d;
}
