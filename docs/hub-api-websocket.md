# Live data without polling

> ⚠️ **This page is about the standalone hub firmware.** A hub running the ESPHome package has **no REST API,
> no WebSocket and no MQTT** — it is an ESPHome device, and Home Assistant talks to it through the RBgrid
> panel and the `rbgrid_cmd` action instead. Everything below assumes the hub serves its own web app.
> → [Run it inside Home Assistant](https://www.rbgrid.io/integrations/esphome)

The hub pushes. Open one WebSocket to `/ws-telemetry` and the readings arrive as the hub produces them —
instead of you asking for them repeatedly and getting a copy that is always slightly out of date.

This is what the app itself uses. If you are building a dashboard, a Node-RED flow or anything that wants a
live number on a screen, this is the transport; for anything that wants a *record* rather than a *value*, use
[history over HTTP](hub-api-rest.md).

**The route list and the live-field reference are in [the generated API reference](https://github.com/rb-amp/RBgrid/blob/master/docs/hub-api-schema.md)**, a file in the
repository, produced from the hub's own schema at every release.
This page does not repeat them: a hand-copied field list goes stale quietly, and a stale field list is worse
than none, because it is believable.

---

## Getting in, from a browser, without putting a token in the URL

A WebSocket address carries no headers, so the obvious move is to put the token in the query string — and that
is how tokens end up in browser history, server logs and screenshots.

The hub gives you the alternative:

1. `POST /api/v1/ws-ticket` over HTTP, with your token.
2. You get a **ticket: single use, valid for 30 seconds**.
3. Open the socket with the ticket.

Thirty seconds is deliberately short, and single-use means a captured ticket is already spent. Fetch one per
connection — including each reconnection — rather than holding one.

> ⚠️ **A ticket is not a session.** It authorises the opening of one socket. If you cache it, your reconnect
> will fail at the worst moment, which is exactly when you reconnect: after something went wrong.

For a server-side client that is not a browser, this dance is unnecessary — send the token the ordinary way.

Reads need the **read** token once the hub has both tokens set. → [Settings](web-settings.md#security)

---

## Starting and stopping the stream

The socket is not a firehose you merely attach to. It takes three messages:

| Message | Does |
|---|---|
| `get_telemetry` | one frame, now |
| `start_telemetry` | begin pushing |
| `stop_telemetry` | stop pushing, keep the socket |

**Stop when your page is hidden and start again when it is shown.** A browser tab left open all night with a
running stream costs the hub real work for nobody's benefit, and the hub is also serving the app, the broker
connection and the measurement loop.

**One socket per client.** Several sockets from one page is a bug that looks like a performance problem: each
one is a stream the hub must fill.

---

## How much the hub sends

Telemetry has two profiles, **lean** and **full**, set on the hub (`/api/system/telemetry-config`). Lean is
the smaller frame; full carries everything the hub can report. The frame also has a size ceiling the hub will
not exceed.

Pick lean unless you are using fields that only full carries. A dashboard showing power and cost per point
does not need full, and the difference is bytes over your network every few seconds, for as long as the client
runs.

---

## Reading what arrives

Three things to do, and the first two are the ones integrations skip:

**Carry the quality through.** Live frames and records alike say what the hub thinks of its own numbers:
partial, estimated, recomputed, from an outage. A dashboard that drops those bits is a dashboard that cannot
explain a strange figure later, and the explanation is usually right there in the bit.
→ [History](web-history.md)

**Trust the hub's time, not yours.** Periods, days and tariff zones are placed by the hub's own clock and time
zone. A client that re-timestamps on arrival will disagree with the hub about which hour a reading belongs to
— and the disagreement is invisible until a day boundary. → [Time](web-time.md)

**Treat a missing field as missing, not as zero.** A point without a voltage module reports no energy at all
rather than an energy computed from a nominal voltage; the hub refuses to invent it, and so should you.

---

## Reconnecting

The socket will close: the hub restarts for an update, Wi-Fi drops, a router reboots. Plan for it rather than
being surprised:

- **Fetch a fresh ticket per attempt.** The old one is spent or expired.
- **Back off between attempts, and keep backing off.** A client retrying in a tight loop against a hub that is
  mid-restart arrives exactly when it is least able to answer.
- **Re-send `start_telemetry`.** A new socket is not streaming until you ask it to.
- **Assume you missed things.** The stream is live values, not a queue; nothing is replayed. If what you missed
  matters, it is in [history](hub-api-rest.md), which is kept, and the
  [price log](web-prices.md), which is kept for 40 days.

---

## When to use something else

| You want | Use |
|---|---|
| a live number on a screen | this |
| Home Assistant, or anything already on a broker | [MQTT](hub-mqtt.md) — discovery included |
| "is energy cheap right now" | the retained price notification over MQTT: a state, not a stream → [Knowing when energy is cheapest](hub-price-notification.md) |
| closed periods, costs, a month of data | [HTTP](hub-api-rest.md) or `export.csv` |

**For the price notification specifically, do not build it on this socket.** It is a state the hub republishes
at every 15-minute boundary and whenever anything changes, and over MQTT it is retained — so a client that
connects late still gets the current state. A WebSocket client that was offline during a transition simply
missed it.

---

## When it does not work: what to check

**The socket opens and immediately closes.**
The ticket was already used or older than 30 seconds. Fetch one per connection.

**The socket stays open and nothing arrives.**
No `start_telemetry` was sent. A fresh socket is silent until asked.

**It worked, then stopped after a hub update.**
The hub restarted and the socket went with it. Reconnect with a new ticket and start the stream again.

**Values arrive but money does not.**
No tariff is set, or the hub is still on the factory example plan. → [Tariffs](web-tariffs.md)

**A point sends current but never energy.**
It has no voltage behind it. The hub shows live power as an estimate from the nominal voltage and books no
energy, by design. → [Remote nodes](web-nodes.md)

**My hour totals disagree with the app's by one hour.**
You are stamping readings with your own clock. The hub decides which hour a reading is in.
→ [Time](web-time.md)

**A browser client is refused while a script on the same machine works.**
The browser is enforcing origin rules the script does not have. Allow your origin on the hub.
→ [Talking to the hub over HTTP](hub-api-rest.md)

> ⚠️ For any work inside an electrical panel, call a qualified electrician.
