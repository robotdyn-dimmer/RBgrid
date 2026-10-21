# RBgrid

**Self-hosted ESP32 firmware for whole-home energy monitoring and electricity-cost tracking, built on rbAmp I²C measurement modules.**

![License: PolyForm Noncommercial 1.0.0](https://img.shields.io/badge/License-PolyForm_Noncommercial_1.0.0-blue.svg)
![Platform: ESP32-WROVER](https://img.shields.io/badge/platform-ESP32--WROVER-orange.svg)

RBgrid turns an ESP32-WROVER hub plus one or more rbAmp modules into a self-hosted energy monitor: real-time per-circuit power, statistics and history, a full time-of-use tariff/billing engine, and a web app — all served from the device, no cloud required.

## What it does

- **Real-time measurement per circuit** — voltage, current, active power and power factor (reactive/apparent where the module provides them) — via rbAmp I²C modules
- **Import / export (net-metering)** and per-zone cost and revenue
- **Time-of-use tariffs** — multi-zone, weekend/holiday, fees, VAT, progressive steps; the full billing logic is in the source
- **3-second and 15-minute live data**, plus flash history (hourly … yearly)
- **Web app**, REST + WebSocket API, and MQTT (with Home Assistant discovery)
- **Single-phase and three-phase supplies** (validated on hardware)


## Hardware

- **Hub:** ESP32-WROVER **with PSRAM** (required).
- **Measurement:** rbAmp I²C modules — a separate product, see **https://www.rbamp.com**. RBgrid has no built-in ADC; the modules measure, the hub collects, computes and stores.

## Build from source

Requires **ESP-IDF v5.4.1** and `openssl`.

> ⏳ **One dependency is publishing next:** a build from a fresh clone also needs the **rbAmp ESP-IDF component**, which is released separately — its publication **follows immediately after this release**. Until it lands, a clean clone won't resolve that dependency yet. (This note is removed once the component is public.)

```bash
git clone https://github.com/robotdyn-dimmer/RBgrid.git
cd RBgrid
components/rbpower/certs/gen_dev_cert.sh      # once, before the first build (generates the LAN HTTPS dev certificate)
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash                        # <PORT> e.g. /dev/ttyUSB0, COM3
```

After the first USB flash, updates are delivered over the air from the web app. On first boot the hub starts its own Wi-Fi `rbgrid-<mac4>`; open `192.168.4.1`, save your home Wi-Fi, then set your own access-point password (and, optionally, an MQTT broker) in the app.

Sensors and measurement points are **configured at runtime** from the web app or the REST API — not in code. See the documentation.

## Documentation

Full documentation: **https://www.rbgrid.io/rbgrid-doc-overview**

## License

RBgrid firmware is **source-available** under the **PolyForm Noncommercial 1.0.0** licence: free to use, modify and build for **personal and non-commercial** use. **Commercial use is by licence** — contact **info@rbamp.com**. This is not an OSI "open source" licence; non-commercial use is free, commercial use is by permission. See [`LICENSE`](LICENSE), [`COMMERCIAL.md`](COMMERCIAL.md) and [`NOTICE`](NOTICE).

The rbAmp measurement libraries are a separate product under the MIT licence; this licence does not cover them.
