#!/usr/bin/env node
/**
 * build-embedded — the hub-installable UI kit (firmware note embed-and-mobile, 2026-10-13: HYBRID,
 * UI files in the hub's LittleFS `storage` partition + a stub page in firmware).
 *
 *   npm run build:embedded            → build-embedded/kit/**  + build-embedded/ui-manifest.json
 *                                       + build-embedded/rbgrid-ui_v<version>.zip (kit/ + ui-manifest.json)
 *   (a production build hides the preview features — ESP-NOW…; REACT_APP_PREVIEW_FEATURES=true shows them)
 *
 * What it does:
 *  1. CRA production build with REACT_APP_EMBEDDED=true (same-origin auto-connect, no device
 *     picker), relative PUBLIC_URL, no source maps.
 *  2. Keeps only what the hub must serve: index.html, static/js|css, fonts, manifest.json,
 *     icons, licence texts. Drops source maps, robots.txt, the CRA asset-manifest and the lazy
 *     MQTT chunk (broker mode is not reachable in the embedded UI — LAN same-origin only).
 *  3. Pre-compresses text assets with gzip -9 → `<path>.gz` (the hub never compresses on the fly;
 *     it serves `<path>.gz` with Content-Encoding: gzip). woff2/png are already compressed and
 *     are stored as-is.
 *  4. Writes ui-manifest.json = the body for POST /api/system/ui-commit:
 *       { name, version, api_version, built_at,
 *         files: [{ path, stored, size, sha256, content_type, encoding }] }
 *     `path` = URL path the browser requests; `stored` = the file name to upload
 *     (POST /api/system/ui-upload?path=<stored>); `sha256`/`size` are of the STORED bytes.
 */
const { execSync } = require('child_process');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { writeZip } = require('./zip');

const ROOT = path.resolve(__dirname, '..');
const OUT = path.join(ROOT, 'build-embedded');
const BUILD = path.join(OUT, 'build');
const KIT = path.join(OUT, 'kit');
const pkg = require(path.join(ROOT, 'package.json'));

/** UI ↔ firmware compatibility: the hub's schema `api_version` this UI was built against. */
const API_VERSION = (pkg.rbgrid && pkg.rbgrid.apiVersion) || 1;

const CONTENT_TYPE = {
  '.html': 'text/html; charset=utf-8', '.js': 'application/javascript', '.css': 'text/css',
  '.json': 'application/json', '.txt': 'text/plain; charset=utf-8', '.svg': 'image/svg+xml',
  '.png': 'image/png', '.ico': 'image/x-icon', '.woff2': 'font/woff2',
};
const PRECOMPRESSED = new Set(['.woff2', '.png', '.ico']);

function walk(dir) {
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
    const p = path.join(dir, e.name);
    return e.isDirectory() ? walk(p) : [p];
  });
}

function gitShort() {
  try { return execSync('git rev-parse --short HEAD', { cwd: ROOT }).toString().trim(); } catch { return 'nogit'; }
}

// 1. build
fs.rmSync(OUT, { recursive: true, force: true });
console.log('▶ design-system dist…');
execSync('node scripts/build-design-system.js', { cwd: ROOT, stdio: 'inherit' });
console.log('▶ react-scripts build (embedded profile)…');
execSync('npx react-scripts build', {
  cwd: ROOT,
  stdio: 'inherit',
  env: {
    ...process.env,
    REACT_APP_EMBEDDED: 'true',
    PUBLIC_URL: '.',
    BUILD_PATH: BUILD,
    GENERATE_SOURCEMAP: 'false',
    DISABLE_ESLINT_PLUGIN: 'true',
    INLINE_RUNTIME_CHUNK: 'false', // no inline <script> → stricter CSP possible on the hub
  },
});

// 2–4. select, compress, manifest
const skip = (rel) =>
  rel.endsWith('.map') || rel === 'robots.txt' || rel === 'asset-manifest.json' ||
  /(^|\/)mqtt\.[0-9a-f]+\.chunk\.js(\.LICENSE\.txt)?$/.test(rel);

const files = [];
for (const abs of walk(BUILD)) {
  const rel = path.relative(BUILD, abs).split(path.sep).join('/');
  if (skip(rel)) continue;
  const ext = path.extname(rel).toLowerCase();
  const raw = fs.readFileSync(abs);
  const gz = !PRECOMPRESSED.has(ext);
  const bytes = gz ? zlib.gzipSync(raw, { level: 9 }) : raw;
  const stored = gz ? `${rel}.gz` : rel;
  fs.mkdirSync(path.dirname(path.join(KIT, stored)), { recursive: true });
  fs.writeFileSync(path.join(KIT, stored), bytes);
  files.push({
    path: rel,
    stored,
    size: bytes.length,
    sha256: crypto.createHash('sha256').update(bytes).digest('hex'),
    content_type: CONTENT_TYPE[ext] || 'application/octet-stream',
    encoding: gz ? 'gzip' : 'identity',
  });
}
files.sort((a, b) => a.path.localeCompare(b.path));

// The hub serves ONLY these UI paths (root, canon cda68f4f: explicit patterns instead of a
// catch-all that swallowed late-registered API routes). A kit file outside them would be a
// silent 404 on the device → fail the build instead.
const HUB_UI_PATTERNS = [
  /^index\.html$/, /^static\//, /^fonts\//, /^manifest\.json$/, /^asset-manifest\.json$/,
  /^ui-manifest\.json$/, /^favicon\.[a-z0-9]+$/, /^apple-icon\.png$/, /^robots\.txt$/, /^icons\//, /^logo/,
];
const unserved = files.filter((f) => !HUB_UI_PATTERNS.some((re) => re.test(f.path)));
if (unserved.length) {
  console.error(`✖ not served by the hub (outside its UI path patterns): ${unserved.map((f) => f.path).join(', ')}`);
  console.error('  Put new files under static/ or ask root to extend the pattern list.');
  process.exit(1);
}

const manifest = {
  name: 'rbgrid-ui',
  version: `${pkg.version}+${gitShort()}`,
  // The documented field (site-docs 06-install-ui.md); `version` kept for older uploaders.
  ui_version: pkg.version,
  api_version: API_VERSION,
  built_at: new Date().toISOString(),
  files,
};
fs.writeFileSync(path.join(OUT, 'ui-manifest.json'), JSON.stringify(manifest, null, 2));

const total = files.reduce((s, f) => s + f.size, 0);
console.log(`\n✔ UI kit → ${path.relative(ROOT, KIT)} (${files.length} files, ${(total / 1024).toFixed(1)} KB on flash)`);
for (const f of files) console.log(`  ${String(f.size).padStart(8)}  ${f.encoding.padEnd(8)}  ${f.stored}`);
console.log(`  manifest: ${path.relative(ROOT, path.join(OUT, 'ui-manifest.json'))}  version ${manifest.version}, api_version ${API_VERSION}`);

// 5. the release package: rbgrid-ui_v<version>.zip = kit/ + ui-manifest.json
const zipName = `rbgrid-ui_v${pkg.version}.zip`;
writeZip(path.join(OUT, zipName), [
  ...files.map((f) => ({ name: `kit/${f.stored}`, data: fs.readFileSync(path.join(KIT, f.stored)) })),
  { name: 'ui-manifest.json', data: fs.readFileSync(path.join(OUT, 'ui-manifest.json')) },
]);
console.log(`  package:  ${path.relative(ROOT, path.join(OUT, zipName))} (${(fs.statSync(path.join(OUT, zipName)).size / 1024).toFixed(1)} KB)`);
