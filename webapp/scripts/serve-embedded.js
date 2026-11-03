#!/usr/bin/env node
/**
 * serve-embedded — a "hub simulator" for the embedded UI kit: serves build-embedded/kit exactly
 * the way the hub will (firmware note embed-and-mobile §A5) and forwards the hub routes to a real
 * hub, so the embedded build can be tested same-origin before the firmware embed canon lands.
 *
 *   npm run build:embedded && node scripts/serve-embedded.js http://<hub-ip> [port=3740]
 *
 * Static (as on the hub): GET <path> → <path>.gz with Content-Encoding: gzip when present, else
 * <path>; `/` → index.html; Cache-Control immutable for hashed names, no-cache for index.html.
 * Proxied to the real hub: /api/*, /wifi/*, /ws-telemetry (incl. the WebSocket upgrade).
 */
const fs = require('fs');
const http = require('http');
const net = require('net');
const path = require('path');

if (!/^https?:\/\/.+/.test(process.argv[2] || '')) {
  console.error('usage: node scripts/serve-embedded.js http://<hub-ip> [port]');
  process.exit(2);
}
const target = new URL(process.argv[2]);
const port = Number(process.argv[3] || 3740);
const KIT = path.resolve(__dirname, '..', 'build-embedded', 'kit');
const manifest = JSON.parse(fs.readFileSync(path.resolve(KIT, '..', 'ui-manifest.json'), 'utf8'));
const byPath = new Map(manifest.files.map((f) => [f.path, f]));
/**
 * The simulator runs on http://localhost — a SECURE context for Chrome — so the browser adds
 * client hints (sec-ch-ua*) that it does NOT send to a real http://192.168.x.x hub. Forwarding
 * them pushes requests past the hub's 512 B header limit (HTTP 431). Drop them to behave like
 * the real http hub (set KEEP_CLIENT_HINTS=1 to reproduce the HTTPS-hub case).
 */
const forwardHeaders = (h) => Object.fromEntries(
  Object.entries(h).filter(([k]) => process.env.KEEP_CLIENT_HINTS || !k.startsWith('sec-ch-')),
);
const proxied = (url) => /^\/(api|wifi)\//.test(url) || url.startsWith('/ws-telemetry');

const server = http.createServer((req, res) => {
  const url = req.url || '/';
  if (proxied(url)) {
    if (process.env.LOG_HEADERS) {
      const raw = Object.entries(req.headers).map(([k, v]) => `${k}: ${v}`);
      console.log(`${req.method} ${url.split('?')[0]}  headers=${raw.join('\r\n').length} B (${raw.length})\n  ` + raw.map((l) => `${l.length}\t${l.slice(0, 110)}`).join('\n  '));
    }
    const up = http.request(
      { host: target.hostname, port: target.port || 80, path: url, method: req.method, headers: { ...forwardHeaders(req.headers), host: target.host } },
      (r) => { res.writeHead(r.statusCode || 502, r.headers); r.pipe(res); },
    );
    up.on('error', (e) => { res.writeHead(502); res.end(String(e)); });
    req.pipe(up);
    return;
  }
  const rel = decodeURIComponent(url.split('?')[0]).replace(/^\/+/, '') || 'index.html';
  const f = byPath.get(rel);
  if (!f) { res.writeHead(404, { 'Content-Type': 'text/plain' }); res.end('not in UI kit'); return; }
  const hashed = /\.[0-9a-f]{8}\./.test(rel);
  res.writeHead(200, {
    'Content-Type': f.content_type,
    ...(f.encoding === 'gzip' ? { 'Content-Encoding': 'gzip' } : {}),
    'Cache-Control': rel === 'index.html' ? 'no-cache' : hashed ? 'public, max-age=31536000, immutable' : 'public, max-age=3600',
  });
  fs.createReadStream(path.join(KIT, f.stored)).pipe(res);
});

// WebSocket upgrade → raw TCP pipe to the hub.
server.on('upgrade', (req, socket, head) => {
  if (!proxied(req.url || '')) { socket.destroy(); return; }
  const up = net.connect(Number(target.port || 80), target.hostname, () => {
    const lines = [`${req.method} ${req.url} HTTP/1.1`];
    for (const [k, v] of Object.entries({ ...forwardHeaders(req.headers), host: target.host })) lines.push(`${k}: ${v}`);
    up.write(lines.join('\r\n') + '\r\n\r\n');
    if (head && head.length) up.write(head);
    up.pipe(socket); socket.pipe(up);
  });
  up.on('error', () => socket.destroy());
  socket.on('error', () => up.destroy());
});

server.listen(port, '0.0.0.0', () => console.log(`hub simulator: http://localhost:${port} → UI kit ${manifest.version}, API → ${target.origin}`));
