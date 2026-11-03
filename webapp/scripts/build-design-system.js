#!/usr/bin/env node
/**
 * Build the local design system (design-system/src -> design-system/dist) and refresh the copy
 * npm placed in node_modules/@rbgrid/design-system.
 *
 * Why: `@rbgrid/design-system` is a `file:` dependency and this repo installs with
 * install-links=true (npm copies the package, honouring its `"files": ["dist"]`). `dist/` is
 * gitignored, so on a fresh clone the copy has no build output and `npm start`/`build` fails.
 * Runs from the root `postinstall` and `prebuild` scripts (root devDeps provide `tsc`).
 */
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const src = path.join(root, 'design-system');
const srcDist = path.join(src, 'dist');
const installed = path.join(root, 'node_modules', '@rbgrid', 'design-system');

const tsc = require.resolve('typescript/bin/tsc', { paths: [root] });
execFileSync(process.execPath, [tsc, '-p', path.join(src, 'tsconfig.json')], { stdio: 'inherit' });

if (fs.existsSync(installed) && fs.realpathSync(installed) !== fs.realpathSync(src)) {
  const dest = path.join(installed, 'dist');
  fs.rmSync(dest, { recursive: true, force: true });
  fs.cpSync(srcDist, dest, { recursive: true });
  fs.copyFileSync(path.join(src, 'package.json'), path.join(installed, 'package.json'));
}
console.log('[design-system] built ->', path.relative(root, srcDist));
