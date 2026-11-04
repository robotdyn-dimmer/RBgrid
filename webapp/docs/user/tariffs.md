# Tariffs

## Overview

The **Tariffs** page holds the hub's electricity price plan: time-of-use zones, per-kWh taxes and fees, VAT, and the price for energy you export. The hub applies the plan to every reading and computes cost and export value per measurement point. Open it from the app's left navigation: **Management → Tariffs**.

The hub keeps **one tariff plan**. Editing it changes the rates used from then on.

## Plan settings

| Field | What it does |
|---|---|
| **Plan name** | Your name for the plan. |
| **Provider** | Your supplier's name, for your reference. |
| **Plan type** | A label for the plan. Prices come from the zones; the app warns if the type doesn't match them (for example "two-zone" with three zones). |
| **Currency** | The currency for all prices and costs. |
| **VAT %** | Added to the energy cost and the per-kWh fees (see *How cost is calculated*). |
| **Default zone** | The zone used for any hour that no other zone covers. |
| **Net metering (selling to the grid)** | Shows the sell-price fields on the zones and keeps the monthly kWh credit (see *Export valuation*). |

### Per-kWh taxes and fees

Three plan-level charges, each a rate per kWh, added to every zone's buy price:

- **Energy tax**
- **Grid fee**
- **Renewable levy**

There's no tax field on a zone: taxes and fees apply to the whole plan.

### Fixed charges

The hub doesn't include fixed (monthly or yearly) charges in any cost, so the editor doesn't show them. Costs and forecasts are energy-based only. Charges saved earlier stay in the plan untouched.

## Zones

A zone is a price that applies during a time window on chosen days of the week.

- At most **one zone of each type**. The editor offers seven types; **Add zone** is greyed out once all seven are used.
- **One time window per zone**, combined with the weekday mask.
- Any hour no zone covers falls to the **Default zone**, so you don't have to cover all 24 hours yourself.

### Zone fields

| Field | What it does |
|---|---|
| **On** | Turns the zone on or off without deleting it. |
| **Zone type** | Sets the zone's name and its priority (below). |
| **Buy price, <currency>/kWh** | The energy price in this zone, before taxes, fees and VAT. |
| **From / To** | The time window. It can cross midnight (22:00–06:00 works). From = To (for example 00:00–00:00) means the whole day: use it for whole-day zones, and not by accident. |
| **Days** | Monday to Sunday on/off. The zone applies only on the days that are on. |
| **Sell price, <currency>/kWh** | Shown with net metering on: the value of one exported kWh in this zone. |

### Zone types and priority

When zones overlap, the hub applies the one with the highest-priority type:

1. Critical Peak
2. Holiday
3. Weekend (Saturday and Sunday)
4. Evening Peak
5. Morning Peak
6. Night
7. Late Evening
8. Day

Notes:
- **Holiday** dates can't be entered, so a Holiday zone never activates and the editor doesn't offer it. A Holiday zone saved earlier is kept.
- **Critical Peak** is a fixed window like any other zone. It doesn't receive supplier-announced (day-ahead) events.

### Two windows on the same day

A zone has one time window. If your tariff has the same price in two separate windows of a day (for example a cheap night and a cheap midday), put the second window in a zone of another type with the same buy price.

### Week by zones

The **week by zones** view shows which zone applies for every hour of the week. Use it to check overlaps and the default zone. The Tariffs page also shows the current zone, read when you open the page.

## Export valuation

Exported energy is valued at a sell price:

- the zone's **Sell price**, if it has one;
- otherwise the plan's **selling ratio** (0–1, a share of the zone's buy price) × the buy price.

The hub values exports this way **whether net metering is on or off**. If your contract pays nothing for exported energy, the sell price must be 0. Watch for this after the setup wizard's quick tariff: its export price is written into every zone even though net metering stays off, and the **Tariffs** editor shows the sell-price fields only with net metering on.

Export value = exported kWh × sell price. No taxes, fees or VAT are applied to it, and it isn't subtracted from the cost: cost and export value are kept separately and shown side by side. Economics also shows **Net cost** (cost − export value) and **Net energy** (imported − exported).

Net metering **adds** a **kWh credit** on top: Economics shows a "Net metering — kWh credit" block with the credit balance, import and export this period, and credit earned and used. The credit is settled at each calendar-month boundary and doesn't change the money figures. It doesn't require a Main Supply point. Binding your supply as the **Main** point is still recommended, so the hub's totals describe your whole connection.

## How cost is calculated

For each measurement point and each reading:

```
cost = ( kWh × zone buy price  +  kWh × (energy tax + grid fee + renewable levy) ) × (1 + VAT% / 100)
```

The hub computes cost every 15 minutes at the zone rate in effect then, and stores it with the hourly records. Day, month and year costs are the sums of these. Changing the plan affects energy from then on; past costs keep the rates that applied. Nothing is prorated, and fixed charges aren't added (see *Fixed charges*). Months are calendar months.

## Where you see cost

- **Monitoring → Economics:** choose a point and a period (hour, today, yesterday, last 7 days, this month, last month, this year). Shows imported and exported energy, cost, export value and net for that point. It also shows a split **by zone, since the hub's last restart**.
- **History:** the **Money** metric per point.
- **Dashboard:** a month-end cost forecast: the cost so far this month, scaled linearly to the whole month at current rates. It excludes standing charges.

## Backup

The tariff plan is part of the hub's **restore point** (**Settings → Restore point**): a copy of the hub's settings, points, tariffs, time and modules, kept on the hub. There's one restore point; a new one replaces the previous. It isn't a downloadable file, and there's no separate plan export.

## Troubleshooting

**The wrong zone applies**
- Check the hub's time and timezone (see *Time Settings*).
- When zones overlap, the higher-priority type wins (list above).
- Check the zone's **Days** and that it's **On**.
- Open **week by zones** to see the zone in effect for each hour, and which hours fall to the default zone.

**Cost is zero**
- Check that the zones have a buy price above 0.
- Check that the point has measured energy in that period.

**Export has no value**
- Turn on **Net metering**.
- Give the zone a **Sell price**, or set the selling ratio.
