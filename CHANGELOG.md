# Changelog

## v1.1.2 — unreleased (pending the .139 hardware run)

Built against the public rbAmp ESP-IDF component.

### Build
- The rbAmp component is fetched from its public repository
  (`https://github.com/rb-amp/rbamp-esp-idf`, tag `v1.7.2`). A fresh clone builds without a separate
  checkout of the component.

### Measurement (from the rbAmp component 1.7.2)
- SCT-013-060 and SCT-013-100 can be selected on rbAmp channels; the component refused them before.
- The per-model current limits of the SCT-013 presets follow the component's updated values.

### History
- Flash history is kept for up to 12 measurement points, as the internal stats partition allows. Up to
  v1.1.1 an internal table stopped at 8: points 9 to 12 showed live readings but kept no history.
- Each point in `/api/system/units-config` carries `history`: `false` means the point is live only (beyond the
  12 points with history, or bound since the last restart).

### Home Assistant
- The system entities "Revenue (Total)" and "Revenue (Day)" are renamed "Export value (Total)" and
  "Export value (Day)". Their unique IDs and the telemetry fields (`rev_total`, `rev_day`) do not change,
  so Home Assistant keeps the existing entities and their history.
- "Cost (Total)" and "Export value (Total)" are published with `state_class: total`. Before, they used
  `total_increasing`, which Home Assistant does not accept for money, so they could not be chosen in the
  Energy dashboard.

### Three-phase input
- An STD-U3I3 can be made the hub's input as one three-phase point:
  `POST /api/v1/units/<input>/rebind {"module": <m>, "ch": 254}`.
  - The reply carries `"requires_reboot": true`; the new layout applies after the hub restarts.
  - The point keeps its id, name and history.
  - Up to v1.1.1 this needed the USB console, and out of the box the input measured phase L1 only.
- Errors:
  - `409 SENSOR_BOUND`: another point uses a channel of the module.
  - `400 NOT_PHASE_MODULE`: the target is not the input, or the module is not a present three-phase module.
- An STD module is no longer reported as "uncalibrated" just because its sensor-class byte is unset, as it
  is on shipped STD-U3I3 modules. It counts as uncalibrated only when its own calibration status says so.

### Hub pages
- The WEB-APP link on the hub's own pages (Wi-Fi setup, home) opens the web app the hub serves itself. On your home
  Wi-Fi it shows the hub's address (`http://<hub>/`). On the hub's own access point it opens the app there. It is
  greyed out when no app is installed. It used to point to https://webapp.rbgrid.io, which a phone on the hub's
  access point can't reach, and which a browser blocks for an HTTP hub.

### Factory reset
- `POST /api/system/factory-reset {"history": "erase" | "keep"}` (admin token) returns the hub to its first
  start. History is erased by default.
  - The hub restarts and erases, early in the next boot: settings, points, tariff, module layout, restore
    point, Wi-Fi, tokens and the access-point password. With `"erase"` it also erases the history.
  - It then comes up as on first boot: its own access point and the Wi-Fi setup portal.
  - A power cut between the request and the restart cancels the reset.
- Up to v1.1.1 the app's "Factory reset" button only restored the site settings to their previous save.

### I²C bus
- One hub handles up to 8 rbAmp modules on its bus; up to v1.1.1 the firmware stopped at 3.
  Modules are added through `/api/v1/i2c` (scan, add, address change) one at a time.
- The hub reports a stalled bus: when the module poll makes no progress for 30 s, it logs a FAIL line and
  `/api/v1/sensors` carries `bus_stalled_s`.
  - A typical cause is a module wired to the bus without power.
  - The stalled bus doesn't recover by itself: check the connections, then restart the hub.
- The hub reports a bus line held low (`/api/v1/sensors` `bus_line_low_s`, a FAIL line). Typical causes: a module
  wired without power, set to another mode, or wired wrong. The scan refuses at once on a held line
  (`409 BUS_STUCK`) instead of walking every address. Readings resume by themselves once the line is released.
- The scan ignores addresses that only acknowledge with no data (0xFF). With an STD-U3I3 on the bus, the hub board's
  bus front end acknowledges addresses nobody owns.
- Scanning and adding a module identify it through the rbAmp library, so STD modules (U3I3, UI1) are found
  and added at any address. Before, the hub's own BASIC-only identity reads refused an STD module and could leave
  it holding the bus.
- After a module's address change, `/api/v1/sensors` shows the new address. The channels of a just-added module
  no longer read as absent before the first point is bound to them.

## v1.1.1 — 2026-10-22

Fixes for a hub installed from scratch (USB flash on an erased chip). Hubs updated over the air from
v1.1.0 keep their configuration; a hub that already lost its default units gets them back on update.

### First start
- The default measurement points (main input and voltage bus) are kept across restarts. In v1.1.0 a hub
  whose configuration was first saved by v1.1.0 lost both after its first restart; this version
  restores them on update.
- The network scan on the setup page (`192.168.4.1`) lists the networks. While no network was saved,
  the hub kept trying to join one and the scan could not start; the page also did not wait for the
  scan result.

### Data quality
- BASIC modules: a reading near zero with no line voltage is no longer reported as reverse flow, and
  the hour is no longer marked as having lost reverse-flow energy.
- A configuration file larger than the buffer it is read into is reported as too large, not as
  corrupted, and is not replaced by its backup copy.

### Log messages
- No false "CRC MISMATCH" for `/config.json` at every start.
- No "task not found" watchdog errors while history is read over HTTP.

### Bench kit (development builds only)
- Load relays switch off after 60 s without a command, and after 4 h of continuous load in any case.

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
- Live readings every 3 s; flash history from hourly up to yearly records; record format v4 with a non-destructive migration.
- Time-of-use tariffs: multiple zones, weekend/holiday rules, fees, taxes, VAT, net-metering with credit
  banking and progressive tiers; per-zone cost and export value.
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
