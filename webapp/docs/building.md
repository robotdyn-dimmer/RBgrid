# Building the web app

## Requirements

Node.js 18 or later with npm. No other tools; the build runs on Linux, macOS and Windows.

## From a clean clone

```bash
cd webapp
npm ci
npm run build:embedded
```

`npm ci` installs the exact versions from `package-lock.json` and builds the local design system
(`design-system/` → `design-system/dist`, from the `postinstall` script).

`npm run build:embedded` makes a production build with the hub profile and packs it:

- `build-embedded/kit/` — the files the hub serves; text files are gzip-compressed (`*.gz`);
- `build-embedded/ui-manifest.json` — the manifest the hub checks on install;
- `build-embedded/rbgrid-ui_v<version>.zip` — both of the above, the release artifact.

`<version>` is `version` in `package.json`. The manifest's `version` also carries the git commit
(`1.1.2+<sha>`), or `+nogit` outside a git checkout; the kit's files don't depend on it.

## Other builds

| Command | Output |
|---|---|
| `npm run build` | a plain production build in `build/` (for hosting the app elsewhere) |
| `npm run build:demo` | the demo-only build (built-in demo sites, no hub) in `build-demo/` |
| `npm run i18n:index` | rebuilds the language packs in `public/static/lang/` after a phrase change |

## Features hidden in a release build

`src/constants/release.ts` lists features whose code ships but whose screens a production build
hides. `REACT_APP_PREVIEW_FEATURES=true` shows them in any build.

Screens that need a newer hub firmware check the hub's version (`GET /api/system/version`) and stay
hidden on older hubs.

## Checks

```bash
CI=true npx react-scripts test --watchAll=false
npx tsc --noEmit -p .
```
