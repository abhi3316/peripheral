// The one place where the viewer meets your server.
//
// Open the page with ?demo to see it run on a fake server (demo.ts). Without
// ?demo it uses useYourServer() below, which you write: connect to your
// server, keep `state`, `events` and `preview` up to date, and carry out the
// actions. The rest of the viewer only uses the ServerLink interface.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useDemoServer } from './demo.ts';
import { type ServerLink } from './link.ts';
import type { ActionResult, CameraCommand, Preview, RecordingState, ServerEvent, ServerState } from './types.ts';

const demo = typeof window !== 'undefined' && new URLSearchParams(window.location.search).has('demo');

export function useServer(): ServerLink {
  const fake = useDemoServer(demo);
  const yours = useYourServer(!demo);
  return demo ? fake : yours;
}

const SERVER_URL = '';

function b64toUint8(b64: string): Uint8Array {
  const bin = atob(b64);
  const u8 = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) {
    u8[i] = bin.charCodeAt(i);
  }
  return u8;
}

function useYourServer(enabled: boolean): ServerLink {
  const [state, setState] = useState<ServerState | null>(null);
  const [stateAtMs, setStateAtMs] = useState<number | null>(null);
  const [preview, setPreview] = useState<{ image: Preview; receivedAtMs: number } | null>(null);
  const [events, setEvents] = useState<ServerEvent[]>([]);
  const [problem, setProblem] = useState<string | null>(null);

  const watchingCameraRef = useRef<string | null>('cam0');
  const watchingFpsRef = useRef<number>(10);

  // Poll server state every 250 ms
  useEffect(() => {
    if (!enabled) return;
    let cancelled = false;

    const pollState = async () => {
      try {
        const res = await fetch(`${SERVER_URL}/api/state`);
        if (!res.ok) {
          if (!cancelled) setProblem(`Server error: ${res.statusText}`);
          return;
        }
        const data = (await res.json()) as ServerState;
        if (data && data.cameras) {
          for (const cam of data.cameras) {
            if (cam.clock) {
              cam.clock.driftPpm = typeof cam.clock.driftPpm === 'number' ? cam.clock.driftPpm : 0;
              cam.clock.uncertaintyUs = typeof cam.clock.uncertaintyUs === 'number' ? cam.clock.uncertaintyUs : 50;
              cam.clock.offsetUs = typeof cam.clock.offsetUs === 'number' ? cam.clock.offsetUs : 0;
            }
            if (!cam.limits) {
              cam.limits = { panMinDeg: -170, panMaxDeg: 170, tiltMinDeg: -30, tiltMaxDeg: 90, panMaxDegPerS: 100, tiltMaxDegPerS: 50 };
            }
            if (!cam.velocity) {
              cam.velocity = { panDegPerS: 0, tiltDegPerS: 0 };
            }
          }
        }
        if (!cancelled) {
          setState(data);
          setStateAtMs(performance.now());
          setProblem(null);
          if (Array.isArray((data as any).events)) {
            setEvents((data as any).events);
          }
        }
      } catch (err) {
        if (!cancelled) {
          setProblem(err instanceof Error ? err.message : String(err));
        }
      }
    };

    void pollState();
    const interval = setInterval(() => void pollState(), 250);
    return () => {
      cancelled = true;
      clearInterval(interval);
    };
  }, [enabled]);

  // Poll camera preview
  useEffect(() => {
    if (!enabled) return;
    let cancelled = false;

    const pollPreview = async () => {
      const cam = watchingCameraRef.current;
      if (!cam) return;
      try {
        const res = await fetch(`${SERVER_URL}/api/preview?camera=${cam}`);
        if (!res.ok) return;
        const data = await res.json();
        if (data.error || !data.pixelsBase64) return;

        const image: Preview = {
          cameraId: data.cameraId,
          bootId: data.bootId,
          frameSeq: data.frameSeq,
          width: data.width,
          height: data.height,
          pixels: b64toUint8(data.pixelsBase64),
          sourceWidth: data.sourceWidth || 1000,
          sourceHeight: data.sourceHeight || 625,
          exposureStartUs: data.exposureStartUs,
          exposureEndUs: data.exposureEndUs,
          pose: data.pose ?? null,
          homed: !!data.homed,
          recorded: !!data.recorded,
          truthStamp: data.truthStampBase64 ? b64toUint8(data.truthStampBase64) : null,
        };

        if (!cancelled) {
          setPreview({ image, receivedAtMs: performance.now() });
        }
      } catch {}
    };

    void pollPreview();
    const interval = setInterval(() => void pollPreview(), 100);
    return () => {
      cancelled = true;
      clearInterval(interval);
    };
  }, [enabled]);

  const watch = useCallback((cameraId: string | null, maxFps: number) => {
    watchingCameraRef.current = cameraId;
    watchingFpsRef.current = maxFps;
  }, []);

  const command = useCallback(async (cameraId: string, c: CameraCommand): Promise<ActionResult> => {
    try {
      const res = await fetch(`${SERVER_URL}/api/command`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ cameraId, ...c }),
      });
      if (!res.ok) {
        return { ok: false, reason: res.statusText };
      }
      return { ok: true };
    } catch (err) {
      return { ok: false, reason: err instanceof Error ? err.message : String(err) };
    }
  }, []);

  const jog = useCallback((cameraId: string, panDegPerS: number, tiltDegPerS: number): void => {
    fetch(`${SERVER_URL}/api/jog`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ cameraId, panDegPerS, tiltDegPerS }),
    }).catch(() => {});
  }, []);

  const startRecording = useCallback(async (): Promise<ActionResult> => {
    try {
      const res = await fetch(`${SERVER_URL}/api/recording/start`, { method: 'POST' });
      if (!res.ok) return { ok: false, reason: res.statusText };
      return { ok: true };
    } catch (err) {
      return { ok: false, reason: err instanceof Error ? err.message : String(err) };
    }
  }, []);

  const stopRecording = useCallback(async (_id: string): Promise<ActionResult> => {
    try {
      const res = await fetch(`${SERVER_URL}/api/recording/stop`, { method: 'POST' });
      if (!res.ok) return { ok: false, reason: res.statusText };
      return { ok: true };
    } catch (err) {
      return { ok: false, reason: err instanceof Error ? err.message : String(err) };
    }
  }, []);

  const listRecordings = useCallback(async (): Promise<RecordingState[]> => {
    try {
      const res = await fetch(`${SERVER_URL}/api/recordings`);
      if (!res.ok) return [];
      return (await res.json()) as RecordingState[];
    } catch {
      return [];
    }
  }, []);

  return useMemo(
    () => ({
      problem,
      state,
      stateAtMs,
      events,
      preview,
      watch,
      command,
      jog,
      startRecording,
      stopRecording,
      listRecordings,
    }),
    [problem, state, stateAtMs, events, preview, watch, command, jog, startRecording, stopRecording, listRecordings],
  );
}
