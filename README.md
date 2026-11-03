# RBgrid

**Self-hosted ESP32 firmware for whole-home energy monitoring and electricity-cost tracking, built on rbAmp I²C measurement modules.**

![License: PolyForm Noncommercial 1.0.0](https://img.shields.io/badge/License-PolyForm_Noncommercial_1.0.0-blue.svg)
![Platform: ESP32-WROVER](https://img.shields.io/badge/platform-ESP32--WROVER-orange.svg)

RBgrid turns an ESP32-WROVER hub plus one or more rbAmp modules into a self-hosted energy monitor: live per-circuit power, statistics and history, a full time-of-use tariff engine, and a web app — all served from the device, no cloud required.

## What it does

- **Real-time measurement per circuit** — voltage, current, active power and power factor (reactive/apparent where the module provides them) — via rbAmp I²C modules
- **Import / export (net-metering)** with per-zone cost and export value
- **Time-of-use tariffs** — multi-zone, weekend rates, fees, VAT, progressive steps; the full tariff logic is in the source
- **3-second and 15-minute live data**, plus flash history (hourly … yearly)
- **Web app**, REST + WebSocket API, and MQTT (with Home Assistant discovery)
- **Grid:** single-phase and three-phase supplies. A three-phase supply is measured by one three-phase rbAmp module — one CT per phase — and appears as one measurement point with per-phase readings. Split-phase (two live legs sharing a neutral) is not covered.


## Hardware

- **Hub:** ESP32-WROVER **with PSRAM** (required).
- **Measurement:** rbAmp I²C modules — a separate product, see **[rbamp.com](https://www.rbamp.com/?utm_source=github&utm_medium=referral&utm_campaign=rbgrid)**. The ESP32 hub collects, computes and stores; the rbAmp modules are the supported measurement path.

## Build from source

Requires **ESP-IDF v5.4.1** and `openssl`.

> ⏳ **Build dependency:** the rbAmp ESP-IDF component is public at [github.com/rb-amp/rbamp-esp-idf](https://github.com/rb-amp/rbamp-esp-idf). In this release `main/idf_component.yml` still points at a local checkout of it. To build from a fresh clone, replace the `rbamp` entry's `path:` line with `git: https://github.com/rb-amp/rbamp-esp-idf.git` and `version: v1.7.2`. The next release does this for you, and this note goes away then.

```bash
git clone https://github.com/robotdyn-dimmer/RBgrid.git
cd RBgrid
components/rbpower/certs/gen_dev_cert.sh      # once, before the first build (generates the development certificate)
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash                        # <PORT> e.g. /dev/ttyUSB0, COM3
```

After the first USB flash, the hub can be updated over the air: you upload a new image from the web app. On first boot the hub starts its own Wi-Fi `rbgrid-<mac4>`; open `192.168.4.1` and save your home Wi-Fi, then (optionally) set an MQTT broker in the app. To use your own access-point password, set it at build time.

Sensors and measurement points are **configured at runtime** from the web app or the REST API — not in code. See the documentation.

## Documentation

Full documentation: **[rbgrid.io/webapp/docs](https://www.rbgrid.io/webapp/docs?utm_source=github&utm_medium=referral&utm_campaign=rbgrid)**

## License

RBgrid firmware is **source-available** under the **PolyForm Noncommercial 1.0.0** licence: free to use, modify and build for **personal and non-commercial** use. **Commercial use requires a licence** — contact **info@rbamp.com**. This is not an OSI "open source" licence; non-commercial use is free, commercial use is by permission. See [`LICENSE`](LICENSE), [`COMMERCIAL.md`](COMMERCIAL.md) and [`NOTICE`](NOTICE).

The rbAmp measurement libraries are a separate product under the MIT licence; this licence does not cover them.
