// npm run storage -- [--forget] [--uplink-mbps 250] [--keys-file file] [--port 7100] [--admin-port 7101] [--token name=secret ...] [--keep-dir dir]
import { rmSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';
import { StorageServer } from './server.ts';

const { values } = parseArgs({
  options: {
    host: { type: 'string', default: '127.0.0.1' },
    port: { type: 'string', default: '7100' },
    'admin-port': { type: 'string', default: '7101' },
    token: { type: 'string', multiple: true, default: [] },
    'keep-dir': { type: 'string' },
    'keys-file': { type: 'string', default: join(dirname(fileURLToPath(import.meta.url)), '..', 'storage-data', 'keys.jsonl') },
    forget: { type: 'boolean', default: false },
    'uplink-mbps': { type: 'string', default: '250' },
    quiet: { type: 'boolean', default: false },
  },
});

const tokens: Record<string, string> = {};
for (const t of values.token ?? []) {
  const [id, tok] = t.split('=');
  if (id && tok) tokens[id] = tok;
}
if (values.forget) rmSync(values['keys-file']!, { force: true });
const storage = new StorageServer({
  host: values.host!,
  port: Number(values.port),
  adminPort: Number(values['admin-port']),
  tokens,
  keepDir: values['keep-dir'] ?? null,
  keysFile: values['keys-file']!,
  uplinkBytesPerSecond: (Number(values['uplink-mbps']) * 1e6) / 8,
  quiet: values.quiet!,
});
const p = await storage.start();
console.log(`cloud storage on http://${values.host}:${p.api}  admin http://${values.host}:${p.admin}/admin/objects`);
console.log(Object.keys(tokens).length ? `  servers: ${Object.keys(tokens).join(', ')}` : '  any server, token "dev-token"');
console.log(`  uplink ${values['uplink-mbps']} Mb/s, shared by all uploads; keys kept in ${values['keys-file']} (--forget starts empty)`);
const stop = () => void storage.stop().then(() => process.exit(0));
process.on('SIGINT', stop);
process.on('SIGTERM', stop);
