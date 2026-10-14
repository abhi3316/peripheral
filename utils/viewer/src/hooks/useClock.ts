import { useEffect, useState } from 'react';
import type { ServerLink } from '../server/link.ts';

export const STALE_AFTER_MS = 2000;

// Re-render on a fixed beat, so that ages and staleness move even when no
// message arrives — which is exactly when they matter.
export function useNow(periodMs = 250): number {
  const [now, setNow] = useState(() => performance.now());
  useEffect(() => {
    const t = setInterval(() => setNow(performance.now()), periodMs);
    return () => clearInterval(t);
  }, [periodMs]);
  return now;
}

export interface Staleness {
  live: boolean; // the page may send motion commands
  ageMs: number | null; // since the last state
  reason: string | null;
  // The server's clock now, estimated from the last state.
  serverNowUs: number | null;
}

export function staleness(link: ServerLink, nowMs: number): Staleness {
  const ageMs = link.stateAtMs === null ? null : Math.max(0, nowMs - link.stateAtMs);
  const serverNowUs = link.state && ageMs !== null ? link.state.measuredAtUs + ageMs * 1000 : null;
  let reason: string | null = null;
  if (link.problem) reason = link.problem;
  else if (ageMs === null) reason = 'waiting for the first state from the server';
  else if (ageMs > STALE_AFTER_MS) reason = `no update from the server for ${(ageMs / 1000).toFixed(1)} s`;
  return { live: reason === null, ageMs, reason, serverNowUs };
}
