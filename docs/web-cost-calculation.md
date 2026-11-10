# Why this hour costs what it costs

Every cost figure in RBgrid comes from the same short chain: measured energy, the **15-minute period** it fell into, the **tariff zone in effect for that period**, and your charges and VAT on top. Nothing is averaged over the day, and nothing is prorated.

This page is the chain end to end. It is the page to read when a figure surprises you - a cheap hour that looks expensive, an hour with energy you did not use at that time, or a total that does not match your supplier's.

Where the pieces live: you build the plan in [Tariffs](web-tariffs.md), you read the result in [Economics](web-economics.md), and the records are kept in [History](web-history.md).

## The chain, one step at a time

1. **The module measures.** An rbAmp module counts energy in its own counters, on its own board. The hub reads them; it does not integrate power into energy itself, so a missed reading does not invent or lose kWh.
2. **The hub closes a 15-minute period.** Periods end on the quarter-hour by the hub's clock. When your tariff zone changes mid-period, the hub closes the period at that moment too, so no period ever straddles two prices.
3. **The period gets a zone** - the zone in effect **for the period's own time**. This is the step most surprises come from, and it is the step with the most rules; they are all in *When the clock or the link was not there* below.
4. **The zone gives a price**, plus the plan's per-kWh charges, plus VAT.
5. **Export is valued separately**, at the zone's sell price - never netted against the cost. See *Export is not a discount*.
6. **Periods add up** into the hour, the hour into the day, the day into the calendar month and year. Each level is the sum of the one below; no level is recomputed from an average.
7. **What is left over is "unmetered"**: the supply point minus the points you measure. See *Unmetered, and points inside points*.

## The formula

For each measurement point and each period:

```
cost = ( kWh × zone buy price  +  kWh × (energy tax + grid fee + renewable levy) ) × (1 + VAT% / 100)
```

The three per-kWh charges are plan-level: they apply in every zone. Fixed monthly or yearly charges are **not** included in any RBgrid figure - costs here are energy-based only, which is the usual reason a month total sits below your bill.

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
- **The hub was off for more than a day.** The energy the modules counted meanwhile is not dropped: it is credited to the hour the hub came back, because its time inside the outage is not knowable. On firmware **v1.1.8** that energy is priced at the plan's **default zone**. The record carries a note of its own in History: *"includes energy from a break in measurement, counted in this hour"*.

  > ⚠️ **Such an hour holds energy that was not used in it.** The kWh are real and the money is approximate: the hub cannot know which rate applied during the break. If the break crossed a cheap window, the cost reads higher than it would have been.

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

**The month does not match my bill**
- Fixed monthly charges are not included in any RBgrid figure (see *The formula*).
- Months are **calendar** months in the hub's own time zone.
- Export value is not subtracted from cost here; your supplier may net them.
- Unmetered circuits are not in a point's cost. Compare your bill with the **supply** point, not with the sum of the lines.

**The numbers moved after a restart**
- A day rebuilt from its hours is marked "recomputed" in History; the value is the sum of the records that survived, and the hub says so rather than smoothing it.

## Where next

- [Tariffs](web-tariffs.md) - build the plan: zones, charges, VAT, sell prices.
- [Economics](web-economics.md) - cost, export value, net, and the input-versus-lines balance.
- [History](web-history.md) - the records themselves, and the quality column that explains a flagged period.
