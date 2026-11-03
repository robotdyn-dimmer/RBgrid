#!/usr/bin/env node
/**
 * ui-upload — install the embedded UI kit onto a hub (firmware note embed-and-mobile, canon TBD):
 *   1. POST /api/system/ui-upload?path=<stored>   raw bytes, application/octet-stream → staging ui_next/
 *   2. POST /api/system/ui-commit                 body = build-embedded/ui-manifest.json
 *      → the hub verifies every staged file's sha256/size, then swaps ui_next → ui atomically.
 * A failed commit changes nothing (the firmware stub page keeps working).
 *
 *   RBGRID_TOKEN=<admin token> npm run ui:upload -- http://<hub-ip>
 *
 * The token is read from the environment only — never pass it on the command line or store it.
 */
const fs = require('fs');
const path = require('path');

const base = (process.argv[2] || '').replace(/\/+$/, '');
if (!/^https?:\/\/.+/.test(base)) {
  console.error('usage: RBGRID_TOKEN=… npm run ui:upload -- http://<hub>');
  process.exit(2);
}
const OUT = path.resolve(__dirname, '..', 'build-embedded');
const manifest = JSON.parse(fs.readFileSync(path.join(OUT, 'ui-manifest.json'), 'utf8'));
const auth = process.env.RBGRID_TOKEN ? { Authorization: `Bearer ${process.env.RBGRID_TOKEN}` } : {};

(async () => {
  for (const f of manifest.files) {
    const body = fs.readFileSync(path.join(OUT, 'kit', f.stored));
    // The hub reads ?path= verbatim (no URL-decoding): send it raw. Allowed charset per root:
    // [A-Za-z0-9._/+-], ≤120 chars — refuse anything else instead of percent-encoding it.
    if (!/^[A-Za-z0-9._\/+-]{1,120}$/.test(f.stored)) throw new Error(`path not allowed on the hub: ${f.stored}`);
    const res = await fetch(`${base}/api/system/ui-upload?path=${f.stored}`, {
      method: 'POST', headers: { 'Content-Type': 'application/octet-stream', ...auth }, body,
    });
    if (!res.ok) throw new Error(`upload ${f.stored}: HTTP ${res.status} ${await res.text()}`);
    console.log(`  ↑ ${f.stored} (${f.size} B)`);
  }
  const res = await fetch(`${base}/api/system/ui-commit`, {
    method: 'POST', headers: { 'Content-Type': 'application/json', ...auth }, body: JSON.stringify(manifest),
  });
  const text = await res.text();
  if (!res.ok) throw new Error(`commit: HTTP ${res.status} ${text}`);
  console.log(`✔ committed ${manifest.version} (${manifest.files.length} files): ${text}`);
})().catch((e) => { console.error('✖', e.message); process.exit(1); });
