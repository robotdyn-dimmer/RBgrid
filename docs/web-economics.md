# Economics

**Economics** puts money next to energy for one point at a time: what it imported and exported, what that cost at your tariff, and what the export was worth. Open it from the app's left navigation: **Monitoring → Economics**.

The prices come from your tariff plan (see [Tariffs](web-tariffs.md)). Economics only shows figures; it doesn't change anything on the hub.

If a figure here surprises you, the chain that produced it — the 15-minute period, the zone in effect for it, the charges and VAT — is on one page: [How cost is calculated](web-cost-calculation.md). This page says what each tile holds; that one says where the number came from.

![Economics (heat pump + EV)](https://www.rbgrid.io/assets/img/webapp-docs/wa-economics-overview.webp)

## Selectors

- **Point**: every point that measures energy. The voltage bus isn't listed. The input comes first and is selected when you open the page. Each entry reads "name · role".
- **Period**: the period the **Totals**, **Generation** and **Balance** blocks cover.

| Period | Shown as | Built from |
|---|---|---|
| hour | last hour | the last closed hour |
| today | today (hub clock) | for the input, the hub's day counters; for other points, today's closed hours (see below) |
| yesterday | yesterday | closed records |
| last 7 days | last 7 days | closed days, plus today |
| this month | this month | closed days, plus today |
| last month | last month | closed records |
| this year | this year | closed months, plus this month's days, plus today |

All periods follow the **hub's** clock (see [Time](web-time.md)). A "closed" record is a finished hour, day or month.

How "today" is counted depends on the block:

- **Totals** for the input: today comes from the hub's day counters, which include the running hour (also for the today part of 7 days, this month and this year).
- **Totals** for any other point: closed records only; the running hour isn't included.
- **Generation** and **Balance**: today comes from each point's own day counter, for every point.

## Totals

Always shown for the selected point:

| Tile | What it is |
|---|---|
| **Imported** | energy taken from the supply in the period |
| **Exported** | energy sent back in the period |
| **Cost** | cost of the imported energy at your tariff |
| **Export value** | value of the exported energy at your sell price |
| **Net energy** | imported − exported |
| **Net cost** | cost − export value |

A footnote says where the figures come from:

- past periods: "From the hub's closed records (days checked against the hour records)."
- the input, on periods that include today: "Today from the hub's day counters (as on the Overview), earlier days from closed records."
- other points: "From the hub’s closed records; the running hour is not included."

**Points on a BASIC module** count consumption only. For them, only **Imported** and **Cost** are shown, with a note that there is no export, export value or net figure.

If the point has no records for the period, the block says "No data for this point."

## Generation

Shown only when at least one point has the **Generation** role and an input exists. It covers all generation points together and doesn't depend on the point you selected.

| Tile | How it is worked out |
|---|---|
| **Produced** | the energy your generation points sent out in the period |
| **Used on site** | produced − the part exported at the input |
| **Not bought from the grid** | an estimate: energy used on site × the period's average buy price at the input (input cost ÷ input kWh) |
| **Exported** | the part of production that left at the input (produced or input export, whichever is smaller), and the input's export value for the period |
| **Self-consumption** | used on site ÷ produced, in % |
| **Self-sufficiency** | used on site ÷ (used on site + input import), in % |

The caption under the block shows the average buy price used. Until the input has a cost for the period, it says the value of own use will appear later.

If there are no generation records for the period: "No generation data for this period."

![Economics for the home with solar (generation, export)](https://www.rbgrid.io/assets/img/webapp-docs/wa-economics-generation.webp)

## Balance: input and lines

Shown when an input exists and at least one **line** is metered. A line here is any point other than the input, a backup input, the voltage bus or a generation point.

| Tile | What it is |
|---|---|
| **Imported at the input** | what the input imported in the period |
| **Metered lines (n)** | the sum of what your lines imported; *n* counts the lines that have data for the period |
| **Unmetered** | input − the sum of the lines |
| **Unmetered, % of input** | the unmetered part as a share of the input |

The unmetered part is the consumption at the input that none of your points measures, for example circuits without a sensor.

**Unmetered is a subtraction, so a load counted twice distorts it.** If you meter a sub-panel *and* a circuit inside that sub-panel, both are lines, both are added to **Metered lines**, and the same energy is subtracted from the input twice: unmetered reads lower than it is, and on a well-metered board it can go negative.

From firmware **v1.1.8** that is what the **inside** setting is for: a point can be marked as measured inside another one, and then it is not added again here. The hub's own totals don't change — they are taken at the input — but this block stops double-counting. Mark the inner point, not the sub-panel. → [Points](web-points.md)

> ⚠️ **A negative or implausibly small unmetered figure has two causes, and only one of them is yours.** Look first for a load counted twice — a point that sits inside another one. The second cause was the app's own, and web app **1.2.0** removes it (see below).

### On a site with generation (web app 1.2.0)

Up to web app 1.1.8 this balance weighed the input's **import** against what the lines used. On a house with solar that is not a fair comparison: lines fed partly by your own panels can use more than was ever imported, so **Unmetered** could read negative with nothing wrong on your board.

From **1.2.0** the comparison is against what was actually used on site:

```
Used on site = import + generation − export
Unmetered    = used on site − the metered lines
```

and the share is shown as **Unmetered, % of use**.

![The Balance block on a site with generation: «Used on site (import + generation − export)» 29.5 kWh, the metered lines beneath it, «Unmetered» 10.5 kWh and «Unmetered, % of use» 35 %](https://www.rbgrid.io/assets/img/webapp-docs/wa-economics-balance-solar.webp)

So on a generating site the block now answers the question it always looked like it was answering: of everything the house used, how much went through circuits you measure. A site without generation is unchanged in substance — with no panels, what was used on site *is* what was imported.

If any line has no records for the period, the block names those points and shows "—" for **Unmetered** and **Unmetered, % of input**: they aren't counted until every line has data. **Unmetered, % of input** also shows "—" when the input imported less than 10 Wh in the period, since a share of almost nothing is noise rather than a reading. (On app v1.1.4 and earlier the threshold was exactly nothing imported.)

If the input itself has no records for the period, the block shows "No data for this point." instead of the tiles.

**A three-phase input metered per phase.** From firmware **v1.1.6**, with the matching app package, every figure on this page covers the **whole input**: the hub serves the three phase points as one summed series and the page reads it. On **v1.1.5** the page read the **L1 point** alone, so cost, the balance, generation and the export value all understated a per-phase input. One consequence worth knowing: the input's entry in the **Point** list is now the three phases summed, so **L2** and **L3** are a click away there, while **L1** on its own is on the input point's own page or in [History](web-history.md) — both still read each point one at a time.

![Economics · «Balance: input and lines» with the unmetered share](https://www.rbgrid.io/assets/img/webapp-docs/wa-economics-balance.webp)

## Running totals

The blocks below don't follow the **Period** selector. They follow only the selected point and show what the hub has accumulated.

### By zone (since the hub's last restart)

A bar split by tariff zone, with the total cost. Below it, one row per zone that has a cost or export value, for example "Night", "Day" or "Evening peak": the cost, and "earned …" where there is export value.

The figures count from the hub's **last restart**. If there are no zone charges yet, the block says so.

### Net metering — kWh credit

Shown when **Net metering (monthly kWh credit)** is on in your tariff plan; hidden while it's off (see [Tariffs](web-tariffs.md#export-valuation)). On app v1.1.4 and earlier the toggle is labelled **Net metering (selling to the grid)**.

| Line | What it is |
|---|---|
| **Credit now** | the kWh credit you hold now |
| **Imported in the billing period** | kWh imported this month |
| **Exported in the billing period** | kWh exported this month |
| **Credit earned since net metering was on** | all credit earned so far |
| **Credit used since net metering was on** | all credit used so far |

The billing period is the calendar month. At each boundary the hub nets that month's import against its export: a surplus is **added** to the credit, at the plan's **selling ratio**; a shortfall is **paid out of** the credit first, and only what is left is billable. The credit then carries into the next month, up to **1,000 kWh**, while the month's import and export counters start again. Carry-over and that ceiling are **fields of the plan**, not properties of the firmware: a plan built in the app takes the defaults (carry-over on, 1,000 kWh) and the app shows no control for either, so only a plan sent over the REST API can turn the carry-over off — and then the bank is cleared at each settlement instead of carrying. The values are in kWh, as the hub reports them; the credit doesn't change the money figures.

Switching net metering off and on again doesn't clear the credit: it is the hub's own state, and on a plan that carries over it is cleared only when the point is deleted.

**What the off state does change, from firmware v1.1.7:** while net metering is off the hub no longer collects the month's import and export at all. So the first month-end after you switch it back on settles **only the part of the month since you switched it**, not the whole month. On **v1.1.6 and earlier** those counters kept running while the toggle was off, and the first settlement could include days when net metering wasn't in force.

Deleting a point clears its net-metering record too — the credit bank and this period's balance — so a point bound later under the same ID starts at zero (firmware **v1.1.6**; on **v1.1.5 and earlier** a reused ID carried on the removed point's credit).

If the input is on a BASIC module, which doesn't measure export, a note says no credit accrues and the figures shown were accumulated before.

### Progressive tariff

This block appears only when the hub runs a progressive tariff, so on most hubs you won't see it. It then shows the current **Tier**, the **Accumulated** import in kWh and the **Multiplier ×**.

If the hub doesn't answer for these running totals, the blocks are simply not shown.

## How money is shown

Amounts use the currency of the hub's active tariff plan and the number of decimals the hub reports. The export value is shown separately from the cost and isn't subtracted from it, except in **Net cost**.

## What to check when a figure looks wrong

**Unmetered is negative, or far too small**
- A metered sub-panel and a point inside it are both counted as lines. Mark the inner one as measured **inside** the outer (firmware v1.1.8) — see *Balance: input and lines* above.
- **On a house with solar, expect it:** the block weighs the input's **import** against the lines' use, and lines fed by your panels can exceed what was imported. Nothing on your board is wrong, and the rest of this page is unaffected (firmware v1.1.8).
- A generation point is not a line and is never subtracted here; if you bound a producing circuit with the wrong role, it lands in the wrong block. → [Points](web-points.md)

**Unmetered shows "—"**
- One of your lines has no records for the period: the block names them, and the figure is withheld until every line has data rather than shown wrong.
- Or the input imported less than 10 Wh in the period, where a percentage would be noise.

**Cost looks too high for a cheap night**
- Check which zone the energy actually fell into: an hour that holds energy from a break in measurement is priced at the plan's default zone, not at the zone that was in force during the break. → [How cost is calculated](web-cost-calculation.md)
- **By zone** counts from the hub's **last restart**, not from the period selector. After a restart the split starts again, so a short bar is not a small bill.

**Cost is zero but energy isn't**
- The zones have no buy price, or the plan's default zone has none. → [Tariffs](web-tariffs.md)

**Export value is 0, or a value you didn't expect**
- Exports are valued at the zones' sell prices whether net metering is on or off. If your contract pays nothing, every zone's sell price must be 0 — check them all, not only the zone in force now. → [Tariffs](web-tariffs.md#export-valuation)

**The month here doesn't match my bill**
- Fixed monthly charges are in no RBgrid figure, and months are calendar months on the hub's clock.
- The kWh credit doesn't move the money figures; it is settled in kWh.
- Compare your bill with the **input**, not with the sum of the lines: unmetered circuits are in the bill and not in any point.

**Totals differ between Economics and Overview for "today"**
- For the input, "today" includes the running hour (the hub's day counters). For any other point it is closed hours only. The footnote under **Totals** says which rule produced the figure you are reading.

**Nothing at all: "No data for this point."**
- The point has no records for that period — a point beyond the hub's 12 history slots shows live values only and never appears here. → [Points](web-points.md)

## Where next

- [How cost is calculated](web-cost-calculation.md) — why a period costs what it costs.
- [Tariffs](web-tariffs.md) — zones, charges, VAT, export prices, the kWh credit.
- [History](web-history.md) — the records behind these totals, and the quality of each one.
