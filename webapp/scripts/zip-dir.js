#!/usr/bin/env node
/** zip-dir <dir> <out.zip> — pack a build folder (paths relative to it) with scripts/zip.js. */
const fs = require('fs');
const path = require('path');
const { writeZip } = require('./zip');

const [dir, out] = process.argv.slice(2);
if (!dir || !out) { console.error('usage: zip-dir <dir> <out.zip>'); process.exit(2); }
const walk = (d) => fs.readdirSync(d, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? walk(path.join(d, e.name)) : [path.join(d, e.name)]));
const files = walk(dir).filter((f) => !f.endsWith('.map')).sort();
fs.mkdirSync(path.dirname(out), { recursive: true });
writeZip(out, files.map((f) => ({ name: path.relative(dir, f).split(path.sep).join('/'), data: fs.readFileSync(f) })));
console.log(`✔ ${out} (${files.length} files, ${(fs.statSync(out).size / 1024).toFixed(1)} KB)`);
