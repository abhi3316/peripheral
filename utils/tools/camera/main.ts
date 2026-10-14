// npm run camera -- [--cameras 2] [--motion ../data/motion.csv | --no-motion]
//
// Starts the simulated pan-tilt cameras. Camera i is cam<i>, with its control,
// video and admin ports at 7001, 7002 and 7000 plus 10 × i. Each one has its
// own clock and plays the recorded motion from a different point.
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';
import { MotionTrack } from './motion.ts';
import { CameraSimulator, STREAM } from './simulator.ts';

const defaultMotion = join(dirname(fileURLToPath(import.meta.url)), '..', '..', 'data', 'motion.csv');

// Per camera: clock start, clock error, and the point in the motion to start from.
const CLOCKS = [
  { counterStart: 4_000_000_000, ppm: 50, motionOffsetUs: 0 },
  { counterStart: 1_500_000_000, ppm: -30, motionOffsetUs: 30_000_000 },
  { counterStart: 2_700_000_000, ppm: 80, motionOffsetUs: 15_000_000 },
  { counterStart: 300_000_000, ppm: -60, motionOffsetUs: 45_000_000 },
];

const { values } = parseArgs({
  options: {
    host: { type: 'string', default: '127.0.0.1' },
    cameras: { type: 'string', default: '2' },
    motion: { type: 'string', default: defaultMotion },
    'no-motion': { type: 'boolean', default: false },
    quiet: { type: 'boolean', default: false },
  },
});

const count = Number(values.cameras);
if (!(count >= 1 && count <= CLOCKS.length)) {
  console.error(`--cameras must be 1 to ${CLOCKS.length}`);
  process.exit(2);
}

// The motion is always loaded, so that /admin/play works after --no-motion.
let motion: MotionTrack | null = null;
if (existsSync(values.motion!)) motion = MotionTrack.load(values.motion!);
else console.warn(`no motion file at ${values.motion}: the cameras stay idle`);

const { width, height, fps } = STREAM;
const cameras: CameraSimulator[] = [];
for (let i = 0; i < count; i++) {
  const camera = new CameraSimulator({
    host: values.host!,
    controlPort: 7001 + 10 * i,
    videoPort: 7002 + 10 * i,
    adminPort: 7000 + 10 * i,
    cameraId: `cam${i}`,
    width,
    height,
    fps,
    exposureUs: 10_000,
    motion,
    autoplay: !values['no-motion'],
    quiet: values.quiet!,
    ...CLOCKS[i]!,
  });
  const ports = await camera.start();
  cameras.push(camera);
  console.log(`cam${i}: control :${ports.control}  video :${ports.video}  admin http://${values.host}:${ports.admin}/admin/stats`);
}
const mbits = ((width * height + 32) * fps * 8) / 1e6;
console.log(`each camera: ${width}×${height} @ ${fps} frames/s = ${mbits.toFixed(0)} Mb/s`);
console.log(motion && !values['no-motion'] ? `playing ${values.motion} in a loop until the server sends a motion command` : 'cameras idle and not homed');
const stop = () => void Promise.all(cameras.map((c) => c.stop())).then(() => process.exit(0));
process.on('SIGINT', stop);
process.on('SIGTERM', stop);
