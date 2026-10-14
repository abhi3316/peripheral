// The cloud storage (README.md, "The cloud storage"): the remote object
// store that stands for the cloud, with faults.
// It checks every body's SHA-256 and keeps only the digest and size, unless a
// keep directory is given. With a keys file, it remembers every key across
// restarts, so that a key is for ever.

import { createServer, type IncomingMessage, type Server, type ServerResponse } from 'node:http';
import type { Socket } from 'node:net';
import { createHash } from 'node:crypto';
import { appendFileSync, createWriteStream, existsSync, mkdirSync, readFileSync, renameSync, rmSync } from 'node:fs';
import { dirname, join } from 'node:path';

export interface StorageOptions {
  host: string;
  port: number;
  adminPort: number;
  tokens: Record<string, string>; // serverId → token; empty = any server, token "dev-token"
  keepDir: string | null;
  keysFile: string | null; // one JSON line per stored object; null = memory only
  uplinkBytesPerSecond: number; // the link that all uploads share; 0 = no limit
  quiet: boolean;
}

export interface StoredObject {
  serverId: string;
  key: string;
  sha256: string;
  bytes: number;
  firstAtMs: number;
  puts: number; // successful PUTs (201 + 200)
  conflicts: number;
}

interface Faults {
  down: boolean;
  bytesPerSecond: number;
  errorRate: number;
  dropAfterBytes: number;
  dropCount: number;
}

const KEY = /^[A-Za-z0-9._/-]{1,200}$/;
const SERVER_ID = /^[a-z0-9_-]{1,16}$/;

export class StorageServer {
  private objects = new Map<string, StoredObject>();
  private status = new Map<string, { atMs: number; body: unknown }>();
  private faults: Faults = { down: false, bytesPerSecond: 0, errorRate: 0, dropAfterBytes: 0, dropCount: 0 };
  private api: Server | null = null;
  private admin: Server | null = null;
  private sockets = new Set<Socket>();
  private counts = { puts: 0, created: 0, exists: 0, conflicts: 0, mismatches: 0, errors503: 0, dropped: 0, unauthorized: 0, bytesIn: 0 };
  private rand = Math.random;
  // When the bytes received so far will all have crossed the shared link.
  private linkFreeAt = 0;

  constructor(readonly opts: StorageOptions) {}

  private log(s: string): void {
    if (!this.opts.quiet) console.log(`[storage] ${s}`);
  }

  // Bytes per second over the shared link: the uplink, or slower while the
  // bandwidth fault is on. 0 = no limit.
  private linkRate(): number {
    const rates = [this.opts.uplinkBytesPerSecond, this.faults.bytesPerSecond].filter((r) => r > 0);
    return rates.length ? Math.min(...rates) : 0;
  }

  // Every key stored before, from the keys file.
  private loadKeys(): void {
    const f = this.opts.keysFile;
    if (!f) return;
    mkdirSync(dirname(f), { recursive: true });
    if (!existsSync(f)) return;
    for (const line of readFileSync(f, 'utf8').split('\n')) {
      if (!line.trim()) continue;
      try {
        const o = JSON.parse(line) as Omit<StoredObject, 'puts' | 'conflicts'>;
        this.objects.set(`${o.serverId}/${o.key}`, { ...o, puts: 0, conflicts: 0 });
      } catch {
        /* a line cut by a crash: skip it */
      }
    }
    this.log(`${this.objects.size} keys from ${f}`);
  }

  async start(): Promise<{ api: number; admin: number }> {
    this.loadKeys();
    this.admin = createServer((req, res) => void this.adminHttp(req, res));
    const admin = await this.listen(this.admin, this.opts.adminPort);
    const api = await this.openApi(this.opts.port);
    return { api, admin };
  }

  private listen(s: Server, port: number): Promise<number> {
    return new Promise((resolve, reject) => {
      s.once('error', reject);
      s.listen(port, this.opts.host, () => {
        const a = s.address();
        resolve(typeof a === 'object' && a ? a.port : port);
      });
    });
  }

  private apiPort = 0;

  private async openApi(port: number): Promise<number> {
    this.api = createServer((req, res) => void this.handle(req, res));
    this.api.on('connection', (s: Socket) => {
      this.sockets.add(s);
      s.on('close', () => this.sockets.delete(s));
    });
    this.apiPort = await this.listen(this.api, port);
    return this.apiPort;
  }

  private async setDown(down: boolean): Promise<void> {
    if (down === this.faults.down) return;
    this.faults.down = down;
    if (down) {
      const s = this.api;
      this.api = null;
      for (const sock of this.sockets) sock.destroy();
      await new Promise<void>((r) => (s ? s.close(() => r()) : r()));
      this.log('down');
    } else {
      await this.openApi(this.apiPort);
      this.log('up');
    }
  }

  async stop(): Promise<void> {
    for (const s of this.sockets) s.destroy();
    await Promise.all([this.api, this.admin].map((s) => new Promise<void>((r) => (s ? s.close(() => r()) : r()))));
  }

  // ---- API ------------------------------------------------------------------------

  private json(res: ServerResponse, status: number, body: unknown): void {
    if (res.headersSent) return;
    res.writeHead(status, { 'Content-Type': 'application/json', Connection: 'keep-alive' });
    res.end(JSON.stringify(body));
  }

  private authorized(req: IncomingMessage, serverId: string): boolean {
    const want = Object.keys(this.opts.tokens).length ? this.opts.tokens[serverId] : 'dev-token';
    return !!want && req.headers.authorization === `Bearer ${want}`;
  }

  private async handle(req: IncomingMessage, res: ServerResponse): Promise<void> {
    const url = new URL(req.url ?? '/', 'http://x');
    const m = /^\/v1\/servers\/([^/]+)\/(objects\/(.+)|status)$/.exec(url.pathname);
    if (!m) return this.json(res, 404, { error: 'not_found' });
    const serverId = decodeURIComponent(m[1]!);
    if (!SERVER_ID.test(serverId)) return this.json(res, 400, { error: 'bad_server_id' });
    if (!this.authorized(req, serverId)) {
      this.counts.unauthorized++;
      req.resume();
      return this.json(res, 401, { error: 'unauthorized' });
    }
    if (m[2] === 'status') {
      if (req.method !== 'POST') return this.json(res, 405, { error: 'method' });
      const chunks: Buffer[] = [];
      let n = 0;
      for await (const c of req) {
        n += (c as Buffer).length;
        if (n > 65536) return this.json(res, 413, { error: 'too_large' });
        chunks.push(c as Buffer);
      }
      try {
        this.status.set(serverId, { atMs: Date.now(), body: JSON.parse(Buffer.concat(chunks).toString()) });
      } catch {
        return this.json(res, 400, { error: 'bad_json' });
      }
      res.writeHead(204).end();
      return;
    }
    const key = decodeURIComponent(m[3]!);
    if (!KEY.test(key) || key.includes('..')) return this.json(res, 400, { error: 'bad_key' });
    const id = `${serverId}/${key}`;
    if (req.method === 'HEAD') {
      const o = this.objects.get(id);
      if (!o) return void res.writeHead(404).end();
      res.writeHead(200, { 'X-Content-SHA256': o.sha256, 'Content-Length': String(o.bytes) }).end();
      return;
    }
    if (req.method !== 'PUT') return this.json(res, 405, { error: 'method' });
    return this.put(req, res, serverId, key, id);
  }

  private async put(req: IncomingMessage, res: ServerResponse, serverId: string, key: string, id: string): Promise<void> {
    this.counts.puts++;
    const lenHeader = req.headers['content-length'];
    if (lenHeader === undefined) return this.json(res, 411, { error: 'length_required' });
    const length = Number(lenHeader);
    const want = String(req.headers['x-content-sha256'] ?? '');
    if (!/^[0-9a-f]{64}$/.test(want)) {
      req.resume();
      return this.json(res, 400, { error: 'bad_digest_header' });
    }
    if (this.faults.errorRate > 0 && this.rand() < this.faults.errorRate) {
      this.counts.errors503++;
      req.resume();
      return this.json(res, 503, { error: 'unavailable' });
    }
    const dropAt = this.faults.dropCount > 0 ? this.faults.dropAfterBytes : -1;
    if (dropAt >= 0) this.faults.dropCount--;

    const hash = createHash('sha256');
    let got = 0;
    const tmp = this.opts.keepDir ? join(this.opts.keepDir, '.tmp', `${process.pid}-${Math.random().toString(36).slice(2)}`) : null;
    let file: ReturnType<typeof createWriteStream> | null = null;
    if (tmp) {
      mkdirSync(dirname(tmp), { recursive: true });
      file = createWriteStream(tmp);
    }
    try {
      for await (const chunk of req) {
        const c = chunk as Buffer;
        got += c.length;
        this.counts.bytesIn += c.length;
        hash.update(c);
        if (file && !file.write(c)) await new Promise<void>((r) => file!.once('drain', () => r()));
        if (dropAt >= 0 && got >= dropAt) {
          this.counts.dropped++;
          req.socket.destroy();
          throw new Error('dropped');
        }
        const rate = this.linkRate();
        if (rate > 0) {
          const now = Date.now();
          this.linkFreeAt = Math.max(this.linkFreeAt, now) + (c.length / rate) * 1000;
          const wait = this.linkFreeAt - now;
          if (wait > 10) {
            req.pause();
            await new Promise((r) => setTimeout(r, wait));
            req.resume();
          }
        }
      }
    } catch {
      file?.destroy();
      if (tmp) rmSync(tmp, { force: true });
      return;
    }
    if (file) await new Promise<void>((r) => file!.end(() => r()));
    if (got !== length) {
      if (tmp) rmSync(tmp, { force: true });
      return this.json(res, 400, { error: 'length_mismatch' });
    }
    const sha = hash.digest('hex');
    if (sha !== want) {
      this.counts.mismatches++;
      if (tmp) rmSync(tmp, { force: true });
      return this.json(res, 422, { error: 'digest_mismatch' });
    }
    const have = this.objects.get(id);
    if (have) {
      if (tmp) rmSync(tmp, { force: true });
      if (have.sha256 !== sha) {
        have.conflicts++;
        this.counts.conflicts++;
        this.log(`conflict ${id}`);
        return this.json(res, 409, { error: 'conflict' });
      }
      have.puts++;
      this.counts.exists++;
      return this.json(res, 200, { status: 'exists', sha256: sha, bytes: got });
    }
    if (tmp && this.opts.keepDir) {
      const dest = join(this.opts.keepDir, serverId, key);
      mkdirSync(dirname(dest), { recursive: true });
      renameSync(tmp, dest);
    }
    const o: StoredObject = { serverId, key, sha256: sha, bytes: got, firstAtMs: Date.now(), puts: 1, conflicts: 0 };
    this.objects.set(id, o);
    if (this.opts.keysFile) appendFileSync(this.opts.keysFile, JSON.stringify({ serverId, key, sha256: sha, bytes: got, firstAtMs: o.firstAtMs }) + '\n');
    this.counts.created++;
    this.log(`stored ${id} (${got} bytes)`);
    return this.json(res, 201, { status: 'stored', sha256: sha, bytes: got });
  }

  // ---- admin ----------------------------------------------------------------------

  private async adminHttp(req: IncomingMessage, res: ServerResponse): Promise<void> {
    const url = new URL(req.url ?? '/', 'http://x');
    const send = (status: number, body: unknown) => {
      res.writeHead(status, { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' });
      res.end(JSON.stringify(body));
    };
    if (req.method === 'OPTIONS') {
      res.writeHead(204, { 'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Methods': 'GET, POST', 'Access-Control-Allow-Headers': 'Content-Type' });
      return void res.end();
    }
    if (req.method === 'GET' && url.pathname === '/admin/objects') return send(200, { objects: [...this.objects.values()] });
    if (req.method === 'GET' && url.pathname === '/admin/status') {
      return send(200, { servers: Object.fromEntries([...this.status].map(([k, v]) => [k, v])) });
    }
    if (req.method === 'GET' && url.pathname === '/admin/stats') return send(200, { ...this.counts, faults: this.faults, objects: this.objects.size, linkBytesPerSecond: this.linkRate() });
    if (req.method === 'POST' && url.pathname === '/admin/fault') {
      const chunks: Buffer[] = [];
      for await (const c of req) chunks.push(c as Buffer);
      const b = (chunks.length ? JSON.parse(Buffer.concat(chunks).toString()) : {}) as Record<string, unknown>;
      if (b.clear) {
        this.faults.bytesPerSecond = 0;
        this.faults.errorRate = 0;
        this.faults.dropCount = 0;
        await this.setDown(false);
      }
      if (typeof b.down === 'boolean') await this.setDown(b.down);
      if (b.bytesPerSecond !== undefined) this.faults.bytesPerSecond = Math.max(0, Number(b.bytesPerSecond));
      if (b.errorRate !== undefined) this.faults.errorRate = Math.max(0, Math.min(1, Number(b.errorRate)));
      if (b.dropAfterBytes !== undefined) {
        this.faults.dropAfterBytes = Math.max(0, Number(b.dropAfterBytes));
        this.faults.dropCount = Math.max(1, Number(b.count ?? 1));
      }
      this.log(`faults ${JSON.stringify(this.faults)}`);
      return send(200, { faults: this.faults });
    }
    return send(404, { error: 'not_found' });
  }
}
