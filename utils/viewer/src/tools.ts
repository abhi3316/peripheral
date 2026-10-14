// The camera simulator's and the cloud storage's test interfaces, called
// directly, so that the Test panel works whether the page comes from the dev
// server or from your server. For testing only.

// The tools run on the same computer as the page's host.
const host = typeof window === 'undefined' || window.location.hostname === 'localhost' ? '127.0.0.1' : window.location.hostname;
const CAMERA_ADMIN: Record<string, string> = { cam0: `http://${host}:7000/admin`, cam1: `http://${host}:7010/admin` };
const STORAGE_ADMIN = `http://${host}:7101/admin`;

async function post(base: string, path: string, body: unknown): Promise<string | null> {
  try {
    const res = await fetch(`${base}${path}`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    if (res.ok) return null;
    const j = (await res.json().catch(() => ({}))) as { error?: string };
    return j.error ?? res.statusText;
  } catch (err) {
    return err instanceof Error ? err.message : String(err);
  }
}

export const CAMERAS = ['cam0', 'cam1'];

export interface CameraStats {
  cameraId: string;
  stream: { width: number; height: number; fps: number };
  sendBytesPerS: number;
  droppedByCamera: number;
  videoClient: boolean;
  controlClient: boolean;
  motion: string;
  playing: boolean;
}

export async function cameraStats(cameraId: string): Promise<CameraStats | null> {
  try {
    const res = await fetch(`${CAMERA_ADMIN[cameraId]}/stats`);
    if (!res.ok) return null;
    const j = (await res.json()) as CameraStats;
    return j.stream ? j : null;
  } catch {
    return null;
  }
}

export const cameraPost = (cameraId: string, path: string, body: unknown) => post(CAMERA_ADMIN[cameraId]!, path, body);
export const storageFault = (body: unknown) => post(STORAGE_ADMIN, '/fault', body);
