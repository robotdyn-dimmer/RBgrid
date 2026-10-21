# Changelog

## v1.1.0 — 2026-10-20

First source release of the RBgrid hub firmware under the **PolyForm Noncommercial 1.0.0** licence
(free for personal and non-commercial use; commercial use by licence — see `COMMERCIAL.md`).

### Measurement
- Measurement now comes from **rbAmp I²C metering modules** (true-RMS voltage, current, active power,
  power factor; reactive/apparent power where the module provides them). The hub collects, computes
  and stores; it has no analog front-end of its own. The native ESP32-ADC path of v1.0.x is removed.
- BASIC modules with 1 to 7 current channels and STD modules (including three-phase U3I3).
- **Single-phase and three-phase supplies**, validated on hardware.
- CT model per channel (SCT-013 5–100 A presets; factory-paired CT-series modules are never rewritten).
  A user's CT choice is bound to the module it was made on; a replaced module keeps its own settings.
- SCT-013 rating correction for BASIC modules (via the rbAmp ESP-IDF component).
- Per-channel sensor load flags: near the sensor limit (`ct_warn`) and clipping (`clip`).
- Any channel may be left unused; free channels are listed in `/api/v1/sensors` and can be bound later.
- 15-minute energy taken from the module's own period accounting; a hub restart inside a quarter keeps
  the energy the module counted meanwhile.

### Statistics, tariffs, export
- Flash history from 3 s up to yearly records; record format v4 with a non-destructive migration.
- Time-of-use tariffs: multiple zones, weekend/holiday rules, fees, taxes, VAT, net-metering with credit
  banking and progressive tiers; per-zone cost and revenue.
- `GET /api/v1/summary`, `GET /api/v1/export.csv`, `GET /api/v1/history` with time ranges.
- One build: every feature is in the published source (no separate closed edition).

### API and integrations
- REST + WebSocket API; telemetry frame **v2 is the default** (v1 on explicit request).
- `hw_rev` in every telemetry frame: changes when hardware or configuration changes.
- MQTT publish, command channel and Last Will; Home Assistant MQTT discovery.
  The hub has **no default MQTT broker** — set one in the app.
- Event log `GET /api/v1/system/events` (persists across reboots).

### Security and operation
- Admin token for write requests (and an optional read token); HTTPS server with a development
  certificate generated at build time (`components/rbpower/certs/gen_dev_cert.sh`).
- Hub access point `rbgrid-<mac4>`; the first-login password comes from the build's provisioning file.
  The user sets their own access-point password in the app (`POST /api/system/ap`); the hub never
  changes it by itself.
- Over-the-air updates with two slots and automatic rollback of an image that does not confirm itself.
- Configuration backup and restore.

### Platform
- ESP32-WROVER **with PSRAM** (required), 16 MB flash; ESP-IDF v5.4.1.
- A patched copy of the ESP-IDF v5.4.1 I²C master driver (`components/esp_driver_i2c`, Apache-2.0,
  see `NOTICE`) fixes watchdog resets and an ISR fault seen under I²C load.

### Build
- Product configuration in `sdkconfig.defaults`; a development console is available with
  `sdkconfig.dev.defaults`.
- The build needs the rbAmp ESP-IDF component; its public release follows this release.
