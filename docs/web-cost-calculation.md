# Why this hour costs what it costs

Every cost figure in RBgrid comes from the same short chain: measured energy, the **15-minute period** it fell into, the **tariff zone in effect for that period**, and your charges and VAT on top. Nothing is averaged over the day, and nothing is prorated.

This page is the chain end to end. It is the page to read when a figure surprises you - a cheap hour that looks expensive, an hour with energy you did not use at that time, or a total that does not match your supplier's.

Where the pieces live: you build the plan in [Tariffs](web-tariffs.md), you read the result in [Economics](web-economics.md), and the records are kept in [History](web-history.md).

## The chain, one step at a time

1. **The module measures.** An rbAmp module counts energy in its own counters, on its own board. The hub reads them; it does not integrate power into energy itself, so a missed reading does not invent or lose kWh.
2. **The hub closes a 15-minute period.** Periods end on the quarter-hour by the hub's clock. When your tariff zone changes mid-period, the hub closes the period at that moment too, so no period ever straddles two prices.
3. **The period gets a zone** - the zone in effect **for the period's own time**. This is the step most surprises come from, and it is the step with the most rules; they are all in *When the clock or the link was not there* below.
4. **The period gets a price.** On a fixed plan that is the zone's buy price, plus the plan's per-kWh charges, plus VAT. With [dynamic prices](hub-dynamic-prices.md) on, the price comes from your provider for that very period — and whether the plan's charges go on top of it depends on which kind of source you use. Both cases are in *The formula*.
5. **Export is valued separately**, at the zone's sell price - never netted against the cost. See *Export is not a discount*.
6. **Periods add up** into the hour, the hour into the day, the day into the calendar month and year. Each level is the sum of the one below; no level is recomputed from an average.
7. **What is left over is "unmetered"**: the supply point minus the points you measure. See *Unmetered, and points inside points*.

## The formula

For each measurement point and each period:

```
cost = kWh × the period's buy price
```

Everything interesting is in how that buy price is arrived at, and there are two cases.

### On a fixed plan

```
buy price = ( zone buy price + energy tax + grid fee + renewable levy ) × (1 + VAT% / 100)
```

The three per-kWh charges are plan-level: they apply in every zone. Fixed monthly or yearly charges are **not** included in any RBgrid figure - costs here are energy-based only, which is the usual reason a month total sits below your bill.

### With dynamic prices on

Here the kind of source decides how much of the chain is already inside the number you were given.

**A spot source** publishes a wholesale price, so your contract and your plan are applied to it:

```
buy price = ( spot × factor + markup
              [ + the zone rate, if "zones stack" is on ]
              + energy tax + grid fee + renewable levy ) × (1 + VAT% / 100)
```

**An all-in source** publishes the retail price you actually pay, so nothing is added:

```
buy price = the provider's price
```

> ⚠️ **Which of the two your source is decides every cost figure in the app.** An all-in price already contains the taxes and VAT. Treated as a spot price, every one of those charges is applied a second time and the figures come out visibly too high. For a network provider the hub sets the class and you read it — an all-in source is shown with **· all-in** in the provider list and in the formula line. For Home Assistant the class is your choice: **What Home Assistant sends**. If your costs look inflated while the price itself looks right, this is the first thing to read.

The period's own price, and which source it came from, is kept for 40 days and can be downloaded - so a figure you read later can still be traced to the price that produced it. See [Where your price comes from](hub-dynamic-prices.md).

### Example: a dynamic price, two periods of one hour

A spot source, a contract with `factor` 1.0 and a `markup` of 0.01, a **grid fee** of 0.05 per kWh and **VAT** 20 %.

| Energy | Spot price for that period | Cost |
|---|---|---|
| 0.5 kWh, 14:00-14:15 | 0.22 | 0.5 × (0.22 + 0.01 + 0.05) × 1.20 = **0.168** |
| 0.5 kWh, 14:15-14:30 | 0.09 | 0.5 × (0.09 + 0.01 + 0.05) × 1.20 = **0.090** |

The same half kilowatt-hour, fifteen minutes apart, for nearly half the money - and the hour's total is the sum of its four periods, not the hour's average price times its energy. That distinction is the whole reason costs are computed per period.

### Example: a day/night plan

Two zones, **Day** 07:00-23:00 at 0.30 and **Night** 23:00-07:00 at 0.15 per kWh, a **Grid fee** of 0.05 per kWh, **VAT** 20 %.

| Energy | Zone | Cost |
|---|---|---|
| 2 kWh at 14:00 | Day | (2 × 0.30 + 2 × 0.05) × 1.20 = **0.84** |
| 2 kWh at 02:00 | Night | (2 × 0.15 + 2 × 0.05) × 1.20 = **0.48** |

The same 2 kWh cost less at night because the zone's buy price is lower; the fee and VAT apply in both zones.

## When the clock or the link was not there

A period is priced **by its own time, not by the time it was processed**. That holds when a period arrives late, when the hub was off, and when it comes from a remote node. What changes is which zone a period gets when its own time is not knowable.

- **A period processed late** keeps its own time and its own zone. From **v1.1.7** this is true even when the hub books the period minutes or hours afterwards - a late period is not priced at the zone that happens to be in force when it is booked.
- **A new tariff plan is applied in one step.** The costing never sees a half-loaded plan, so no period is priced at a mixture of the old and the new rates. A plan with more zones than the hub holds is refused outright rather than loaded in part.
- **The clock is not set yet** (a hub powered up without internet): periods are priced at the plan's **default zone** instead of whatever zone an unset clock would have fallen into. Records made before the clock was set are marked "before sync" in History and are left off the charts.

  Without a set clock a period cannot be matched to a provider's period either, so a **dynamic price is not used at all** — the plan's default zone prices it. Those periods also get **no row in the price log**: the gap in the log is how you recognise them.
- **The hub was off for more than a day.** The energy the modules counted meanwhile is not dropped: it is credited to the hour the hub came back, because its time inside the outage is not knowable. On firmware **v1.1.8** that energy is priced at the plan's **default zone**. The record carries a note of its own in History: *"includes energy from a break in measurement, counted in this hour"*.

  > ⚠️ **Such an hour holds energy that was not used in it.** The kWh are real and the money is approximate: the hub cannot know which rate applied during the break. If the break crossed a cheap window, the cost reads higher than it would have been.

  **With dynamic prices on, that period is priced in two parts**, and knowing this is what stops you misreading the log. The energy measured after the hub came back uses **the period's own price** — the market one, if a price covers it. The energy counted during the break uses the plan's **default zone**, because its real price is unknowable (a negative default rate is treated as zero). The record's money is the sum of the two.

  The price log shows that period's own price, and **not** the rate the outage part was costed at. So a period whose money looks high against the price in the log is not a contradiction: part of its energy was never priced at that number.

- **A remote node** closes its own 15-minute periods by its own clock and keeps them until the hub confirms them, so a node period is priced by **the period the node closed**, not by the time it reached the hub. From node firmware 1.5 the node keeps about five days of periods in its own flash. → [Remote nodes](web-nodes.md)
- **A node period longer than 15 minutes** - energy a node counted before it had the hub's clock - is kept rather than discarded (**v1.1.8**). It goes into that period's hour and carries the outage flag.
- **A node input without a voltage module** books no energy at all (**v1.1.8**). The hub will not turn current into energy using a nominal voltage, because that would not be measured energy, so the hour stays without data for that point. Its **live** power is still shown, and that figure *is* an estimate from the nominal voltage - fine to watch, not something to bill against.

## Export is not a discount

Exported energy is valued at a sell price - the zone's own, or the plan's selling ratio × the buy price - and **no taxes, fees or VAT are applied to it**. Export value is kept beside the cost, not subtracted from it. Economics shows both, plus **Net cost** (cost − export value) and **Net energy** (imported − exported).

If your contract pays nothing for exported energy, the sell prices must be **0**: the hub values exports whether net metering is on or off. Setting them up, and the kWh credit on top, are in [Tariffs](web-tariffs.md) and [Economics](web-economics.md).

## Unmetered, and points inside points

**Unmetered** is what the supply measured minus what your measured points account for: the circuits you have not clamped. It is a subtraction, so anything counted twice distorts it.

That is what **points inside points** are for (**v1.1.8**): a charger inside a metered sub-panel can be marked as measured **inside** that sub-panel. The hub's own totals do not change - they are taken at the supply - but the unmetered figure stops subtracting the nested load twice. Without it, a sub-panel and a load inside it both count against the supply, and unmetered reads lower than it is, sometimes negative.

> ⚠️ **One point at the supply, and no load counted twice.** A sub-panel point *and* a point inside it are correct only when the inner one is marked as inside. → [Points](web-points.md)

## What to check when a figure looks wrong

**The wrong zone applied**
- Check the hub's clock and time zone - until the clock is set, periods go to the default zone. → [Time](web-time.md)
- Where zones overlap, the higher-priority zone type wins, and a Weekend zone wins only on Saturday and Sunday. Open **The week by zones** in Tariffs to see the zone for each hour, and which hours fall to the default zone.
- Check that the zone is **On** and that its **Days** include the day in question.

**An hour holds energy that was not used then**
- Look at the record's quality in History: a flagged hour is one the hub is telling you about, not a silent error. An outage hour is priced at the default zone by design.
- A remote node that was out of reach delivers its periods late; they land in the hours they belong to, so an hour can gain energy after you first looked at it. → [Remote nodes](web-nodes.md)

**Cost is zero**
- The zones' buy prices are 0, or the point measured no energy in that period.

**Every cost figure is far too high, and the price itself looks right**
- An all-in source is being treated as a spot one, so the plan's taxes and VAT are added on top of a price that already held them. Read your source's class in the provider list (an all-in one shows **· all-in**); if Home Assistant pushes the price, check **What Home Assistant sends**, which is the one place the class is yours to set. → [Where your price comes from](hub-dynamic-prices.md)

**Every cost figure is too low, or lower than my supplier's app**
- The reverse: a bare spot price with no contract behind it. A wholesale number is not a retail one. Check `factor` and `markup`, and that the plan behind them carries your energy tax, grid fee, levy and VAT.

**The cost is right but the price is not the one I see at my supplier**
- Read the price log for those periods: it keeps the price applied and where it came from. A `plan` entry means no market price covered that period, so the zone rate priced it - which is correct behaviour, and a different number.

**A period's money does not match the price the log shows for it**
- Two cases, and both are by design. The period carries an outage flag: part of its energy was counted during a break and is costed at the plan's default zone, which the log does not show (see *When the clock or the link was not there*). Or the period has **no row at all** in the log: it was recorded before the clock was set, and the default zone priced it.

**A period cost less than nothing**
- The market price was negative and your contract passes that through, so the hub booked a negative cost. If yours does not, set a buy floor.

**My hourly totals differ from my supplier's by a small, steady amount**
- If your contract settles per hour while the hub prices per 15-minute period, every hour will be slightly out in both directions. The **hourly mean** option bills each period at the hour's mean instead.

**The month does not match my bill**
- Fixed monthly charges are not included in any RBgrid figure (see *The formula*).
- Months are **calendar** months in the hub's own time zone.
- Export value is not subtracted from cost here; your supplier may net them.
- Unmetered circuits are not in a point's cost. Compare your bill with the **supply** point, not with the sum of the lines.

**The numbers moved after a restart**
- A day rebuilt from its hours is marked "recomputed" in History; the value is the sum of the records that survived, and the hub says so rather than smoothing it.

## Where next

- [Tariffs](web-tariffs.md) - build the plan: zones, charges, VAT, sell prices.
- [Where your price comes from](hub-dynamic-prices.md) - the provider, the backup, your plan as the fallback, and the record of which priced each period.
- [Economics](web-economics.md) - cost, export value, net, and the input-versus-lines balance.
- [History](web-history.md) - the records themselves, and the quality column that explains a flagged period.
