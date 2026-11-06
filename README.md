# RBgrid

**See your electricity as money. Every circuit in your home, costed by your own tariff, on a hub you own.**

[![Release](https://img.shields.io/github/v/release/robotdyn-dimmer/RBgrid)](https://github.com/robotdyn-dimmer/RBgrid/releases/latest)
![License: PolyForm Noncommercial 1.0.0](https://img.shields.io/badge/License-PolyForm_Noncommercial_1.0.0-blue.svg)
![Platform: ESP32-WROVER](https://img.shields.io/badge/platform-ESP32--WROVER-orange.svg)
![ESP-IDF 5.4.1](https://img.shields.io/badge/ESP--IDF-5.4.1-red.svg)

RBgrid is self-hosted firmware for an ESP32-WROVER hub that reads [rbAmp](https://www.rbamp.com/?utm_source=github&utm_medium=referral&utm_campaign=rbgrid) measurement modules and shows what each circuit costs right now, today and this month, on your own tariff zones. The history stays on the hub in your fuse board. No account, no cloud, no subscription, no ads. Free for personal, non-commercial use; source-available.

**[Try the live demo](https://demo.rbgrid.io/) · [Get started](https://www.rbgrid.io/get-started?utm_source=github&utm_medium=referral&utm_campaign=rbgrid) · [Which modules do I need?](https://www.rbgrid.io/hardware?utm_source=github&utm_medium=referral&utm_campaign=rbgrid) · [Documentation](https://www.rbgrid.io/webapp/docs?utm_source=github&utm_medium=referral&utm_campaign=rbgrid) · [Questions → Discussions](https://github.com/robotdyn-dimmer/RBgrid/discussions)**

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://www.rbgrid.io/assets/img/shots/shot-overview-flat-dark-1600.webp">
  <img alt="RBgrid Overview: live grid import in €/h, today and month to date, the active tariff zone, where today's money goes and the standby load." src="https://www.rbgrid.io/assets/img/shots/shot-overview-flat-light-1600.webp">
</picture>

## Why RBgrid

- **Your real tariff, not an average price.** Time-of-use zones, weekend rates, per-kWh taxes and fees, VAT and a sell price for export are all entered separately and all included in the cost. The tariff engine runs on the device, and its code is in this repository.
- **Every circuit, not just the whole house.** Up to 8 rbAmp modules on one hub (up to 56 current channels), with history kept for up to 12 measurement points.
- **On your own hardware.** The hub keeps measuring and storing with the internet down. Nothing goes through our servers.
- **Works with Home Assistant, doesn't need it.** No server to run. If you do run Home Assistant, the hub plugs into it over MQTT.

## What the app shows you

Open the web app two ways: from the hub itself, at its address on your home network, or at [webapp.rbgrid.io](https://webapp.rbgrid.io/), which connects to your hub on your home network (checked in Chrome and Edge; the browser asks once to allow access to local devices). Either way, your data goes straight from the browser to the hub. Every screen shows energy and money side by side.

- **Overview:** cost right now per line and for the whole house; today and month to date, with yesterday for comparison; which tariff zone you are in and when the cheaper one starts; where today's money goes; the standby load.
- **History:** from this hour to this year; energy, power, voltage, reactive power, power factor and money; chart or table; hourly for 60 days, then daily, monthly and yearly, on the hub's flash; CSV download.
- **Economics:** cost per period and per tariff zone; import and export measured separately, export valued at your own sell rate; input against metered lines, so you see how much is unmetered.
- **Tariffs:** your plan as your supplier writes it, on a weekly grid of named zones; history keeps the rates that applied.
- **Points and events:** bind any module channel to a named point at runtime; sensor offline, clamp near its limit or clipping, with what to check.
- **Setup and upkeep:** a seven-step setup wizard; over-the-air updates with automatic rollback; a restore point on the hub; API tokens; light and dark themes; English and Russian.

| | |
|---|---|
| <picture><source media="(prefers-color-scheme: dark)" srcset="https://www.rbgrid.io/assets/img/shots/shot-economics-dark-1600.webp"><img alt="RBgrid Economics: this month's cost split by tariff zone, and the unmetered share." src="https://www.rbgrid.io/assets/img/shots/shot-economics-light-1600.webp"></picture> | <picture><source media="(prefers-color-scheme: dark)" srcset="https://www.rbgrid.io/assets/img/shots/shot-tariffs-dark-1600.webp"><img alt="RBgrid Tariffs: the weekly zone grid of a two-zone Day/Night plan." src="https://www.rbgrid.io/assets/img/shots/shot-tariffs-light-1600.webp"></picture> |
| **Economics:** cost by tariff zone | **Tariffs:** zones on a weekly grid |
| <picture><source media="(prefers-color-scheme: dark)" srcset="https://www.rbgrid.io/assets/img/shots/shot-history-dark-1600.webp"><img alt="RBgrid History, seven days in money: daily bars kept on the hub's flash." src="https://www.rbgrid.io/assets/img/shots/shot-history-light-1600.webp"></picture> | <picture><source media="(prefers-color-scheme: dark)" srcset="https://www.rbgrid.io/assets/img/shots/shot-points-dark-1600.webp"><img alt="RBgrid Points: measured points across two modules, each bound to a named circuit." src="https://www.rbgrid.io/assets/img/shots/shot-points-light-1600.webp"></picture> |
| **History:** this hour to this year | **Points:** every channel bound to a named circuit |

Try it with sample data, no hardware needed: the [live demo](https://demo.rbgrid.io/) switches between a flat, a house with a heat pump and an EV, a house that exports, a workshop and a car park.

## Home Assistant

The hub announces every measurement point over MQTT discovery, and Home Assistant creates the sensors itself. Each point appears as a device with power, current, reactive power, power factor, and energy in and out. The hub adds net power, frequency and a bus-voltage device. The energy sensors go straight into the Energy dashboard: no templates, no `utility_meter`. Tariff editing, long-term history and sensor setup stay in the RBgrid app. Details: [Integrations](https://www.rbgrid.io/integrations?utm_source=github&utm_medium=referral&utm_campaign=rbgrid).

## How it fits together

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://www.rbgrid.io/assets/img/illus/diagram-architecture-dark.png">
  <img alt="Data flow: rbAmp modules send measurements over I²C to an ESP32-WROVER hub, which computes points, statistics, tariff costs and history, and serves the web app, the REST/WebSocket API and MQTT." src="https://www.rbgrid.io/assets/img/illus/diagram-architecture-light.png" width="720">
</picture>

rbAmp modules clip onto your existing wiring and measure each circuit. The ESP32-WROVER hub reads them over I²C, applies your tariff, keeps the history and serves the app, all on your home network.

## Hardware

| | |
|---|---|
| **Hub** | An ESP32-WROVER board with PSRAM and 16 MB flash, one you have or any you buy. You flash it once over USB. |
| **Measurement** | One or more [rbAmp](https://www.rbamp.com/?utm_source=github&utm_medium=referral&utm_campaign=rbgrid) modules, up to eight per hub: **Basic** modules for your own SCT-013 clamps (one to seven channels), or **STD** energy meters for the supply (STD-UI1 single-phase, STD-U3I3 three-phase, with calibrated sensors included). |
| **Grid** | Single-phase and three-phase supplies. A three-phase supply is measured by one STD-U3I3, set as the three-phase input in the app: one measurement point with per-phase readings. Split-phase (two live legs sharing a neutral) is planned for a future release. |
| **Wiring** | I²C, four wires. Keep the module cable up to 15 cm (recommended). Pull-ups and longer cables: see the [rbAmp wiring guide](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection). |

| | |
|---|---|
| <img alt="An rbAmp Basic measurement module board, top view." src="https://www.rbgrid.io/assets/img/photos/module-basic-sct-ui5.webp" height="160"> | <img alt="An rbAmp STD-U3I3 three-phase meter module on a DIN rail, with its three calibrated current sensors." src="https://www.rbgrid.io/assets/img/photos/module-std-u3i3-din.webp" width="360"> |

RBgrid itself is free for personal, non-commercial use; the hardware is the only cost. Module prices and sets: [rbgrid.io/hardware](https://www.rbgrid.io/hardware?utm_source=github&utm_medium=referral&utm_campaign=rbgrid).

> For any work inside an electrical panel, call a qualified electrician. The rbAmp modules clip onto existing wiring.

## Get it running

### 1. Flash the firmware

**Option A: the release image.** Download `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` and `main_release.bin` from the [latest release](https://github.com/robotdyn-dimmer/RBgrid/releases/latest) (their SHA-256 are in the release notes) and flash them over USB:

```bash
python -m esptool --chip esp32 -p <PORT> -b 460800 write_flash --flash_mode dio --flash_size 16MB --flash_freq 40m \
  0x1000 bootloader.bin 0x10000 partition-table.bin 0x17000 ota_data_initial.bin 0x20000 main_release.bin
```

To keep the hub's settings and history, flash as above; to start clean, run `python -m esptool --chip esp32 -p <PORT> erase_flash` first.

A hub already on v1.1.x can simply update over the air from the app (Settings → Hub firmware).

**Option B: build from source.** Requires **ESP-IDF v5.4.1** and `openssl`:

```bash
git clone https://github.com/robotdyn-dimmer/RBgrid.git
cd RBgrid
components/rbpower/certs/gen_dev_cert.sh      # once, before the first build (generates the development certificate)
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash                        # <PORT> e.g. /dev/ttyUSB0, COM3
```

The rbAmp ESP-IDF component (`rb-amp/rbamp-esp-idf`) is fetched automatically.

### 2. Join the hub

On first boot the hub starts its own Wi-Fi `rbgrid-<mac4>`. Join it, open `192.168.4.1`, and save your home Wi-Fi.

The hub's own Wi-Fi stays on so you can always reach the hub. Once the web app is in, change its password in **Settings → Hub access point**, and set an API token in **Settings → Security**, so nobody nearby can change your hub.

### 3. Install the web app

No install needed if you use [webapp.rbgrid.io](https://webapp.rbgrid.io/): open it on a computer on the same network as the hub and connect to the hub's address. To have the app on the hub itself, install it once. It's a separate package on the release page, `rbgrid-ui_vX.Y.Z.zip` ([latest release](https://github.com/robotdyn-dimmer/RBgrid/releases/latest)). Unpack it (it holds `kit/` and `ui-manifest.json` side by side), then upload each file and commit:

```bash
HUB=http://<your-hub-address>     # e.g. http://192.168.4.1 on the hub's own Wi-Fi
cd kit                              # in the folder where you unpacked the zip
for f in $(find . -type f | sed 's|^\./||'); do
  curl -sf -X POST "$HUB/api/system/ui-upload?path=$f" \
       -H 'Content-Type: application/octet-stream' --data-binary "@$f" || { echo "upload failed: $f"; exit 1; }
done
curl -sf -X POST "$HUB/api/system/ui-commit" -H 'Content-Type: application/json' --data-binary @../ui-manifest.json
```

If you've set an admin token, add `-H "Authorization: Bearer <token>"` to both calls. A successful commit answers `{"status":"OK", …, "installed":true}`.

Then open the hub's address in a browser: the setup wizard takes you through the rest.

### 4. Measure

Add your rbAmp modules in the app (**Points → Add a module**) and bind their channels to measurement points (or do it over the REST API). After adding points, restart the hub so they start keeping history. Optionally, set an MQTT broker for Home Assistant in **Settings → Home Assistant**.

After this, the hub is updated over the air: you upload a new firmware image from the web app.

## Build the web app yourself

The web app's source is in [`webapp/`](webapp/), at the same commit as the release package. A build from a clean copy gives the same files as the package you install above:

```bash
cd webapp
npm ci
npm run build:embedded
```

Details: [`webapp/README.md`](webapp/README.md) (build the kit, install it on a hub) and [`webapp/docs/`](webapp/docs/).

## For developers

- **REST + WebSocket API and MQTT**, with a self-describing schema at `GET /api/v1/schema`. Sensors and measurement points are configured at runtime from the app or the API, not in code.
- The hub firmware is an ESP-IDF application, not a library: you build and flash the whole image.
- **Browser access** is limited to an allow-list: the hub's own pages and the RBgrid app (`https://webapp.rbgrid.io`, `https://demo.rbgrid.io`) are always allowed. To run the web app from your own address (for example `http://localhost:5173`), add it in **Settings → Browser access (CORS)** (up to 8 sites).

## Documentation

**[rbgrid.io/webapp/docs](https://www.rbgrid.io/webapp/docs?utm_source=github&utm_medium=referral&utm_campaign=rbgrid)**: tariffs, points, sensors, time, settings. Building and flashing: [rbgrid.io/source/build](https://www.rbgrid.io/source/build?utm_source=github&utm_medium=referral&utm_campaign=rbgrid).

The same user guide is in this repository: [`webapp/docs/user/`](webapp/docs/user/README.md).

## License

RBgrid firmware is **source-available** under the **PolyForm Noncommercial 1.0.0** licence: free to use, modify and build for **personal and non-commercial** use. **Commercial use requires a licence**; contact **info@rbamp.com**. This is not an OSI "open source" licence; non-commercial use is free, commercial use is by permission. See [`LICENSE`](LICENSE), [`COMMERCIAL.md`](COMMERCIAL.md) and [`NOTICE`](NOTICE).

The web app (source in `webapp/` and the release package) is under the same licence. Third-party components and their licences are listed in [`webapp/NOTICE.md`](webapp/NOTICE.md) and in `THIRD_PARTY_LICENSES.md` on the release page.

The rbAmp measurement libraries are a separate product under the MIT licence; this licence does not cover them.

RBgrid is developed by the rbAmp team; module sales fund the work. The firmware carries no ads and no telemetry to us.
