# RBGrid design-sync notes

- **Package:** `design-system/` (`@rbgrid/design-system`). Build: `npm run build` (tsc → `dist/`, ESM + `.d.ts`).
- **Converter run** (from `design-system/`):
  `node .ds-sync/package-build.mjs --config design-sync.config.json --node-modules ../node_modules --entry ./dist/index.js --out ./ds-bundle`
  then `node .ds-sync/package-validate.mjs ./ds-bundle` and `node .ds-sync/package-capture.mjs --out ./ds-bundle`.
- `--node-modules ../node_modules` → the parent `rbgrid-app/node_modules` (react/@mui resolve there; the
  design-system package has no own node_modules besides its `.ds-sync/` dev deps).
- CSS-in-JS (MUI/emotion) → no static CSS; the converter writes a self-styling `styles.css`. The
  `[CSS_RUNTIME]` warnings are EXPECTED — don't chase them.
- `globalName`: `RBGridDS`. `projectId`: `6873c788-aef3-4da4-938e-138483397fbe` (RBGrid Design System).
- **Provider:** `cfg.provider = RBGridThemeProvider` wraps every preview in the HA-dark MUI theme, so
  previews render on-brand (not MUI default light). It is excluded from the visible component set via
  `cfg.componentSrcMap.RBGridThemeProvider = null`.
- **Groups** come from `@category` JSDoc tags on each component (Surfaces / Primitives / Dashboard /
  Settings / Layout) — all components live flat in `src/components/`, so the dir-derived group would be
  the generic "components"; the `@category` tag is what drives the picker grouping.
- **Wide components** (AppShell / TrendChart / SystemSummary) set `cfg.overrides.<Name>.viewport`
  (AppShell also `cardMode: single`, `primaryStory: Dashboard`) so their cards have room.

## Chromium for render-check / capture (WSL, no sudo)

The repo's `playwright` is installed but the cached Linux Chromium is missing 3 system libs
(`libnss3`, `libnspr4`, `libasound2`) and apt needs a password we don't have. Fixed **without sudo**:
`apt-get download libnss3 libnspr4 libasound2t64` (no root) → `dpkg-deb -x` each into
`.ds-sync/.syslibs/extracted/` → run validate/capture with
`LD_LIBRARY_PATH=$(cat .ds-sync/.syslibs/LIBPATH)`. The lib dir path is cached in
`.ds-sync/.syslibs/LIBPATH`. Re-extract if `.ds-sync/` is wiped. (Windows Chrome on CDP :9222 is NOT
reachable from WSL — Chrome rejects non-localhost Host headers — so the local Linux Chromium is the path.)

## Re-sync risks / TODO

- **`pgrep -f` self-match**: any waiter shell that runs `until ! pgrep -f "package-build.mjs"` matches
  its OWN command line and spins forever. Use the harness `run_in_background` + task notification, or a
  grep marker in a log file the waiter does NOT mention by script name. Cost a debugging cycle once.
- Previews live in `.design-sync/previews/<Name>.tsx` (committed, hand-authored, no marker). Re-syncs
  reuse them; verified-state carries via the uploaded `_ds_sync.json`. 33 cells, all graded good.
- Component set is the full **16** (5 surfaces/primitives + dashboard viz + settings forms + AppShell).
  To add more: extract a pure props component into `src/components`, add `@category`, export from
  `src/index.ts`, author `.design-sync/previews/<Name>.tsx`, rebuild — sync is incremental.
- `RBGridThemeProvider`/`rbgridTheme` are exported for the app too (`src/theme.ts`). If the app's real
  theme drifts from `tokens.ts`, update both — previews use this theme as the brand source of truth.
