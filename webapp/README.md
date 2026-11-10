# RBgrid web app

The web interface of **RBgrid**, the ESP32 power-monitoring hub ([rbgrid.io](https://rbgrid.io)). It shows
live and stored measurements from a hub and configures it: points, tariffs and costs, time, network,
Home Assistant / MQTT, security, modules, the setup wizard.

The hub serves this app itself: it is built into a **UI kit** (`rbgrid-ui_vX.Y.Z.zip`) that you install on
the hub once; then `http://<hub>/` opens the app with no internet and no external server. Each RBgrid
release attaches the kit built from this folder.

Stack: React 18 + TypeScript (Create React App / `react-scripts` 5), MUI 5 + Emotion, and the local
design-system package `design-system/` (`@rbgrid/design-system`).

## Build the kit

Requires Node.js 18+ and npm.

```bash
npm ci                      # installs the dependencies and builds design-system/
npm run build:embedded      # → build-embedded/rbgrid-ui_v<version>.zip = kit/ + ui-manifest.json
```

The zip holds `kit/` (the files the hub serves, pre-compressed) and `ui-manifest.json` beside it (each
file's SHA-256 and size, `ui_version`, `api_version`). Details: [docs/web-kit-format.md](../docs/web-kit-format.md).

## Install it on a hub

Upload every file of `kit/` and commit the manifest, with the hub's admin token — the steps (curl and
PowerShell) are in the release notes and in [docs/web-installing.md](../docs/web-installing.md).

## Develop

```bash
npm start                   # dev server (http://localhost:3000), runs on built-in demo data
CI=true npx react-scripts test --watchAll=false
npx tsc --noEmit -p .
```

To work against a real hub, create `.env.local`:

```bash
REACT_APP_USE_MOCK_DATA=false
REACT_APP_API_BASE_URL=http://<hub-ip>
```

More: [docs/web-building.md](../docs/web-building.md).

User guide (how to use the app): [docs/](../docs/README.md), the `web-` pages — also on [rbgrid.io/webapp/docs](https://rbgrid.io/webapp/docs).

## Layout

| Path | What |
|---|---|
| `src/core/` | Device layer: transports (HTTP/WebSocket, MQTT), config shadow, session, schema-driven capabilities |
| `src/pages/`, `src/app/` | Screens and the app shell |
| `src/components/` | Dashboard cards, energy-flow diagrams, settings panels, dialogs |
| `src/entities/`, `src/hooks/`, `src/services/` | Telemetry model, React hooks, API clients |
| `src/types/`, `src/constants/` | Types of the hub API, constants |
| `src/i18n/` | Phrase files (English, Russian); packs are built into `public/static/lang/` |
| `src/mock/` | Demo data for development and the public demo |
| `design-system/` | Tokens, MUI theme and presentational components |
| `scripts/` | Kit build, design-system build, language index, local helpers |

## Licence

PolyForm Noncommercial 1.0.0, as the rest of the RBgrid repository — see [LICENSE](LICENSE).
Third-party packages bundled into the kit keep their own licences: [NOTICE.md](NOTICE.md).

Changes: [CHANGELOG.md](CHANGELOG.md).
