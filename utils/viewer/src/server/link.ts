// The connection between the viewer and a server: everything the page reads
// and every action it can take. useServer.ts gives one to the page.

import type { ActionResult, CameraCommand, Preview, RecordingState, ServerEvent, ServerState } from './types.ts';

export interface ServerLink {
  // Why the page has no working connection now (shown to the operator), or null.
  problem: string | null;
  // The latest state, and performance.now() when it arrived. Without a new state
  // for 2 s the page shows "NOT LIVE" and disables the motion controls.
  state: ServerState | null;
  stateAtMs: number | null;
  events: ServerEvent[]; // newest first
  preview: { image: Preview; receivedAtMs: number } | null;

  // Previews of one camera, at most maxFps each second; null = no previews.
  watch(cameraId: string | null, maxFps: number): void;
  command(cameraId: string, c: CameraCommand): Promise<ActionResult>;
  // Called every 100 ms while the operator holds a jog button, and one time with
  // (0, 0) at release. A page can close or freeze while the camera moves.
  jog(cameraId: string, panDegPerS: number, tiltDegPerS: number): void;
  // One recording covers both cameras, from startRecording until stopRecording.
  startRecording(): Promise<ActionResult>;
  stopRecording(recordingId: string): Promise<ActionResult>;
  listRecordings(): Promise<RecordingState[]>; // newest first
}

const notConnected: ActionResult = { ok: false, reason: 'not connected' };

export const DISCONNECTED: ServerLink = {
  problem: 'not connected to a server',
  state: null,
  stateAtMs: null,
  events: [],
  preview: null,
  watch: () => {},
  command: async () => notConnected,
  jog: () => {},
  startRecording: async () => notConnected,
  stopRecording: async () => notConnected,
  listRecordings: async () => [],
};
