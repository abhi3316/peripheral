// What the viewer shows. Your server decides what it sends and how; your code
// in useServer.ts turns it into these types.
//
// Every field that ends in `Us` is a time in microseconds on ONE clock of your
// server (for example CLOCK_MONOTONIC). The viewer only subtracts these times
// from each other, for example to show "3 s ago".

export type LinkState = 'connected' | 'disconnected';
export type MotionState = 'idle' | 'homing' | 'moving' | 'jogging' | 'stopping' | 'faulted';

export interface Limits {
  panMinDeg: number;
  panMaxDeg: number;
  tiltMinDeg: number;
  tiltMaxDeg: number;
  panMaxDegPerS: number;
  tiltMaxDegPerS: number;
}

export interface CameraState {
  cameraId: string;
  link: LinkState; // the control connection
  videoLink: LinkState; // the video connection
  lastFrameUs: number | null; // when the last good frame arrived
  bootId: number;
  firmwareVersion: string;
  homed: boolean; // false: the pose is relative to the power-on position
  motion: MotionState;
  fault: { code: string; message: string; sinceUs: number } | null;
  pose: { panDeg: number; tiltDeg: number; atUs: number } | null; // the latest position
  velocity: { panDegPerS: number; tiltDegPerS: number };
  target: { panDeg: number; tiltDeg: number } | null; // a goto in progress
  limits: Limits;
  stream: { width: number; height: number; fps: number } | null;
  // The camera clock as the server sees it. Null until it is measured.
  clock: { offsetUs: number; driftPpm: number; uncertaintyUs: number } | null;
  counters: {
    framesReceived: number;
    framesDamaged: number;
    framesMissing: number;
    framesDroppedByCamera: number;
    framesDroppedInServer: number;
    resyncBytes: number;
  };
}

// The path of the video from the socket to the disk.
export interface Pipeline {
  ingestBytesPerS: number;
  ingestFramesPerS: number;
  buffer: { capacityBytes: number; usedBytes: number; highWaterBytes: number };
  disk: { quotaBytes: number; usedBytes: number; writeBytesPerS: number };
  memoryBytes: number; // the server's own memory
}

export interface UploadState {
  storage: 'connected' | 'disconnected' | 'unknown';
  lastContactUs: number | null;
  backlogObjects: number; // not yet confirmed by the cloud storage
  backlogBytes: number;
  oldestPendingUs: number | null;
}

export type GapReason = 'dropped_by_camera' | 'damaged' | 'link_down' | 'dropped_in_server' | 'disk_full' | 'unknown';

// Frames that are not in a recording, and why.
export interface Gap {
  cameraId: string;
  bootId: number;
  fromSeq: number;
  toSeq: number;
  reason: GapReason;
}

export interface RecordingState {
  recordingId: string;
  state: 'recording' | 'stopped';
  startedAtUs: number;
  startedAtWall: string; // ISO 8601, for people
  stoppedAtUs: number | null;
  frames: number;
  bytes: number;
  gaps: Gap[];
  complete: boolean; // false: it did not stop normally (a crash, a power cut)
  upload: 'pending' | 'partial' | 'uploaded';
}

export interface Alarm {
  code: string;
  severity: 'warning' | 'alarm';
  cameraId: string | null;
  message: string;
  sinceUs: number;
}

export interface ServerEvent {
  id: number;
  atUs: number;
  level: 'info' | 'warning' | 'alarm';
  cameraId: string | null;
  message: string;
}

export interface ServerState {
  serverId: string;
  measuredAtUs: number; // when the server measured this state
  cameras: CameraState[];
  pipeline: Pipeline;
  upload: UploadState;
  recording: RecordingState | null; // the active recording
  alarms: Alarm[];
}

// A reduced image of a frame, for the operator to look at. Never a full frame.
export interface Preview {
  cameraId: string;
  bootId: number;
  frameSeq: number;
  width: number;
  height: number;
  pixels: Uint8Array; // width × height bytes, 8-bit grey, row by row
  sourceWidth: number; // the size of the full frame
  sourceHeight: number;
  exposureStartUs: number;
  exposureEndUs: number;
  pose: { panDeg: number; tiltDeg: number } | null; // the server's pose for this frame
  homed: boolean;
  recorded: boolean;
  // The first 12 bytes of the FULL frame, unchanged: the simulated camera's truth
  // stamp. With it, the viewer shows the error of your pose. Null if you do not send it.
  truthStamp: Uint8Array | null;
}

export type CameraCommand =
  | { command: 'home' }
  | { command: 'goto'; panDeg: number; tiltDeg: number }
  | { command: 'stop' }
  | { command: 'clear_fault' };

export type ActionResult = { ok: true } | { ok: false; reason: string };
