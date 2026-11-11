# Talking to the hub over HTTP

> ⚠️ **This page is about the standalone hub firmware.** A hub running the ESPHome package has **no REST API,
> no WebSocket and no MQTT** — it is an ESPHome device, and Home Assistant talks to it through the RBgrid
> panel and the `rbgrid_cmd` action instead. Everything below assumes the hub serves its own web app.
> → [Run it inside Home Assistant](https://www.rbgrid.io/integrations/esphome)

Everything the app does, it does over HTTP against the hub on your own network. There is no cloud in the path
and no account: if you can reach the hub, you can read it, and with its token you can change it.

This page is the part a route table cannot tell you — which token, how often to ask, and the handful of
mistakes that look like hub faults and are not. **The routes themselves are in [the generated API reference](https://github.com/rb-amp/RBgrid/blob/master/docs/hub-api-schema.md)**, a file in the repository that the hub
produces from its own schema and that is regenerated at every release. Nothing here repeats that list on purpose: a hand-copied route table is wrong one release later, and
it is wrong silently.

---

## The two tokens

| Token | What it is for |
|---|---|
| **admin** | changing the hub. Once set, every write needs it — and the hub says `Admin token required.` when it is missing |
| **read** | reading the hub, when you have set one |

Set them in the app, **Settings → Security**. Until an admin token is set, anyone who can reach the hub can
change it — which is why the app keeps a warning on screen until both that and the access-point password are
done. → [Settings](web-settings.md#security)

Send a token as a bearer token on the request. The generated reference marks every route that needs the admin token for writes.

> ⚠️ **What the generated reference does not tell you** is which *reads* need the read token — it says so
> itself, in its own header. The rule the app follows is: when the hub has both tokens set, reads need the read
> token too. If you are building something that only reads, set a read token and send it; a 401 on a route you
> expected to be open almost always means that.

For the five price routes the split is stated: **GET takes the read token, POST takes the admin token.** So a
dashboard that only reads prices and the applied-price log never needs the admin one — and should not be given
it.

### A browser client should not carry a token in a URL

For a page that opens a WebSocket, putting a token in the address is how tokens end up in logs, history and
screenshots. The hub has a way out: `POST /api/v1/ws-ticket` returns a **single-use ticket, valid for 30
seconds**, which the socket presents instead. → [Live data over WebSocket](hub-api-websocket.md)

### Letting a browser on another origin call the hub

A page served from somewhere else is blocked by the browser, not by the hub. `/api/system/security` is where
you allow an origin — up to **eight** of them. That is a deliberate door, so open it for the origins you run
and no others.

---

## What to poll, and what not to

**Do not poll for live values.** The hub pushes them: over
[WebSocket](hub-api-websocket.md) and over [MQTT](hub-mqtt.md), at the hub's own cadence. Polling
`/api/v1/summary` in a loop gets you the same numbers later, costs the hub work it could spend measuring, and
gives you a sampling interval that drifts against the hub's.

**Poll for things that change slowly**, and match the rate to how fast they actually change:

| What | Changes | Ask |
|---|---|---|
| history records | when a period closes | once per period at most; by `from`/`to`, not by re-reading the day |
| economics | with the records behind it | the same |
| the price state | when prices arrive or a state changes | the price notification is a state you can subscribe to; see below |
| configuration | when you change it | after you change it |

> ⚠️ **A loop with no backoff is the one pattern that will cost you a weekend.** If a route fails, the honest
> response is to wait longer before the next try, not to try harder. The hub is a microcontroller serving a web
> app, a broker connection, the measurement loop and you at the same time; a client hammering it in a tight
> retry is indistinguishable, from the hub's side, from a client doing it on purpose.

---

## Reading what the hub has measured

**History** is per point and per period, and it is the same data the app's History page shows — including the
quality of each record, which is the part integrators most often drop and then cannot explain a figure. A
record carries bits saying whether it is partial, recomputed, estimated, or covers an outage. Carry them
through to whatever you build; a number whose quality you have discarded is a number you cannot defend.
→ [History](web-history.md)

**Economics** is cost and export value, computed by the hub with the price in force for each period — not
recomputed from a current rate. → [How cost is calculated](web-cost-calculation.md)

**`export.csv`** gives you the same thing as a file, by point, period and type. For anything that wants a
spreadsheet or a long archive, this is the cheaper route: one request instead of a loop.

> ⚠️ **Hourly history is kept for 60 days**, then daily, monthly and yearly. If you need the hours of a whole
> billing period after the fact, pull them before they roll — a scheduled `export.csv` is the usual way.

---

## The price routes (firmware 1.2.0 and later)

Three routes, and they answer three different questions:

| Route | Answers |
|---|---|
| `/api/v1/dynprice` | how pricing is configured, which providers exist, and the live state |
| `/api/v1/price` | the resolved price per 15-minute period — and with `log=1`, what each closed period was actually booked at, for 40 days |
| `/api/v1/signal` | the current price notification: the price now, its source, the period's rank in the day, which cheap windows are open, and the two flags |

**`/api/v1/price` is also a write.** A client that knows your prices — a Home Assistant integration, your own
script — can push up to 200 periods into the hub, and the hub prices energy with them. That is the path for a
supplier the hub has no source for. You state whether what you push is a spot price or an all-in one; getting
that wrong is the single most expensive mistake available here, because it silently multiplies every cost
figure. → [Where your price comes from](hub-dynamic-prices.md)

**For the notification, prefer the retained MQTT topic over polling `/api/v1/signal`.** It is a *state*: the
hub republishes it at every 15-minute boundary and the moment anything changes, and a retained message hands
the current state to a client that connects later. A poller gets the same thing, late, and misses nothing only
by asking constantly. → [Knowing when energy is cheapest](hub-price-notification.md)

---

## Changing things

Writes take the admin token and, for some routes, need a restart before they apply — the generated reference
has a column saying which. A few rules that are not in any table:

- **A refused write names the field.** The hub answers `400` with the field it did not accept, which is far
  more useful than a status code. Read it: it is usually telling you something true about your configuration,
  such as a price zone trading in a currency your plan is not in.
- **Names are limited in bytes, not characters** — 24 bytes for a short name, 31 for a name. A Cyrillic or
  accented letter takes two, so a name that looks short can still be refused.
- **Configuration is a whole document on some routes.** Where a route takes a config section, sending a partial
  one replaces the section. Read it, change what you mean to change, send it back.

---

## What the API will not do

- **No cloud, no account, no remote access.** The hub serves your network. Anything beyond that is your
  tunnel, your VPN, your responsibility.
- **It controls nothing.** There is no route that switches a load. The hub measures, prices, and publishes;
  acting on that is the job of whatever you build. → [Knowing when energy is cheapest](hub-price-notification.md)
- **No per-route rate limit to lean on.** The absence of one is not permission; see the backoff warning above.

---

## When a call does not do what you expect: what to check

**`401` on something that worked yesterday.**
A token was set on the hub since. Reads need the read token once both tokens exist; writes need the admin one.

**`Admin token required.` on a write you are sure is authorised.**
The token is set but not reaching the hub — check it is a bearer token on the request itself, not a query
parameter.

**A browser gets a CORS error, `curl` works fine.**
That is the browser refusing, not the hub. Add your origin in `/api/system/security`; there is room for eight.

**A write returns `400` and names a field you did not send.**
Some routes take a whole configuration section. The field is in the section you replaced, and it was not valid
in the state the hub is in — a zone whose currency does not match the plan's is the common one.

**A setting saved but nothing changed.**
Some routes need a restart to apply; the generated reference says which in its own column.

**History has gaps.**
Gaps are records the hub does not have, not records it lost: it was restarting, updating or without power. The
quality bits on the neighbouring records say what happened. → [History](web-history.md)

**The price log has fewer than 96 rows for a day.**
Same cause, same meaning: the log holds the periods the hub was running through. A missing period is not a
missing price.

**Live values arrive slowly, or out of step with the app.**
You are polling where you should be subscribing. → [Live data over WebSocket](hub-api-websocket.md)

> ⚠️ For any work inside an electrical panel, call a qualified electrician.
