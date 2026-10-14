// Pan seen from above, tilt seen from the side. The grey arcs are the travel
// limits; the dashed needle is the goto target.

import type { CameraState } from '../server/types.ts';
import { deg } from '../format.ts';

const R = 52;

// Angle convention: 0° points up on the screen; positive is clockwise (pan right).
function point(cx: number, cy: number, angleDeg: number, r = R): [number, number] {
  const a = (angleDeg * Math.PI) / 180;
  return [cx + r * Math.sin(a), cy - r * Math.cos(a)];
}

function arc(cx: number, cy: number, from: number, to: number, r = R): string {
  const [x1, y1] = point(cx, cy, from, r);
  const [x2, y2] = point(cx, cy, to, r);
  const large = Math.abs(to - from) > 180 ? 1 : 0;
  return `M ${x1} ${y1} A ${r} ${r} 0 ${large} 1 ${x2} ${y2}`;
}

function Needle({ cx, cy, angle, dashed }: { cx: number; cy: number; angle: number; dashed?: boolean }) {
  const [x, y] = point(cx, cy, angle, R - 6);
  return (
    <line
      x1={cx}
      y1={cy}
      x2={x}
      y2={y}
      className={dashed ? 'needle target' : 'needle'}
      strokeDasharray={dashed ? '4 3' : undefined}
    />
  );
}

export function PoseView({ camera }: { camera: CameraState }) {
  const { limits: l, pose, target } = camera;
  const relative = !camera.homed;
  // Tilt is drawn on the side view: 0° tilt = pointing right (screen angle 90°).
  const tiltScreen = (t: number) => 90 - t;
  return (
    <section className="panel">
      <header>
        <h2>Position</h2>
        {relative && <span className="pill warn">relative — not homed</span>}
      </header>
      <div className={`pose ${relative ? 'is-relative' : ''}`}>
        <figure>
          <svg viewBox="0 0 140 140" role="img" aria-label={`pan ${pose?.panDeg ?? 'unknown'} degrees`}>
            <path d={arc(70, 70, l.panMinDeg, l.panMaxDeg)} className="travel" />
            <path d={arc(70, 70, l.panMaxDeg, 360 + l.panMinDeg)} className="no-go" />
            {target && <Needle cx={70} cy={70} angle={target.panDeg} dashed />}
            {pose && <Needle cx={70} cy={70} angle={pose.panDeg} />}
            <circle cx={70} cy={70} r={4} className="hub" />
            <text x={70} y={12} textAnchor="middle" className="tick">0°</text>
          </svg>
          <figcaption>
            Pan <b className="mono">{deg(pose?.panDeg)}</b>
            <span className="muted mono"> {camera.velocity.panDegPerS.toFixed(1)} °/s</span>
          </figcaption>
        </figure>
        <figure>
          <svg viewBox="0 0 140 140" role="img" aria-label={`tilt ${pose?.tiltDeg ?? 'unknown'} degrees`}>
            <path d={arc(40, 70, tiltScreen(l.tiltMaxDeg), tiltScreen(l.tiltMinDeg))} className="travel" transform="translate(0,0)" />
            <line x1={40} y1={70} x2={100} y2={70} className="horizon" />
            {target && <Needle cx={40} cy={70} angle={tiltScreen(target.tiltDeg)} dashed />}
            {pose && <Needle cx={40} cy={70} angle={tiltScreen(pose.tiltDeg)} />}
            <circle cx={40} cy={70} r={4} className="hub" />
          </svg>
          <figcaption>
            Tilt <b className="mono">{deg(pose?.tiltDeg)}</b>
            <span className="muted mono"> {camera.velocity.tiltDegPerS.toFixed(1)} °/s</span>
          </figcaption>
        </figure>
      </div>
    </section>
  );
}
