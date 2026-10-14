// Home, go to, jog, stop, clear fault. Motion controls are disabled while the
// page is stale. Stop is never disabled while there is a camera: if the page
// might be wrong about the state, a stop attempt is still the safe action.

import { useEffect, useState, type KeyboardEvent, type PointerEvent } from 'react';
import type { Jog } from '../hooks/useJog.ts';
import type { ServerLink } from '../server/link.ts';
import type { CameraCommand, CameraState } from '../server/types.ts';

interface Props {
  camera: CameraState;
  live: boolean;
  jog: Jog;
  command: ServerLink['command'];
}

const DIRS: { label: string; pan: number; tilt: number; key?: string }[] = [
  { label: '↖', pan: -1, tilt: 1 },
  { label: '↑', pan: 0, tilt: 1, key: 'ArrowUp' },
  { label: '↗', pan: 1, tilt: 1 },
  { label: '←', pan: -1, tilt: 0, key: 'ArrowLeft' },
  { label: '', pan: 0, tilt: 0 },
  { label: '→', pan: 1, tilt: 0, key: 'ArrowRight' },
  { label: '↙', pan: -1, tilt: -1 },
  { label: '↓', pan: 0, tilt: -1, key: 'ArrowDown' },
  { label: '↘', pan: 1, tilt: -1 },
];

export function MotionControls({ camera, live, jog, command }: Props) {
  const [pending, setPending] = useState<string | null>(null);
  const [result, setResult] = useState<{ text: string; ok: boolean } | null>(null);
  const [pan, setPan] = useState('0');
  const [tilt, setTilt] = useState('0');
  const [speed, setSpeed] = useState(50); // percent of the axis maximum

  const l = camera.limits;
  const connected = camera.link === 'connected';
  const faulted = camera.fault !== null;
  const canMove = live && connected && !faulted;
  const canAim = canMove && camera.homed;

  // Reset the form when the operator selects another camera.
  useEffect(() => {
    setResult(null);
  }, [camera.cameraId]);

  const run = async (label: string, c: CameraCommand) => {
    if (pending) return; // one action at a time; a double click does nothing
    setPending(label);
    const r = await command(camera.cameraId, c);
    setResult(r.ok ? { text: `${label}: accepted`, ok: true } : { text: `${label}: rejected — ${r.reason}`, ok: false });
    setPending(null);
  };

  const panNum = Number(pan);
  const tiltNum = Number(tilt);
  const gotoError =
    pan.trim() === '' || tilt.trim() === '' || !Number.isFinite(panNum) || !Number.isFinite(tiltNum)
      ? 'enter numbers'
      : panNum < l.panMinDeg || panNum > l.panMaxDeg
        ? `pan must be ${l.panMinDeg}…${l.panMaxDeg}`
        : tiltNum < l.tiltMinDeg || tiltNum > l.tiltMaxDeg
          ? `tilt must be ${l.tiltMinDeg}…${l.tiltMaxDeg}`
          : null;

  const startJog = (d: (typeof DIRS)[number]) => {
    if (!canAim || (d.pan === 0 && d.tilt === 0)) return;
    jog.start((d.pan * l.panMaxDegPerS * speed) / 100, (d.tilt * l.tiltMaxDegPerS * speed) / 100);
  };

  const onPadDown = (e: PointerEvent<HTMLButtonElement>, d: (typeof DIRS)[number]) => {
    e.currentTarget.setPointerCapture(e.pointerId);
    startJog(d);
  };

  const onPadKey = (e: KeyboardEvent<HTMLDivElement>, down: boolean) => {
    const d = DIRS.find((x) => x.key === e.key);
    if (!d) return;
    e.preventDefault();
    if (down && !e.repeat) startJog(d);
    if (!down) jog.stop();
  };

  return (
    <section className="panel">
      <header>
        <h2>Control</h2>
        {pending && <span className="muted">{pending}…</span>}
      </header>

      <div className="row">
        <button
          type="button"
          className="danger big"
          disabled={!connected || pending === 'stop'}
          onClick={() => {
            jog.stop();
            void run('stop', { command: 'stop' });
          }}
        >
          STOP
        </button>
        <button type="button" disabled={!canMove || !!pending} onClick={() => void run('home', { command: 'home' })}>
          Home
        </button>
        <button
          type="button"
          disabled={!live || !connected || !faulted || !!pending}
          onClick={() => void run('clear fault', { command: 'clear_fault' })}
        >
          Clear fault
        </button>
      </div>

      <form
        className="row goto"
        onSubmit={(e) => {
          e.preventDefault();
          if (!gotoError && canAim) void run('go to', { command: 'goto', panDeg: panNum, tiltDeg: tiltNum });
        }}
      >
        <label>
          Pan
          <input inputMode="decimal" value={pan} onChange={(e) => setPan(e.target.value)} disabled={!canAim} />
        </label>
        <label>
          Tilt
          <input inputMode="decimal" value={tilt} onChange={(e) => setTilt(e.target.value)} disabled={!canAim} />
        </label>
        <button type="submit" disabled={!canAim || !!gotoError || !!pending}>
          Go to
        </button>
        {canAim && gotoError && <span className="hint warn">{gotoError}</span>}
      </form>

      <div className="jog">
        <div
          className="jog-pad"
          tabIndex={0}
          aria-label="Jog pad. Hold an arrow key or a button to move."
          onKeyDown={(e) => onPadKey(e, true)}
          onKeyUp={(e) => onPadKey(e, false)}
          onBlur={() => jog.stop()}
        >
          {DIRS.map((d, i) =>
            d.pan === 0 && d.tilt === 0 ? (
              <span key={i} className={`jog-center ${jog.active ? 'on' : ''}`} aria-hidden>
                {jog.active ? '●' : ''}
              </span>
            ) : (
              <button
                key={i}
                type="button"
                tabIndex={-1}
                disabled={!canAim}
                onPointerDown={(e) => onPadDown(e, d)}
                onPointerUp={() => jog.stop()}
                onPointerCancel={() => jog.stop()}
                onLostPointerCapture={() => jog.stop()}
                onContextMenu={(e) => e.preventDefault()}
              >
                {d.label}
              </button>
            ),
          )}
        </div>
        <label className="speed">
          Jog speed {speed}%
          <input type="range" min={5} max={100} step={5} value={speed} onChange={(e) => setSpeed(Number(e.target.value))} />
          <span className="muted mono">
            {((l.panMaxDegPerS * speed) / 100).toFixed(0)} °/s pan · {((l.tiltMaxDegPerS * speed) / 100).toFixed(0)} °/s tilt
          </span>
        </label>
      </div>

      <p className="status-line">
        {!live && <span className="bad">Motion disabled: the page is not live. </span>}
        {live && !connected && <span className="bad">Camera link is down. </span>}
        {live && connected && faulted && <span className="bad">Camera has a fault. Clear it first. </span>}
        {live && connected && !faulted && !camera.homed && <span className="warn">Home the camera before Go to or Jog. </span>}
        {result && <span className={result.ok ? 'good' : 'bad'}>{result.text}</span>}
      </p>
    </section>
  );
}
