# Changelog

## v1.1.6 — 2026-11-06

### Three-phase input metered per phase
- **History and economics of the whole input:** `GET /api/v1/history?unit=input&…` and
  `GET /api/v1/economics?unit=input` return the grid input as one series, the records of all input points
  (one per phase) summed by time. Same parameters and shapes as for a single point (`offset`/`limit`,
  `from`/`to`, `since`, `type=energy|economics|all`); the reply lists the points in `units`. With the input
  as one point it is the same as that point. `GET /api/v1/schema` → `capabilities.inputAggregate: true`.

### Fixed
- **Time settings lost after a restart.** The app saves only the time settings you changed, and the hub
  wrote them as the whole settings file. After changing the time zone in the app (web app 1.1.2–1.1.5),
  the next restart left the hub with no time zone and no NTP server, so the clock was never synced again.
  The hub now merges what it receives into the saved settings. A hub already in that state recovers at
  the first boot on v1.1.6: the saved time zone is kept and the NTP servers go back to `pool.ntp.org` /
  `time.nist.gov`. If the time zone was lost too, set it again in **Settings → Time**.
- A point created with the id of a deleted point inherited the deleted point's net-metering figures
  (period kWh and credit). Deleting a point now clears them.
- **Time settings out of range:** `POST /api/system/time-sync` (`save_config`) now refuses, with 400 and the
  allowed range, `resync_interval_sec` outside 60–86400, `sntp_timeout_ms` outside 1000–60000,
  `sync_retry_count` outside 1–10 and an empty or malformed `sntp_server`. Before, such a value was saved and
  the hub came up after the next restart with no clock sync at all. A value already saved out of range is
  brought into range at boot.
- **Clock set by hand:** after "sync from this browser" (or a POSTed time) the hub reported `tq: 0`,
  `time_src: "none"` until an NTP sync. It now reports `tq: 1` and `time_src: "manual"` (2 after 24 h, as
  for NTP), in the live frame and in `schema.time_quality`. `time_src` is `none`, `sntp` or `manual`.
- **Clock set by hand on a hub without NTP, in the app:** right after "sync from this browser" the hub
  tried NTP; when that failed, **Settings → Time** showed "not synchronized" with source `NONE`, and the
  setup wizard's last step waited for a synchronized clock. A hand-set clock now stays synchronized
  (source `MANUAL`) until NTP takes over.
- **Settings → Time failed to load now and then on a hub without NTP:** about one request in three
  answered 500 while the hub was retrying NTP. It now always answers.
- **`enable_dst`** in the time settings did nothing: summer time comes from the time-zone rule alone. The
  field is now read-only: it shows `true` when the hub's time-zone rule has summer time; a value written to
  it is ignored.
- **Update right after an update:** for about a minute after an over-the-air update the hub refuses the
  next one (the new firmware is being confirmed). It answered `500 ota_begin failed`; it now answers
  `409` with `error_code: "IMAGE_NOT_CONFIRMED"`, a plain message, `retry_after_s: 60` and a
  `Retry-After` header.

## v1.1.5 — 2026-11-06

### Tariff
- Fixed: **the saved tariff plan was lost at every restart** (power cut, reboot, firmware update). The plan
  was saved correctly, but at start-up the hub replaced it with its built-in placeholder plan
  "Test Day/Night". The saved plan now comes back complete after a restart: name, zones, weekend days,
  per-kWh charges and VAT.

### Three-phase input: one point, or one point per phase
- A three-phase module (STD-U3I3) can now be metered **as one point** (the phases are netted every
  15 minutes, like a meter that sums the phases) **or as one point per phase** (each phase has its own
  import and export, like a meter that counts each phase). Choose what matches your supply meter.
- Split: move the three-phase point to one phase (`POST /api/v1/units/<id>/rebind {"module":m,"ch":0}`).
  It keeps its name and history on that phase. The other phases become free sensors; add them as supply
  points (role `MAIN_SUPPLY`) and restart the hub. Group again: delete the phase points, move the
  remaining one back to the module's three-phase sensor (`"ch":254`) and restart.
- With several supply points the system totals (power, energy in/out, cost, export value) are the sum of
  all of them. The first one stays the main input. Deleting a phase point moves its totals to the main
  input, so the system totals don't go down.
- Before installing an older firmware on a hub whose input is metered per phase, switch it back to one
  point: older firmware handles only one supply point and would lose the totals.

### Generation points
- A point with role **Generation** now counts the flow in the direction of the sensor arrow (from the
  source to the consumers) as production: negative power, energy into **Energy Out**. Before, such a point
  needed `invert_sign`. `invert_sign` still flips the result on top.
- **After updating:** if you had set `invert_sign` on a Generation point to make it read production,
  clear it (`POST /api/v1/units/<id> {"invert_sign":false}`), or the point will read consumption again.

### Web app 1.1.5
- **Points → the STD-U3I3 card that carries the input:** choose "One point, phases netted" or "One point per
  phase"; the app does the steps above and offers the restart. With the input split, the live view shows
  the input as the sum of its phase points.
- Tariffs: every zone shows its sell price whatever the net-metering setting (export-only tariffs such as
  SEG); the toggle reads "Net metering (monthly kWh credit)".
- Time: southern-hemisphere zones with summer time (Sydney, Adelaide, Auckland, Santiago, …); "today" and
  hour axes start at the hub's real midnight on clock-change days.
- Car park: a three-phase charger bound as one point per conductor counts as one station when its points
  share a group.
- Full list: `webapp/CHANGELOG.md`.

### Known limitation
- With the input metered **per phase**, history and money **per period** in the app (Economics balance,
  Overview month and "vs yesterday", the energy chart) read the first phase point (L1) only. Live values
  and the hub's system totals (`system.*`, Home Assistant) are the sum of all phases. A summed history
  is planned for the next release.

## v1.1.4 — 2026-11-04

### Points
- Fixed: renaming a point could make it disappear after a restart on ESP32 chip revisions before v3.
  The point's identity was damaged when its name was saved; it then vanished at the next restart
  together with its history. Names are now saved safely.

### Measurement
- Mains voltage and frequency now come only from a module that actually sees mains. A module whose
  voltage inputs are not wired (for example an STD-U3I3 used for currents only) no longer overrides
  them; before, the frequency could read about 45 Hz.

### Adding modules
- A configured module that isn't answering and that no metering point depends on is now reported as an
  empty slot (`placeholder` in `GET /api/v1/i2c`). The app no longer stops the add-module steps on it:
  a new module from the box at 0x50 takes that slot.

### Web app 1.1.4
- Clearer texts (deleting a point, MQTT with no broker set, history) and the empty-slot step when
  adding a module. Full list: `webapp/CHANGELOG.md`.

## v1.1.3 — 2026-11-04

### Browser access (CORS)
- **Allow-list by default.** The hub now answers browser requests only from the hub's own pages and
  from listed websites. Before, it answered any website (`*`).
- **The RBgrid web app is always allowed:** `https://webapp.rbgrid.io` and `https://demo.rbgrid.io`
  are built in. They are not stored, don't use one of the 8 slots and can't be removed.
- Add your own origins (up to 8) in the app: Settings → Browser access (CORS). "Allow any website"
  brings back the old behaviour.
- A hub where you already chose "Allow any website" keeps that setting after the update.
- `GET /api/system/security` also returns `builtin_origins` and `origins_max`.
- Developing the app against a hub: add your dev origin (for example `http://localhost:5173`) to
  the list, or allow any website on a test hub.

### Web app 1.1.3
- The CORS panel shows the built-in origins as fixed rows and your own list separately.
- On an HTTPS page that can't reach the hub, the app suggests allowing access to devices on the local
  network.
- Economics: "Where the money goes" no longer shows energy as money; the standby share is a share of
  the month's consumption; the net-metering credit is hidden while net metering is off, and the
  progressive block shows only when the hub runs one.
- The setup wizard asks before a quick tariff replaces a fuller plan.
- Text fixes: hints name the real panels (Settings → Connection / Security).
- Full list: `webapp/CHANGELOG.md`.

## v1.1.2 — 2026-11-03

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

### Stability
- The web server task has a larger stack (12 KB, was 8 KB). With up to 8 modules, the app plus a settings save could
  overflow it. The hub then restarted, or in one case hung with no watchdog reset (.139, 2026-11-03). Measured
  headroom under the same load is now 4 KB (it was 144 bytes).
- A crash record (`FAIL boot: reset=PANIC`) now names the task that crashed.

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
