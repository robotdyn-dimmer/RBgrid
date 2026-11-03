
**Self-hosted ESP32 firmware for whole-home energy monitoring and electricity-cost tracking, built on rbAmp I²C measurement modules.**

![License: PolyForm Noncommercial 1.0.0](https://img.shields.io/badge/License-PolyForm_Noncommercial_1.0.0-blue.svg)
![Platform: ESP32-WROVER](https://img.shields.io/badge/platform-ESP32--WROVER-orange.svg)
![ESP-IDF 5.4.1](https://img.shields.io/badge/ESP--IDF-5.4.1-red.svg)

RBgrid turns an ESP32-WROVER hub plus one or more rbAmp modules into a self-hosted energy monitor: live per-circuit power, statistics and history, a full time-of-use tariff engine, and a web app served from the hub. No cloud required.

**[Try the live demo](https://demo.rbgrid.io/) · [Get started](https://www.rbgrid.io/get-started?utm_source=github&utm_medium=referral&utm_campaign=rbgrid) · [Documentation](https://www.rbgrid.io/webapp/docs?utm_source=github&utm_medium=referral&utm_campaign=rbgrid) · [Questions → Discussions](https://github.com/robotdyn-dimmer/RBgrid/discussions)**

## What it does

- **Real-time measurement per circuit**: voltage, current, active power and power factor (reactive/apparent where the module provides them), via rbAmp I²C modules
- **Up to 8 rbAmp modules per hub** (up to 56 current channels), with **history for up to 12 measurement points**
- **Import / export** with per-zone cost and export value
- **Time-of-use tariffs**: named zones on a weekly grid, weekend rates, per-kWh taxes and fees, VAT, and a per-zone sell price; the full tariff logic is in the source
- **3-second and 15-minute live data**, plus flash history (hourly … yearly)
- **Web app**, REST + WebSocket API, and MQTT with Home Assistant discovery
- **Grid:** single-phase and three-phase supplies. A three-phase supply is measured by one three-phase rbAmp module (STD-U3I3, one CT per phase) and, set as the three-phase input in the app, appears as one measurement point with per-phase readings. Split-phase (two live legs sharing a neutral) is planned for a future release.

## Hardware

- **Hub:** ESP32-WROVER **with PSRAM** and 16 MB flash (required).
- **Measurement:** rbAmp I²C modules, a separate product; see **[rbamp.com](https://www.rbamp.com/?utm_source=github&utm_medium=referral&utm_campaign=rbgrid)**. Keep the module cable up to 15 cm (recommended); with up to three modules, leave each module's pull-ups as they are, and for four or more see the [rbAmp wiring guide](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection).

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

The hub's own Wi-Fi stays on so you can always reach the hub. Once the web app is in, change its password in **Settings → Network → Hub access point**, and set an API token in **Settings → Security**, so nobody nearby can change your hub.

### 3. Install the web app

The web app is a separate package on the release page, [`rbgrid-ui_v1.1.2.zip`](https://github.com/robotdyn-dimmer/RBgrid/releases/download/v1.1.2/rbgrid-ui_v1.1.2.zip). Install it onto the hub over its API:

Unpack the zip (it holds `kit/` and `ui-manifest.json` side by side), then upload each file and commit:

```bash
HUB=http://<your-hub-address>     # e.g. http://192.168.4.1 on the hub's own Wi-Fi
cd rbgrid-ui_v1.1.2/kit
for f in $(find . -type f | sed 's|^\./||'); do
  curl -sf -X POST "$HUB/api/system/ui-upload?path=$f" \
       -H 'Content-Type: application/octet-stream' --data-binary "@$f" || { echo "upload failed: $f"; exit 1; }
done
curl -sf -X POST "$HUB/api/system/ui-commit" -H 'Content-Type: application/json' --data-binary @../ui-manifest.json
```

If you've set an admin token, add `-H "Authorization: Bearer <token>"` to both calls. A successful commit answers `{"status":"OK", …, "installed":true}`.

Then open the hub's address in a browser: the setup wizard takes you through the rest.

### 4. Measure

Add your rbAmp modules and bind their channels to measurement points in the app (or over the REST API). After adding points, restart the hub so they start keeping history. Optionally, set an MQTT broker for Home Assistant in **Settings → Home Assistant**.

After this, the hub is updated over the air: you upload a new firmware image from the web app.

## Build the web app yourself

The web app's source is in [`webapp/`](webapp/), at the same commit as the release package. A build from a clean copy gives the same files as the package you install above:

```bash
cd webapp
npm ci
npm run build:embedded
```

Details: [`webapp/README.md`](webapp/README.md) (build the kit, install it on a hub) and [`webapp/docs/`](webapp/docs/).

## Documentation

**[rbgrid.io/webapp/docs](https://www.rbgrid.io/webapp/docs?utm_source=github&utm_medium=referral&utm_campaign=rbgrid)**: tariffs, points, sensors, time, settings.

## License

RBgrid firmware is **source-available** under the **PolyForm Noncommercial 1.0.0** licence: free to use, modify and build for **personal and non-commercial** use. **Commercial use requires a licence**; contact **info@rbamp.com**. This is not an OSI "open source" licence; non-commercial use is free, commercial use is by permission. See [`LICENSE`](LICENSE), [`COMMERCIAL.md`](COMMERCIAL.md) and [`NOTICE`](NOTICE).

The web app (source in `webapp/` and the release package) is under the same licence. Third-party components and their licences are listed in [`webapp/NOTICE.md`](webapp/NOTICE.md) and in `THIRD_PARTY_LICENSES.md` on the release page.

The rbAmp measurement libraries are a separate product under the MIT licence; this licence does not cover them.
