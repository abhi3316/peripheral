// The latest image of the selected camera, with the server's pose and — in the
// simulation — the truth stamp and the error between them.

import { useEffect, useRef } from 'react';
import type { ServerLink } from '../server/link.ts';
import type { Preview } from '../server/types.ts';
import { deg } from '../format.ts';

type Received = NonNullable<ServerLink['preview']>;

// The truth stamp: true pan, tilt (0.001°) and frame_seq, little-endian.
function readTruthStamp(img: Preview): { panDeg: number; tiltDeg: number; frameSeq: number } | null {
  const b = img.truthStamp;
  if (!b || b.length < 12) return null;
  const v = new DataView(b.buffer, b.byteOffset, 12);
  return { panDeg: v.getInt32(0, true) / 1000, tiltDeg: v.getInt32(4, true) / 1000, frameSeq: v.getUint32(8, true) };
}

const IMAGE_STALE_MS = 1500;

function errorClass(e: number): string {
  const a = Math.abs(e);
  if (a < 0.25) return 'good';
  if (a < 1) return 'warn';
  return 'bad';
}

export function ImageView({
  received,
  nowMs,
  serverNowUs,
  live,
}: {
  received: Received | null;
  nowMs: number;
  serverNowUs: number | null;
  live: boolean;
}) {
  const canvas = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const c = canvas.current;
    if (!c || !received) return;
    const { image } = received;
    c.width = image.width;
    c.height = image.height;
    const ctx = c.getContext('2d');
    if (!ctx) return;
    const rgba = ctx.createImageData(image.width, image.height);
    for (let i = 0; i < image.pixels.length; i++) {
      const v = image.pixels[i]!;
      rgba.data[i * 4] = v;
      rgba.data[i * 4 + 1] = v;
      rgba.data[i * 4 + 2] = v;
      rgba.data[i * 4 + 3] = 255;
    }
    ctx.putImageData(rgba, 0, 0);
  }, [received]);

  const img = received?.image ?? null;
  const ageMs = received ? nowMs - received.receivedAtMs : null;
  const stale = !live || ageMs === null || ageMs > IMAGE_STALE_MS;
  const truth = img ? readTruthStamp(img) : null;
  const err =
    img && truth && img.pose && img.homed
      ? { panDeg: img.pose.panDeg - truth.panDeg, tiltDeg: img.pose.tiltDeg - truth.tiltDeg, seqMatches: truth.frameSeq === img.frameSeq }
      : null;
  // Exposure end → the moment the page received it, on the server timebase.
  const latencyMs =
    img && received && serverNowUs !== null
      ? (serverNowUs - (nowMs - received.receivedAtMs) * 1000 - img.exposureEndUs) / 1000
      : null;

  return (
    <section className="panel image-panel">
      <header>
        <h2>Camera</h2>
        {img && <span className="muted mono">{img.cameraId} #{img.frameSeq}</span>}
        {img?.recorded && <span className="pill rec">REC</span>}
      </header>
      <div className={`image-frame ${stale ? 'is-stale' : ''}`} style={img ? { aspectRatio: `${img.width} / ${img.height}` } : undefined}>
        <canvas ref={canvas} width={320} height={180} aria-label="latest camera image (preview)" />
        <div className="crosshair" aria-hidden />
        {stale && (
          <div className="image-overlay">
            {received ? `No new image for ${((ageMs ?? 0) / 1000).toFixed(1)} s` : 'No image yet'}
          </div>
        )}
      </div>
      {img && (
        <table className="kv">
          <tbody>
            <tr>
              <th>Preview</th>
              <td className="mono" colSpan={3}>
                {img.width}×{img.height} of {img.sourceWidth}×{img.sourceHeight} · boot {img.bootId}
              </td>
            </tr>
            <tr>
              <th>Exposure</th>
              <td className="mono">{((img.exposureEndUs - img.exposureStartUs) / 1000).toFixed(1)} ms</td>
              <th>Latency</th>
              <td className="mono">
                {latencyMs === null ? '—' : `${latencyMs.toFixed(0)} ms`}
                {latencyMs !== null && (latencyMs < 0 || latencyMs > 10_000) && (
                  <span className="bad"> — are the image and state times on one clock?</span>
                )}
              </td>
            </tr>
            <tr>
              <th>Computed pose</th>
              <td className="mono" colSpan={3}>
                {img.pose ? `pan ${deg(img.pose.panDeg, 3)}  tilt ${deg(img.pose.tiltDeg, 3)}` : 'none'}
                {!img.homed && <span className="muted"> (relative — not homed)</span>}
              </td>
            </tr>
            <tr>
              <th>True pose</th>
              <td className="mono" colSpan={3}>
                {truth ? `pan ${deg(truth.panDeg, 3)}  tilt ${deg(truth.tiltDeg, 3)}` : '—'}
                <span className="muted"> (from the camera simulator's truth stamp)</span>
              </td>
            </tr>
            <tr>
              <th>Pose error</th>
              <td className="mono" colSpan={3}>
                {err ? (
                  <>
                    <span className={errorClass(err.panDeg)}>pan {deg(err.panDeg, 3)}</span>{' '}
                    <span className={errorClass(err.tiltDeg)}>tilt {deg(err.tiltDeg, 3)}</span>
                    {!err.seqMatches && <span className="bad"> — truth stamp seq ≠ header seq</span>}
                  </>
                ) : (
                  <span className="muted">not comparable (not homed, no pose, or no truth stamp)</span>
                )}
              </td>
            </tr>
          </tbody>
        </table>
      )}
    </section>
  );
}
