# Knowing when energy is cheapest, in time to use it

You can read yesterday's prices on any screen. What is hard is knowing, **right now**, whether this
15-minute period is one of the cheap ones — and having something act on that without you writing a price
comparison by hand and rewriting it every time your tariff changes.

The hub does the comparing. It works out which periods are the cheap ones under rules you set, and publishes
one small message saying what is open now and what opens next. Something else reads that message and acts.

> ⚠️ **The hub switches nothing.** There is no relay on it, and no setting that makes it turn anything on or
> off. It computes windows and publishes them. The decision and the switching belong to whatever reads the
> notification — a Home Assistant automation, a diverter, a charger that subscribes to MQTT itself. If you
> were looking for a device that controls a load by price, this is not that device.

---

## It is not only for dynamic prices

The rules work on the **all-in buy price of each 15-minute period**: the dynamic price when one covers that
period, and otherwise **your plan's zone rate with its charges**. So a hub on a plain time-of-use plan gets
cheap windows too — they simply follow your own zones, and time-of-use grid fees count towards the comparison
like everything else.

Which means this page is worth reading whether or not you use
[dynamic prices](hub-dynamic-prices.md). What changes with a market price is how much the cheap periods
move about from day to day; the mechanism is the same.

The rules need the hub's clock to be set, and they use the hub's own time zone to decide what "a day" and
"07:00" mean — see [Time](web-time.md).

---

## Three kinds of rule

Up to **eight** rules, each with a name you choose, and each switchable on and off on its own. A name is at most **23 bytes** as the hub stores it — the app says so plainly, and a Cyrillic letter counts as two.

### Price below a level
The app calls it **Price below a level**. You give a price to switch on at or below, and a price to switch off at or above.

| Parameter | |
|---|---|
| `on_below` | active while the all-in buy price is at or under this |
| `off_above` | stays active until the price reaches this; must be at or above `on_below` |

The two levels exist so a price hovering around one number doesn't make the window flicker on and off every
period. Set them equal and you get exactly that flicker, so give yourself a gap.

**Use it when** your threshold is absolute: "run while electricity is under 10 cents, whenever that happens."

### Cheapest periods in a time span
The app calls it **Cheapest periods in a time span** — «for a load that can run any time in a span, such as a water heater». You give a count of 15-minute periods and the hours to look within.

| Parameter | |
|---|---|
| `periods` | 1 to 96, each 15 minutes |
| `from_h`, `to_h` | the daily window, local hours; `to_h` at or before `from_h` crosses midnight, and equal hours mean the whole 24 hours |
| `consecutive` | when on, the periods must be one unbroken block |

**Use it when** the amount of running time is what you know: "the four cheapest 15-minute periods between
22:00 and 06:00." With `consecutive` on you get one unbroken hour instead of four scattered quarter-hours —
which is what you want for anything that dislikes being interrupted.

### Cheapest periods before a time
The app calls it **Cheapest periods before a time** — «for a load that must be done by a time, such as an EV by 07:00». Same idea as above, anchored to the hour it must be finished by.

| Parameter | |
|---|---|
| `periods` | 1 to 96 |
| `deadline_h` | the local hour it must be done by |
| `consecutive` | as above |

The hub looks at the 24 hours ending at that hour each day and picks the cheapest periods in them.

**Use it when** the deadline is the real constraint: "give it two hours of running before 07:00, and pick the
cheapest two hours in the night to do it."

A rule is refused rather than quietly trimmed if it cannot be satisfied — asking for more periods than the
window holds comes back naming the field that is wrong. That is deliberate: a window silently shortened to fit
would be a different rule from the one you wrote.

---

## Two flags, for when you don't need a window

Alongside the rules, two simple levels:

| Flag | True while |
|---|---|
| **Buy is cheap below** | the buy price now is at or under the level you set |
| **Sell is low below** | the sell price now is at or under the level you set — export is worth little, or nothing |

Leave a level unset and its flag stays false. These are not windows: there is no hysteresis and no planning
ahead, just a statement about the price right now. The sell flag is the one to watch on a site with
generation: it is the cheap signal for using your own energy instead of exporting it.

---

## What gets published

One message, under a kilobyte, on **`rbgrid/<mac>/price`** — **retained**, so anything that connects later
still gets the current state at once. Also at `GET /api/v1/signal`, and in the ESPHome build through
`rbgrid_cmd`.

| Field | What it tells you |
|---|---|
| `period_from`, `period_until` | the 15-minute period this is about |
| `buy`, `sell`, `currency` | the price now |
| `price_state`, `provenance` | the dynamic-price state, and where this price came from: your source, the backup, a push from Home Assistant, the hour's mean, or your plan |
| `rank_buy`, `rank_sell` | where this period ranks in its local day, 1 being the cheapest; 0 means not known yet |
| `periods_today` | how many of today's periods are known — the denominator for that rank |
| `windows[]` | one entry per enabled rule: its name, whether it is active, until when, and when it next starts |
| `buy_cheap`, `sell_low` | the two flags |
| `ts`, `v` | when the message was built, and its schema version |

### It is a state, not an event

The notification is republished at **every 15-minute boundary**, and immediately whenever anything changes —
a window opening or closing, new prices arriving, a rule or a level edited.

**Build your automation on the current value, not on the arrival of a message.** A consumer that treats each
publication as an event will fire repeatedly through a long window and will miss a transition that happened
while it was offline. A consumer that reads `windows[].active` and acts on what it says is right at every
moment, including the moment it reconnects — which is what `retained` is for.

Openings and closings are also written to the hub's own log, where you can read them afterwards in
[Events](web-events.md):

```
price: window 'Boiler' on until <time>
price: window 'Boiler' off, next <time>
```

---

## The example, drawn honestly

A boiler that runs on the cheapest two hours of the night:

1. On the hub: a rule named `Boiler`, type *cheapest periods of a daily window*, 8 periods, 22:00 to 06:00,
   `consecutive` on.
2. In Home Assistant: an automation triggered by the MQTT topic `rbgrid/<mac>/price` (or, in the ESPHome
   build, by the hub's own entity).
3. Its condition reads the entry named `Boiler` in `windows[]`. Active — Home Assistant turns the boiler
   switch on. Not active — it turns it off.

Every decision in that list after step 1 is Home Assistant's. The hub contributed one sentence: *this period
is in the window you described.* A diverter or a charger that subscribes to the topic itself works the same
way, and the division is the same: the hub states a fact about price, the other device decides what to do
about it.

Which also means the hub cannot be the reason something failed to run. If the automation didn't fire, the
notification is the thing to read first — it says what the hub believed, and that is checkable.

---

## What it does not do

- **No switching of any kind by the hub.** This is the whole design, not a gap waiting to be filled.
- **Eight rules.** A ninth is refused, not silently dropped.
- **No windows on the sell side.** Windows use the all-in buy price; for export use the *sell is low* flag.
- **Nothing happens without a set clock.** Rules describe hours and days, and a hub that hasn't synchronised
  its clock cannot place them.

---

## When it doesn't do what you expected: what to check

**A window never opens.**
Read the notification itself — `windows[]` carries `next_start` for every enabled rule. If `next_start` is
there, the rule is working and you are early. If the rule is missing from the list, it is switched off.

**A window opens at a different time every day, and I expected a fixed one.**
That is a cheapest-periods rule doing its job: the cheap periods move when prices move. If you want the same
hours daily, you want a zone in your [plan](web-tariffs.md), not a window.

**The window flickers on and off.**
A below-a-price rule with its two levels too close together, and a price sitting between them. Widen the gap
between switch-on and switch-off.

**My rule was refused when I saved it.**
The response names the field. The usual two: more periods than the window can hold, or a switch-off price
below the switch-on price.

**Windows ignore the dynamic price and follow my plan instead.**
They follow the all-in buy price of each period, which falls back to the plan whenever no dynamic price covers
that period. Check the `provenance` field in the notification: `plan` there means no market price was in force
for that period — see [Where your price comes from](hub-dynamic-prices.md).

**`rank_buy` is 0.**
The rank has no denominator yet: today's prices aren't fully known. It fills in as prices arrive.

**My automation fires over and over through one window.**
It is triggering on the message rather than reading the state. The notification republishes every 15 minutes
by design; condition the automation on `windows[].active` instead.

**My automation missed a window that the log says opened.**
The consumer was offline at the transition and is waiting for an event that already happened. Read the
retained message on connect — that is what it is retained for.

**The hours are wrong by a fixed offset.**
The rules use the hub's own time zone, not your phone's or Home Assistant's. Check
[Time](web-time.md).

**I want the window to account for my demand charge too.**
It doesn't — windows are about price per kilowatt-hour. The highest import of a billing cycle is measured
separately and is not priced into anything: see [Import peaks](hub-peaks.md).

> ⚠️ For any work inside an electrical panel, call a qualified electrician.
