// Faults on demand, through the camera simulator's and the cloud storage's test
// interfaces. It appears when the camera simulator answers.

import { useEffect, useState } from 'react';
import { CAMERAS, cameraPost, cameraStats, storageFault, type CameraStats } from '../tools.ts';

export function TestPanel() {
  const [stats, setStats] = useState<(CameraStats | null)[]>([]);
  const [target, setTarget] = useState(CAMERAS[0]!);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let stop = false;
    const load = async () => {
      const s = await Promise.all(CAMERAS.map((id) => cameraStats(id)));
      if (!stop) setStats(s);
    };
    void load();
    const t = setInterval(() => void load(), 1000);
    return () => {
      stop = true;
      clearInterval(t);
    };
  }, []);

  if (!stats.some((s) => s)) return null;
  const act = async (p: Promise<string | null>) => setError(await p);
  const fault = (body: unknown) => void act(cameraPost(target, '/fault', body));

  return (
    <details className="panel sim">
      <summary>
        <h2>Test</h2>
        <span className="muted mono">
          {stats.map((s, i) =>
            s ? (
              <span key={i}>
                {i > 0 && ' · '}
                {s.cameraId} {((s.sendBytesPerS * 8) / 1e6).toFixed(0)} Mb/s, {s.droppedByCamera} dropped
              </span>
            ) : null,
          )}
        </span>
      </summary>
      <h3>Camera simulator</h3>
      <div className="row wrap">
        {CAMERAS.map((id) => (
          <button key={id} type="button" className={target === id ? 'primary' : ''} onClick={() => setTarget(id)} aria-pressed={target === id}>
            {id}
          </button>
        ))}
        <span className="muted">The buttons below act on {target}.</span>
      </div>
      <div className="row wrap">
        <button type="button" onClick={() => void act(cameraPost(target, '/play', {}))}>Play motion</button>
        <button type="button" onClick={() => void act(cameraPost(target, '/play', { stop: true }))}>Stop motion</button>
        <button type="button" onClick={() => fault({ kind: 'corrupt', count: 10 })}>10 damaged frames</button>
        <button type="button" onClick={() => fault({ kind: 'garbage', bytes: 5000 })}>Garbage bytes</button>
        <button type="button" onClick={() => fault({ kind: 'video_down', ms: 3000 })}>Video down 3 s</button>
        <button type="button" onClick={() => fault({ kind: 'control_down', ms: 3000 })}>Control down 3 s</button>
        <button type="button" onClick={() => fault({ kind: 'reboot', counterStart: 2 ** 32 - 10_000_000 })}>Restart (clock wraps in 10 s)</button>
        <button type="button" onClick={() => fault({ kind: 'stall', axis: 'pan' })}>Stall pan</button>
        <button type="button" onClick={() => fault({ kind: 'unstall' })}>Free axes</button>
      </div>
      <h3>Cloud storage</h3>
      <div className="row wrap">
        <button type="button" onClick={() => void act(storageFault({ down: true }))}>Down</button>
        <button type="button" onClick={() => void act(storageFault({ bytesPerSecond: 1_250_000 }))}>Slow (10 Mb/s)</button>
        <button type="button" onClick={() => void act(storageFault({ errorRate: 0.3 }))}>30% errors</button>
        <button type="button" className="primary" onClick={() => void act(storageFault({ clear: true }))}>Restore</button>
      </div>
      {error && <p className="status-line bad">{error}</p>}
    </details>
  );
}
