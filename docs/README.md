# RBgrid documentation

Everything about RBgrid in one place: the web app you look at, the hub you flash, and the remote nodes. Current for hub firmware **v1.2.1** and web app 1.2.0 — what changed in each release is in [CHANGELOG.md](../CHANGELOG.md).

The same pages, with search, are on [rbgrid.io](https://www.rbgrid.io/webapp/docs).

> **Which RBgrid these pages describe.** There are two ways to run RBgrid, and this set documents the first:
> the **standalone hub firmware**, which serves its own web app and speaks MQTT, REST and WebSocket.
>
> The other way is the **ESPHome package**, where the hub is an ESPHome device and Home Assistant is the
> interface. It runs the same metering core and measures identically — points, history, tariffs, dynamic
> prices and remote nodes all work the same — but it has **no web app, no MQTT, no REST and no WebSocket**,
> and it is set up from the RBgrid panel in Home Assistant rather than from the app these pages show you.
> If that is the one you have, start at
> [rbgrid.io/integrations/esphome](https://www.rbgrid.io/integrations/esphome) and at the package's own
> documentation; the pages here on points, history, prices and nodes still describe what your hub does, and
> only the screens and the APIs are someone else's.

## Start here

The seven questions people actually arrive with:

| Question | Page |
|---|---|
| Why does this hour cost what it costs? | [How cost is calculated](web-cost-calculation.md) |
| When is electricity cheapest today? | [Where your price comes from](hub-dynamic-prices.md) · [Dynamic prices](web-prices.md) |
| How do I add a module and bind a point? | [Points](web-points.md) · [Sensors and modules](web-sensors.md) |
| My node is offline — will the radio reach? | [Remote nodes](web-nodes.md) |
| How do I get this into Home Assistant? | [MQTT broker](hub-mqtt.md) |
| How do I flash the board? | [Flashing the hub](hub-flashing.md) |
| Which modules do I need? | [rbgrid.io/hardware](https://www.rbgrid.io/hardware) |

New hub, nothing set up yet? → [Quick start](web-quick-start.md), then [the setup wizard](web-setup-wizard.md).

## The web app

**Getting in**
- [Quick start](web-quick-start.md) — from a blank board to a live cost on screen.
- [One main meter, step by step](web-scenario-main-meter.md) — the same setup, explained.
- [Opening the app](web-opening-the-app.md) — from the hub, or at webapp.rbgrid.io.
- [First setup: the wizard](web-setup-wizard.md) — seven steps.
- [Your first measurements](web-first-measurements.md) — what the first numbers mean.

**Using it**
- [Overview](web-dashboard.md) — cost now, today, month to date, where the money goes.
- [History](web-history.md) — this hour to this year, and the quality of each record.
- [Economics](web-economics.md) — cost, export value, net, input against metered lines.
- [How cost is calculated](web-cost-calculation.md) — the chain from measured energy to money.
- [Dynamic prices](web-prices.md) — the Prices screen: the price now, today and tomorrow, windows, peaks, the log.
- [Where your price comes from](hub-dynamic-prices.md) — the provider, the backup, your plan as the fallback.
- [Knowing when energy is cheapest](hub-price-notification.md) — cheap windows, and what the hub publishes for automations.
- [The highest 15 minutes of the month](hub-peaks.md) — import peaks per billing cycle.
- [Events](web-events.md) — what the hub reports, and what to check.

**Measuring**
- [Sensors and units: the idea](web-sensors-units.md) — what a sensor is and what a point is.
- [Sensors and modules](web-sensors.md) — modules, channels, three-phase input.
- [Points](web-points.md) — bind a channel to a named circuit; points inside points.
- [Remote nodes](web-nodes.md) — measure elsewhere in the building over ESP-NOW.

**Settings**
- [Settings](web-settings.md) — the whole settings screen, card by card.
- [Tariffs](web-tariffs.md) — zones, charges, VAT, export prices.
- [Time](web-time.md) — time zone, NTP, the day boundary.

**Questions**
- [FAQ](web-faq.md)

## The hub

- [Flashing the hub](hub-flashing.md) — USB install for ESP32-WROVER and ESP32-S3, over-the-air updates, the partition layout.
- [MQTT broker](hub-mqtt.md) — topics, Home Assistant discovery, commands, TLS.
- [API reference](hub-api-schema.md) — every public route, generated from the hub's own `GET /api/v1/schema`.

## Building the app yourself

- [Building](web-building.md) · [Installing the kit on a hub](web-installing.md) · [Kit format](web-kit-format.md)

## Licence

The hub and node firmware and the web app are **source-available under PolyForm Noncommercial 1.0.0** — free for personal, non-commercial use; commercial use requires a licence (**info@rbamp.com**). The rbAmp measurement libraries are a separate product under the MIT licence. See [LICENSE](../LICENSE) and [COMMERCIAL.md](../COMMERCIAL.md).
