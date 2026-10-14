// The smaller read-only panels: camera list, camera details, recording, upload,
// alarms and events.

import { useEffect, useState } from 'react';
import type { ServerLink } from '../server/link.ts';
import type { Alarm, Gap, CameraState, Pipeline, RecordingState, ServerEvent, ServerState } from '../server/types.ts';
import { ageUs, bytes, since } from '../format.ts';

export function CameraList({
  cameras,
  selected,
  onSelect,
}: {
  cameras: CameraState[];
  selected: string | null;
  onSelect: (id: string) => void;
}) {
  return (
    <nav className="panel camera-list" aria-label="Cameras">
      <header>
        <h2>Cameras</h2>
      </header>
      {cameras.length === 0 && <p className="muted">No cameras reported.</p>}
      <ul>
        {cameras.map((h) => {
          const state = h.link !== 'connected' ? 'bad' : h.fault ? 'bad' : !h.homed ? 'warn' : 'good';
          return (
            <li key={h.cameraId}>
              <button
                type="button"
                className={`camera-item ${selected === h.cameraId ? 'selected' : ''}`}
                onClick={() => onSelect(h.cameraId)}
                aria-pressed={selected === h.cameraId}
              >
                <span className={`dot ${state}`} aria-hidden />
                <span className="mono">{h.cameraId}</span>
                <span className="muted">
                  {h.link !== 'connected' ? 'link down' : h.fault ? h.fault.code : !h.homed ? 'not homed' : h.motion}
                </span>
              </button>
            </li>
          );
        })}
      </ul>
    </nav>
  );
}

export function CameraDetails({ camera, serverNowUs }: { camera: CameraState; serverNowUs: number | null }) {
  const c = camera.counters;
  const clockBad = camera.clock !== null && camera.clock.uncertaintyUs > 200;
  return (
    <section className="panel">
      <header>
        <h2>Camera {camera.cameraId}</h2>
        <span className="muted mono">fw {camera.firmwareVersion} · boot {camera.bootId}</span>
      </header>
      <table className="kv">
        <tbody>
          <tr>
            <th>Links</th>
            <td>
              <span className={camera.link === 'connected' ? 'good' : 'bad'}>control {camera.link === 'connected' ? 'up' : 'down'}</span> ·{' '}
              <span className={camera.videoLink === 'connected' ? 'good' : 'bad'}>video {camera.videoLink === 'connected' ? 'up' : 'down'}</span>
            </td>
            <th>Last frame</th>
            <td className="mono">{since(serverNowUs, camera.lastFrameUs)}</td>
          </tr>
          <tr>
            <th>Motion</th>
            <td>{camera.motion}</td>
            <th>Stream</th>
            <td className="mono">{camera.stream ? `${camera.stream.width}×${camera.stream.height} @ ${camera.stream.fps}` : '—'}</td>
          </tr>
          {camera.fault && (
            <tr>
              <th>Fault</th>
              <td colSpan={3} className="bad">
                {camera.fault.code} — {camera.fault.message} ({since(serverNowUs, camera.fault.sinceUs)})
              </td>
            </tr>
          )}
          <tr>
            <th>Clock offset</th>
            <td className="mono">{camera.clock ? `${(camera.clock.offsetUs / 1000).toFixed(3)} ms` : '—'}</td>
            <th>Drift</th>
            <td className="mono">{camera.clock ? `${camera.clock.driftPpm.toFixed(2)} ppm` : '—'}</td>
          </tr>
          <tr>
            <th>Time uncertainty</th>
            <td className={`mono ${clockBad ? 'bad' : ''}`} colSpan={3}>
              {camera.clock ? `± ${camera.clock.uncertaintyUs.toFixed(0)} µs` : 'not measured'}
              {clockBad && ' — more than 200 µs'}
            </td>
          </tr>
          <tr>
            <th>Frames</th>
            <td className="mono" colSpan={3}>
              {c.framesReceived} received · <span className={c.framesDamaged ? 'warn' : ''}>{c.framesDamaged} damaged</span> ·{' '}
              <span className={c.framesMissing ? 'warn' : ''}>{c.framesMissing} missing</span>
            </td>
          </tr>
          <tr>
            <th>Dropped</th>
            <td className="mono" colSpan={3}>
              <span className={c.framesDroppedByCamera ? 'bad' : ''}>{c.framesDroppedByCamera} at the camera</span> ·{' '}
              <span className={c.framesDroppedInServer ? 'bad' : ''}>{c.framesDroppedInServer} in the server</span> · {c.resyncBytes} resync bytes
            </td>
          </tr>
        </tbody>
      </table>
    </section>
  );
}

function gapSummary(gaps: Gap[]): string {
  if (gaps.length === 0) return 'none';
  const by = new Map<string, number>();
  for (const g of gaps) by.set(g.reason, (by.get(g.reason) ?? 0) + (g.toSeq - g.fromSeq + 1));
  return [...by].map(([reason, n]) => `${n} ${reason.replace(/_/g, ' ')}`).join(' · ');
}

function GapList({ gaps }: { gaps: Gap[] }) {
  if (gaps.length === 0) return <p className="muted">No gaps: every frame the camera captured is in this recording.</p>;
  return (
    <table className="list">
      <thead>
        <tr>
          <th>Camera</th>
          <th>Boot</th>
          <th>Frames</th>
          <th>Reason</th>
        </tr>
      </thead>
      <tbody>
        {gaps.slice(0, 50).map((g, i) => (
          <tr key={i}>
            <td className="mono">{g.cameraId}</td>
            <td className="mono">{g.bootId}</td>
            <td className="mono">{g.fromSeq === g.toSeq ? g.fromSeq : `${g.fromSeq}–${g.toSeq}`}</td>
            <td className={g.reason === 'unknown' ? 'bad' : 'warn'}>{g.reason.replace(/_/g, ' ')}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export function RecordingPanel({ link, live }: { link: ServerLink; live: boolean }) {
  const [pending, setPending] = useState(false);
  const [message, setMessage] = useState<{ text: string; ok: boolean } | null>(null);
  const [history, setHistory] = useState<RecordingState[]>([]);
  const [open, setOpen] = useState<string | null>(null);
  const rec = link.state?.recording ?? null;
  const nowUs = link.state?.measuredAtUs ?? 0;
  const { listRecordings } = link;

  useEffect(() => {
    let stop = false;
    const load = async () => {
      const r = await listRecordings();
      if (!stop) setHistory(r.slice(0, 10));
    };
    void load();
    const t = setInterval(() => void load(), 3000);
    return () => {
      stop = true;
      clearInterval(t);
    };
  }, [listRecordings, rec?.recordingId, rec?.state]);

  const act = async () => {
    if (pending) return;
    setPending(true);
    const r = rec ? await link.stopRecording(rec.recordingId) : await link.startRecording();
    setMessage(r.ok ? { text: rec ? `${rec.recordingId} stopped` : 'recording started', ok: true } : { text: r.reason, ok: false });
    setPending(false);
  };

  const shown = history.find((r) => r.recordingId === open) ?? null;

  return (
    <section className="panel">
      <header>
        <h2>Recording</h2>
        {rec && <span className="pill rec">REC {rec.recordingId}</span>}
      </header>
      <div className="row">
        <button type="button" className={rec ? 'danger' : 'primary'} disabled={!live || pending} onClick={() => void act()}>
          {pending ? '…' : rec ? 'Stop recording' : 'Start recording'}
        </button>
        {rec && (
          <span className="mono">
            {ageUs(nowUs - rec.startedAtUs)} · {rec.frames} frames · {bytes(rec.bytes)} ·{' '}
            <span className={rec.gaps.length ? 'warn' : ''}>gaps: {gapSummary(rec.gaps)}</span>
          </span>
        )}
      </div>
      {message && <p className={`status-line ${message.ok ? 'good' : 'bad'}`}>{message.text}</p>}
      {history.length > 0 && (
        <table className="list">
          <thead>
            <tr>
              <th>ID</th>
              <th>Started</th>
              <th>Frames</th>
              <th>Gaps</th>
              <th>Upload</th>
            </tr>
          </thead>
          <tbody>
            {history.map((r) => (
              <tr key={r.recordingId} className={open === r.recordingId ? 'selected' : ''}>
                <td className="mono">
                  <button type="button" className="link" onClick={() => setOpen(open === r.recordingId ? null : r.recordingId)}>
                    {r.recordingId}
                  </button>
                </td>
                <td className="mono" title={r.startedAtWall}>{r.startedAtWall.slice(11, 19)}</td>
                <td className="mono" title={bytes(r.bytes)}>
                  {r.frames}
                  {r.state === 'stopped' && !r.complete && <span className="warn" title="did not stop normally"> ⚠</span>}
                </td>
                <td className={`mono ${r.gaps.length ? 'warn' : ''}`}>{r.gaps.length}</td>
                <td className={r.upload === 'uploaded' ? 'good' : 'warn'}>{r.state === 'recording' ? 'recording' : r.upload}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      {shown && (
        <div className="gaps">
          <h3>
            {shown.recordingId} · {shown.complete ? 'complete' : shown.state === 'recording' ? 'recording' : 'not complete: it did not stop normally'}
          </h3>
          <p className="mono muted">gaps: {gapSummary(shown.gaps)}</p>
          <GapList gaps={shown.gaps} />
        </div>
      )}
    </section>
  );
}

export function UploadPanel({ state, serverNowUs }: { state: ServerState; serverNowUs: number | null }) {
  const u = state.upload;
  return (
    <section className="panel">
      <header>
        <h2>Cloud upload</h2>
        <span className={`pill ${u.storage === 'connected' ? 'good' : 'bad'}`}>{u.storage}</span>
      </header>
      <table className="kv">
        <tbody>
          <tr>
            <th>Backlog</th>
            <td className="mono">
              {u.backlogObjects} objects · {bytes(u.backlogBytes)}
            </td>
          </tr>
          <tr>
            <th>Oldest not confirmed</th>
            <td className="mono">{since(serverNowUs, u.oldestPendingUs)}</td>
          </tr>
          <tr>
            <th>Last contact</th>
            <td className="mono">{since(serverNowUs, u.lastContactUs)}</td>
          </tr>
        </tbody>
      </table>
    </section>
  );
}

export function AlarmList({ alarms, serverNowUs }: { alarms: Alarm[]; serverNowUs: number | null }) {
  if (alarms.length === 0) return null;
  return (
    <section className="panel alarms" aria-live="polite">
      <header>
        <h2>Alarms</h2>
      </header>
      <ul>
        {alarms.map((a, i) => (
          <li key={`${a.code}-${a.cameraId}-${i}`} className={a.severity === 'alarm' ? 'bad' : 'warn'}>
            <b>{a.severity}</b> {a.message} <span className="muted">· {since(serverNowUs, a.sinceUs)}</span>
          </li>
        ))}
      </ul>
    </section>
  );
}

export function EventLog({ events, serverNowUs }: { events: ServerEvent[]; serverNowUs: number | null }) {
  return (
    <section className="panel events">
      <header>
        <h2>Events</h2>
      </header>
      {events.length === 0 ? (
        <p className="muted">No events since the page opened.</p>
      ) : (
        <ol>
          {events.slice(0, 30).map((e) => (
            <li key={e.id} className={e.level === 'info' ? '' : e.level === 'alarm' ? 'bad' : 'warn'}>
              <span className="muted mono">{since(serverNowUs, e.atUs)}</span> {e.message}
            </li>
          ))}
        </ol>
      )}
    </section>
  );
}

// The data path: how fast frames arrive, how full the buffer is, how fast the
// disk writes. A buffer that fills under steady load means the disk (or the
// writer) cannot keep up.
function Bar({ used, high, capacity }: { used: number; high?: number; capacity: number }) {
  const pct = capacity > 0 ? Math.min(100, (used / capacity) * 100) : 0;
  const hw = capacity > 0 && high !== undefined ? Math.min(100, (high / capacity) * 100) : null;
  const cls = pct > 80 ? 'bad' : pct > 50 ? 'warn' : 'good';
  return (
    <div className="bar" role="meter" aria-valuemin={0} aria-valuemax={capacity} aria-valuenow={used}>
      <div className={`fill ${cls}`} style={{ width: `${pct}%` }} />
      {hw !== null && <div className="mark" style={{ left: `${hw}%` }} title="high water" />}
    </div>
  );
}

export function PipelinePanel({ pipeline: p }: { pipeline: Pipeline }) {
  return (
    <section className="panel">
      <header>
        <h2>Pipeline</h2>
        <span className="muted mono">memory {bytes(p.memoryBytes)}</span>
      </header>
      <table className="kv">
        <tbody>
          <tr>
            <th>Ingest</th>
            <td className="mono">
              {((p.ingestBytesPerS * 8) / 1e6).toFixed(1)} Mb/s · {p.ingestFramesPerS.toFixed(0)} frames/s
            </td>
          </tr>
          <tr>
            <th>Buffer</th>
            <td>
              <Bar used={p.buffer.usedBytes} high={p.buffer.highWaterBytes} capacity={p.buffer.capacityBytes} />
              <span className="mono muted">
                {bytes(p.buffer.usedBytes)} of {bytes(p.buffer.capacityBytes)} · high water {bytes(p.buffer.highWaterBytes)}
              </span>
            </td>
          </tr>
          <tr>
            <th>Disk</th>
            <td>
              <Bar used={p.disk.usedBytes} capacity={p.disk.quotaBytes} />
              <span className="mono muted">
                {bytes(p.disk.usedBytes)} of {bytes(p.disk.quotaBytes)} · writing {((p.disk.writeBytesPerS * 8) / 1e6).toFixed(1)} Mb/s
              </span>
            </td>
          </tr>
        </tbody>
      </table>
    </section>
  );
}
