# Where your price comes from

Your supplier charges a different price every 15 minutes, and most energy monitors price everything at one flat
rate. Every cost figure they show is then a guess — not wrong by a little, but built on a number that was never
your price. If your contract follows the market, the only cost worth reading is one that used **the price of the
period the energy was measured in**.

That is what this page is about: where the hub takes that price, what it does to it, and how to see which price
it used. What the screen shows, and what to do when a figure looks wrong, is on [Dynamic prices](web-prices.md).

> ⚠️ **The hub prices energy. It switches nothing.** There is no load control here: no relay, no charger, no
> inverter, no "cheapest hour" automation on the device. If you want something to run when energy is cheap, the
> hub publishes a notification and Home Assistant acts on it — see
> [Cheap windows and the price notification](hub-price-notification.md).

---

## The short version

- Dynamic prices are **off by default**. With them off, your plan prices every period, exactly as before.
- With them on, each 15-minute period gets its price from **your primary source**; if that source has no price for
  the period, from **your backup source**; if neither has one, from **your plan**.
- The hub records **which of the three** was used, for every period, and keeps that record for 40 days.
- Your plan never stops mattering: it is the fallback, it supplies the taxes and VAT added to a market price, and
  its currency is the money currency.

---

## The chain, in order

For each 15-minute period the hub resolves one price:

| | source | what it means |
|---|---|---|
| 1 | **Primary** | the provider you chose, for your zone |
| 2 | **Backup** | optional, and must be a different provider; used only for periods the primary doesn't cover |
| 3 | **Your plan** | the zone rate in effect for that period — see [Tariffs](web-tariffs.md) |

Nothing here is a blend: one period, one price, one source. The source is stored with the period, so a figure you
read next week can still be traced to where its price came from.

### Why a backup is worth setting
A day-ahead market publishes tomorrow once a day. If that publication is late, or the fetch fails, or your zone is
missing from a response, the primary simply has no price for those periods. Without a backup the plan steps in —
which is correct, but it is a *different* price, and your costs for that stretch will not match the market. A
backup from a second provider covering the same zone keeps the market price in place.

---

## The sources

| Source | Kind | Resolution | Areas | Currency |
|---|---|---|---|---|
| **Energy-Charts** (Fraunhofer ISE) | spot | 15 minutes | AT, BE, CH, CZ, DE-LU, DK1, DK2, FR, HU, IT-North, NL, NO2, PL, SE4, SI | EUR |
| **Nord Pool** (unofficial data portal) | spot | 15 minutes | SE1–4, NO1–5, DK1–2, FI, EE, LV, LT | the area's: SEK, NOK, DKK or EUR |
| **Elpriset just nu** (SE) | spot | 15 minutes | SE1–4 | SEK |
| **Hva koster strømmen** (NO) | spot | 1 hour | NO1–5 | NOK |
| **EnergyZero** (NL) | all-in | 15 minutes | NL | EUR |
| **Octopus Agile** (UK) | all-in | 30 minutes | the 14 GSP regions, A–P | GBP |
| **Home Assistant** | you choose: spot or all-in | whatever is pushed | — | your plan's |

No source needs a token or an account in this release.

**Resolution is not the billing step.** A 30-minute or hourly price applies to each 15-minute period inside it, so
your costs are still computed per 15-minute period — four of them simply share one price. The step at which you are
*billed* is a separate setting; see [Hourly mean](#hourly-mean-when-your-contract-settles-by-the-hour).

**Spot or all-in is the single most important thing to get right**, and it is explained below, because getting it
wrong is the most common way a cost figure comes out too high.

> ⚠️ **Currency is not converted.** The hub will not turn SEK into EUR. A zone whose currency differs from your
> plan's currency is refused when you save it (the field comes back as an error), and a source in another currency
> is skipped rather than approximated. Nord Pool SE3 needs a plan in SEK; Energy-Charts DE-LU needs a plan in EUR.
> Set your plan's currency first, then choose a zone that matches it.

### Attribution is shown, and that is on purpose
Each source carries the wording its own licence asks for — for example `Bundesnetzagentur | SMARD.de /
Energy-Charts (CC BY 4.0)` — and the hub displays it on the Prices page. It is not decoration: it is the condition
on which the data may be used, and it tells you what you are looking at. Energy-Charts is served only for the zones
published under CC BY 4.0.

**Nord Pool is reached through an unofficial data portal**, and the hub says so where it names the source. There is
no service agreement behind it: it can change or stop without notice. Use it knowingly, and give it a backup.

---

## What your contract does to the price

A market price is not what you pay. The two kinds of source differ in how much of the rest is already in the number.

### A spot source: the price is the start of the chain

```
buy = ( spot × factor
        + markup
        [ + your plan's zone rate, if "zones stack" is on ]
        + your plan's per-kWh energy tax
        + grid fee
        + renewable levy )
      × ( 1 + your plan's VAT )
```

`factor` and `markup` are your contract's terms: a multiplier and a per-kWh addition. Everything else comes from
the plan you already entered on [Tariffs](web-tariffs.md), which is why the plan still matters when the price
is dynamic.

Accepted ranges, and a value outside them is refused rather than clamped: `factor` 0 to 10, `markup` −100 to 100.

### An all-in source: the price is the whole chain

```
buy = the provider's price
```

Nothing from the plan is added — no tax, no grid fee, no levy, no VAT.

> ⚠️ **This is the mechanism that prevents the most common cost error.** An all-in price already contains the
> taxes and VAT. If it were run through the spot formula, every one of those would be added a second time and your
> costs would come out visibly too high. EnergyZero and Octopus Agile are marked all-in for that reason — **the
> hub sets a network provider's class, not you**; you see it in the provider list as **· all-in** and in the
> formula line. The one place the class is your choice is Home Assistant, where **you** state which kind you are
> pushing. If your costs look inflated and the price itself looks right, this is the first thing to read.

### Home Assistant as the source
Home Assistant — or any client — can push prices to the hub instead of the hub fetching them: up to 200 periods at
a time, kept in the hub's own store. You mark whether what you push is a **spot** price or an **all-in** price, and
from then on it is treated exactly like a provider of that kind. This is the path for a supplier the hub has no
source for, or for a price you compute yourself.

In the ESPHome build Home Assistant is listed among the sources from the start; on the standalone firmware it
appears in the list once a push has arrived. Either way dynamic prices are off until you turn them on, and the
source is yours to pick — Home Assistant is one option in the list, not the one already chosen.

Everything on this page is true of both builds: the ESPHome hub runs the same core and the same fetcher, with
the same providers, schedule and fallback chain. What differs is only where you set it — in the ESPHome build
that is the RBgrid panel in Home Assistant rather than the web app.

---

## The sell side

Export is priced separately, and never by the same number as import. Three modes:

| Mode | The sell price is |
|---|---|
| **From the provider** (default) | the source's own export price where it has one, otherwise your plan's zone sell rate |
| **From the tariff plan** | your plan's zone sell rate |
| **Market price × factor − fee** | `spot × export factor − export fee` |

The export factor accepts 0 to 10, the export fee −100 to 100 (`k_sell` and the feed-in fee in the API). No tax or VAT is applied to the sell side.

**Export value is still never subtracted from your cost.** Imported energy and its cost, exported energy and its
value, side by side — see [Economics](web-economics.md). A dynamic price changes the numbers, not that rule.

---

## Bill at the hourly mean: when your contract settles by the hour

Some contracts follow the market but settle **per hour**, even where the market publishes every 15 minutes. Turn
**Bill at the hourly mean** on and each 15-minute period of an hour is billed at the mean of that hour's prices.

This is a property of **your contract**, not of your source. A 15-minute source with hourly settlement needs it
on; an hourly source does not need it at all, because its four periods already share one price. Turning it on
"because my provider is hourly" changes nothing; leaving it off when your contract is hourly makes every period
differ from your supplier's arithmetic by a little.

Periods billed this way are marked `hourly_mean` in the price log, so you can tell them apart afterwards.

---

## Negative prices, and floors

When the market goes negative, the hub **keeps the negative price** and books a negative cost for that period. That
is the honest reading: during those periods, consuming energy credits you under a contract that follows the market.

If your contract does not pass negative prices through, set a floor: **Lowest buy price** and **Lowest sell price**
(−100 to 100, or none; `floor_buy` and `floor_sell` in the API). Left off, each reads «None: negative prices pass
through». The floor is applied last, after everything else.

With dynamic prices **off**, plan rates never go below zero, exactly as before. Floors exist because a market
price can, and your contract may not.

---

## When prices arrive

- **Today's** prices are fetched as soon as the day isn't fully covered.
- **Tomorrow's** are fetched from **11:00 UTC**, when day-ahead publication begins; the app puts it the way a
  reader experiences it — «they usually arrive after 13:00 CET».
- If a fetch fails, or tomorrow isn't out yet, the hub retries **every 15 minutes until 15:00 UTC**, then hourly.
  After 15:00 UTC the page also says so outright: the state becomes «Tomorrow's prices not in yet».

So an empty second half of the chart before midday is normal, not a fault: tomorrow does not exist yet.

Both outcomes are written to the hub's log, and you can read them in [Events](web-events.md):

```
price: state A -> B (source …)
FAIL price: <provider> <zone> fetch failed (<reason>), retry in N min
```

The reason is one of `fetch_failed`, `http_status`, `parse`, `auth`, `no_data_for_zone` or `rate_limited` — and
which one it is decides what to do about it (see below). The live state and how far prices are known are on the
Prices page, and in the API as `price_state` and `valid_until`.

---

## The record: which price was used, for every period

Every 15-minute period's applied price is logged with its source, and kept for **40 days** — including when
dynamic prices are off, where the entry simply reads `plan`. Download it as CSV from the Prices page.

| `prov` | the price came from |
|---|---|
| `source` | your primary source |
| `backup` | your backup source |
| `ha_push` | pushed in by Home Assistant or another client |
| `hourly_mean` | the mean of that hour, per your contract setting |
| `plan` | your tariff plan's zone rate |

🔑 **This is what makes a cost figure checkable rather than trustworthy.** A monitor that shows you a cost is asking
you to believe it. A monitor that shows you the price it used, per period, with where that price came from, is
handing you the arithmetic — and 40 days is long enough to settle an argument with a monthly statement in hand.

The history records themselves are unchanged in format: money is computed with each period's own price, and
anything already reading the hub's history or CSV needs no change.

---

## Where the price shows up elsewhere

- **[Dynamic prices](web-prices.md)** — the screen: price now, today and tomorrow, your source and formula,
  cheap windows, peaks, the log.
- **[Why this hour costs what it costs](web-cost-calculation.md)** — the full chain from energy to money,
  now with a market price in it.
- **[Cheap windows and the price notification](hub-price-notification.md)** — what the hub publishes so
  something else can act on a cheap stretch.
- **[Import peaks](hub-peaks.md)** — the highest 15-minute and hourly import of a billing cycle. Measured
  whether or not you use dynamic prices.
- **Home Assistant and MQTT** — the live telemetry carries the state, the source in use, how far prices are known,
  and the current buy and sell rate; see [MQTT broker](hub-mqtt.md).

---

## If you are on firmware v1.2.0, update

The hub's own release notes carry one recommendation, and it is worth acting on rather than reading past:
**update from v1.2.0 if you use dynamic prices.**

On **v1.2.0** the fetched price periods were kept in the hub's small settings store — the same store that holds
the hour in progress and the energy anchors. With dynamic prices on, the prices could fill it, and then a
restart could lose part of the hour being measured. The hub says so when it happens: the Hub log carries
`FAIL hour persist …: ESP_ERR_NVS_NOT_ENOUGH_SPACE`.

**v1.2.1** moves the price periods into the hub's file system and frees that space on the first start, logging
`price: stores moved from NVS to the file system`. Nothing a reader does changes, and nothing on this page
reads differently on either version — but the failure above stops being possible.

The cheap-window rules and the import-peak figures are small and stay in the settings store on both versions:
"the price stores moved" does not mean everything price-related moved.

## What it does not do

Stated plainly, because each of these is a thing a reader could reasonably expect:

- **No Tibber, no Amber.** Not implemented. If your supplier is one of them, push the price from Home Assistant.
- **No currency conversion.** Your plan's currency and your zone's currency must be the same one.
- **No service agreement behind Nord Pool.** The endpoint is an unofficial data portal and can change without notice.
- **No load control.** The hub measures and prices; it switches nothing. The notification is the hand-off point.
- **No capacity-charge billing.** Peaks are measured, not priced into your costs.

---

## When the figure is wrong: what to check

**My costs are far too high, and the price itself looks right.**
Your source is all-in but is being treated as spot, so the plan's taxes and VAT are added on top of a price that
already contained them. For a network provider the class is the hub's, not yours: read it in the provider list,
where an all-in source is shown with **· all-in**, and in the formula line. For Home Assistant the class *is*
yours — check **What Home Assistant sends**.

**My costs look too low, or the price is lower than my supplier's app.**
The reverse: a spot price with no contract terms behind it. Check that `factor` and `markup` match your contract,
and that the plan behind them carries your energy tax, grid fee, levy and VAT. A bare spot price is a wholesale
number, not a retail one.

**Half the chart is empty.**
If it is before mid-afternoon, tomorrow's prices are not out yet — the hub starts asking at 11:00 UTC and they
usually arrive after 13:00 CET. If it is after that, look in [Events](web-events.md) for a `FAIL price:` line and read
its reason.

**`FAIL price: … no_data_for_zone`.**
The provider answered, but not for your zone. Either the zone isn't in that source's list, or the source has
stopped serving it. Switch the zone, or switch the source.

**`FAIL price: … parse` or `http_status`, repeatedly.**
The source changed or is down. This is the case a backup source exists for; set one from a different provider
covering the same zone.

**`FAIL price: … rate_limited`.**
Too many requests reached the provider from your address. The hub backs off on its own — every 15 minutes until
15:00 UTC, then hourly — so this clears itself. If it persists for a day, something else on your network is polling
the same endpoint.

**I saved a zone and it was refused.**
The zone's currency does not match your plan's currency, and the hub does not convert. Set the plan's currency to
the one your market trades in, then save the zone.

**The price is right but costs are still computed from my plan.**
Look at the price log for those periods: a `plan` entry means neither the primary nor the backup covered them.
Either prices had not arrived for that stretch, or dynamic prices were off at the time — the setting applies from
the moment it is saved, and energy already measured keeps the price it was costed with.

**My numbers differ from my supplier's by a small, steady amount.**
Check **hourly mean** against your contract: if you are settled per hour and the hub is pricing per 15-minute
period, every hour will be slightly off in both directions. Turn it on and the hub bills the hour's mean instead.

**A period cost less than zero.**
The market price was negative and your contract passes that through, so the hub booked a negative cost. If your
contract does not pass it through, set `floor_buy` to the lowest price you can actually be charged.

**I changed the source and old costs did not change.**
They never do. A period is costed once, with the price in force when it was measured, and the figure stays — see
[Why this hour costs what it costs](web-cost-calculation.md). Changing a source changes what happens next.

> ⚠️ For any work inside an electrical panel, call a qualified electrician.
