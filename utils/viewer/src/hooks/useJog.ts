// Hold-to-move jog. While held: link.jog() every 100 ms. On release, on blur,
// when the tab hides, or when the page goes stale: one zero jog, then nothing.

import { useCallback, useEffect, useRef, useState } from 'react';

const JOG_REFRESH_MS = 100;

export interface Jog {
  active: { pan: number; tilt: number } | null;
  start: (panDegPerS: number, tiltDegPerS: number) => void;
  stop: () => void;
}

export function useJog(sendJog: (cameraId: string, pan: number, tilt: number) => void, cameraId: string | null, enabled: boolean): Jog {
  const [active, setActive] = useState<{ pan: number; tilt: number } | null>(null);
  const timer = useRef<ReturnType<typeof setInterval> | null>(null);
  const vel = useRef<{ pan: number; tilt: number } | null>(null);
  const camera = useRef(cameraId);

  const emit = useCallback(
    (id: string, pan: number, tilt: number) => sendJog(id, pan, tilt),
    [sendJog],
  );

  const stop = useCallback(() => {
    if (timer.current) clearInterval(timer.current);
    timer.current = null;
    const id = camera.current;
    if (vel.current && id) emit(id, 0, 0);
    vel.current = null;
    setActive(null);
  }, [emit]);

  const start = useCallback(
    (pan: number, tilt: number) => {
      const id = camera.current;
      if (!id || !enabled) return;
      vel.current = { pan, tilt };
      setActive({ pan, tilt });
      emit(id, pan, tilt);
      if (timer.current) clearInterval(timer.current);
      timer.current = setInterval(() => {
        const v = vel.current;
        if (v && camera.current) emit(camera.current, v.pan, v.tilt);
      }, JOG_REFRESH_MS);
    },
    [emit, enabled],
  );

  // A change of camera stops the jog of the old camera.
  useEffect(() => {
    if (camera.current !== cameraId) stop();
    camera.current = cameraId;
  }, [cameraId, stop]);

  useEffect(() => {
    if (!enabled) stop();
  }, [enabled, stop]);

  useEffect(() => {
    const onHide = () => {
      if (document.visibilityState !== 'visible') stop();
    };
    window.addEventListener('blur', stop);
    document.addEventListener('visibilitychange', onHide);
    return () => {
      window.removeEventListener('blur', stop);
      document.removeEventListener('visibilitychange', onHide);
      stop();
    };
  }, [stop]);

  return { active, start, stop };
}
