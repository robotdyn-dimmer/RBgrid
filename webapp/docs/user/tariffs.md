# Tariffs

## Overview

The **Tariffs** page holds the hub's electricity price plan: time-of-use zones, per-kWh taxes and fees, VAT, and the price for energy you export. The hub applies the plan to every reading and computes cost and export value per measurement point. Open it from the left navigation, **Management → Tariffs**, or from the **Tariff** card in Settings.

The hub keeps **one tariff plan**. Editing it changes the rates used from then on; costs already recorded keep the rates that applied.

![The Tariff card in Settings: the plan, zones and prices are on the Tariffs page](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-tariff.webp)

> **A quick plan from the setup wizard.** The wizard's Tariff step sets a simple one- or two-zone plan from your bill price. It rebuilds the whole plan: taxes, fees, fixed payments and net metering are cleared, progressive is turned off, VAT is set to the value you enter, and the export price you enter (or 0) is written into every zone. If the hub already has a plan you set up, the wizard first asks "Replace the hub's tariff?". For anything more, use this page. → [First setup: the wizard](setup-wizard.md)

## Saving

Changes are kept in the page until you save. A bar stays at the top: *No unsaved changes* or *The tariff has unsaved changes*, with **Cancel** and **Save**. While a field has an error, the bar reads *Fix the fields marked red*. Saving applies at once; the hub doesn't restart. If you leave the page with unsaved changes, the app asks first.

> ⚠️ **After the hub restarts, check this page.** On the current firmware a restart — a power cut, a reboot or a firmware update — leaves the hub running its built-in placeholder plan, **Test Day/Night** (0.20 / 0.30). Your plan is still in the hub's storage, but it isn't the one in use until you open Tariffs and press **Save** again. Energy measured in between is costed at the placeholder's prices and those costs are stored like any other, so re-save first and then read the figures. The cause is known — the plan loads correctly at boot and the tariff engine then overwrites it — and a fix is on its way.

**Save** stays disabled while:
- nothing has changed;
- the plan has no name;
- some hour of the week has no zone and there's no default zone;
- an enabled zone has no days;
- a field has an error.

## Plan settings

The header shows the plan name with an **active** chip. If the plan has an end date that has passed, a warning says so: the hub keeps pricing with it.

| Field | What it does |
|---|---|
| **Plan name** | Required. Your name for the plan. |
| **Provider** | Your supplier's name, for your reference. |
| **Plan type** | A label: Flat rate, Two-zone (day/night), Three-zone, Multi-zone, Progressive, Seasonal, Dynamic or Custom. Prices always come from the zones. For **Flat rate**, **Two-zone (day/night)** and **Three-zone**, the app warns when the number of enabled zones doesn't match (1, 2 or 3); for **Flat rate** with more than one zone it offers **One zone all day**. The other labels never warn. |
| **Currency** | The currency for all prices and costs. |
| **VAT %** | 0–100. Added to the energy price and the per-kWh fees (see *How cost is calculated*). |
| **Default zone** | The zone used for any hour that no other zone covers. |
| **Net metering (selling to the grid)** | Turns on the monthly kWh credit (see *Export valuation*). It's part of the plan: whatever it shows when you press **Save** is what the hub keeps. Set it to match your contract. |

### Taxes and charges

Three plan-level charges, each a rate per kWh, added to every zone's buy price before VAT:

- **Energy tax**
- **Grid fee**
- **Renewable levy**

There's no tax field on a zone: taxes and fees apply to the whole plan. The card also shows **Progressive: on/off**, read-only; a progressive tariff can't be set up in the app.

### Fixed charges

The hub doesn't include fixed (monthly or yearly) charges in any cost, so the editor doesn't show them. Costs and forecasts are energy-based only. Charges saved earlier stay in the plan untouched.

## Zones

A zone is a price that applies during a time window on chosen days of the week.

![Tariffs: the plan (name, currency, VAT, provider, plan type, default zone, net metering), «The week by zones» bar, one card per zone (buy price, type, hours, days), taxes and charges](https://www.rbgrid.io/assets/img/webapp-docs/wa-tariffs-editor.webp)

- **Up to seven zones.** **Add zone** picks a type that isn't in use yet, and is greyed out once the plan has seven zones (a Holiday zone kept from earlier counts). Each zone's **Zone type** list offers all seven types, so two zones can end up with the same type: keep one zone per type. The hub accepts two zones of the same type as they are: it doesn't merge or refuse them, and only notes a warning in its log. Where their hours overlap, priority decides between them. But the cost split by zone is kept by zone type, so the two zones count as one there.
- **One time window per zone**, combined with the days.
- Any hour no zone covers falls to the **Default zone**, so you don't have to cover all 24 hours yourself.

### Zone fields

| Field | What it does |
|---|---|
| **now** | A chip on the zone in force right now. |
| **On** | Turns the zone on or off without deleting it. A new zone starts switched on only if there's a free time slot for it. |
| **Zone type** | Night, Morning peak, Day, Evening peak, Late evening, Weekend or Critical peak. Sets the zone's priority (below) and, unless you renamed it, its name. It doesn't set the days. |
| **Buy price, <currency>/kWh** | The energy price in this zone, before taxes, fees and VAT. A new zone starts with the first zone's price. |
| **From / To** | The time window. It can cross midnight (22:00–06:00 works). From = To (for example 00:00–00:00) means the whole day: use it for whole-day zones, and not by accident. |
| **Days** | Monday to Sunday on/off. At least one day. |
| **Sell price, <currency>/kWh** | The value of one exported kWh in this zone. The field appears only with net metering on, and only on a zone that already has a sell price. A zone without one shows no field, and the editor can't add one. A new zone copies the sell price from the plan's other zones. |

The bin icon removes a zone after you confirm; the last zone can't be removed.

### Zone types and priority

When zones overlap, the hub applies the one with the highest-priority type:

1. Critical peak
2. Holiday
3. Weekend: on Saturday and Sunday only
4. Evening peak
5. Morning peak
6. Night
7. Late evening
8. Day

On Monday to Friday a Weekend zone ranks last, below Day, so any other zone that covers the same hours wins.

Notes:
- The type doesn't choose the days. A **Weekend** zone applies on the days you switch on under **Days**, like any other zone. For a Saturday-and-Sunday zone, switch on only those two days.
- **Holiday** dates can't be entered, so a Holiday zone never activates and the editor doesn't offer it. A Holiday zone saved earlier is kept.
- **Critical peak** is a fixed window like any other zone. It doesn't receive supplier-announced (day-ahead) events.

### Two windows on the same day

A zone has one time window. If your tariff has the same price in two separate windows of a day (for example a cheap night and a cheap midday), put the second window in a zone of another type with the same buy price.

### The week by zones

**The week by zones** shows which zone applies in every hour of the week. Below it, the app lists what it finds:

- **Hours without a zone.** With no default zone, an error: those hours aren't priced. With a default zone, an info line names the default zone that takes them.
- **Overlaps:** which zone applies in those hours, and which zones it ranks above. The priority order is shown as a reminder only when zones overlap.
- **A zone that never applies**, and the zone that overrides it. Change its hours or days.

Check it after every change.

## Export valuation

Exported energy is valued at a sell price:

- the zone's **Sell price**, if it has one;
- otherwise the plan's **selling ratio** (0–1, a share of the zone's buy price) × the buy price.

What the page shows for this:

- The **selling ratio** field is part of the net-metering block, so it shows only with net metering on, and then only while some enabled zone has no sell price of its own. When every enabled zone has its own price, the field is replaced by a note: "Every zone has its own selling price — the selling ratio is not used. Only zones without a price use it."
- A read-only **Selling price /kWh (calculated)** shows, with net metering on, only when no zone has a sell price and the plan has a default selling rate.
- The **Sell price** field shows on a zone only with net metering on, and only if that zone already has a sell price (see *Zone fields*). With net metering off, the editor doesn't show the zones' sell prices.

The hub values exports this way **whether net metering is on or off**. If your contract pays nothing for exported energy, the sell prices and the selling ratio must be 0. Watch for this after the setup wizard's quick tariff: its export price is written into every zone even though net metering stays off.

To check with net metering off: open **Economics** and look at the export value for a period with exports. If it isn't 0 and your contract pays nothing for exports, the zones carry sell prices you can't see in this version's editor. The way to zero them is the wizard's quick tariff with the export price left empty or 0: it writes 0 into every zone, but it rebuilds the whole plan, so re-enter your zones, fees and VAT afterwards (see the note at the top).

Don't switch **Net metering** on just to look at the prices. The toggle is saved with the plan: if it's on when you press **Save**, the kWh credit is on from then. If you switched it on and your contract has no net metering, switch it off again before **Save**. Sell prices you set stay as you set them.

Export value = exported kWh × sell price. No taxes, fees or VAT are applied to it, and it isn't subtracted from the cost: cost and export value are kept separately and shown side by side. Economics also shows **Net cost** (cost − export value) and **Net energy** (imported − exported).

Net metering **adds** a **kWh credit** on top: Economics shows a "Net metering — kWh credit" block with the credit balance, import and export this period, and credit earned and used. The credit is settled at each boundary of the local calendar month and doesn't change the money figures. It doesn't require a Main Supply point. Binding your supply as the **Main** point is still recommended, so the hub's totals describe your whole connection.

## How cost is calculated

For each measurement point and each reading:

```
cost = ( kWh × zone buy price  +  kWh × (grid fee + energy tax + renewable levy) ) × (1 + VAT% / 100)
```

The zone buy price is the rate of the zone in effect for that energy. The hub computes cost every 15 minutes at the zone in effect then, and stores it with the hourly records. On modules that count energy on board, such as STD modules, energy is costed once per 15-minute quarter, at the zone in effect for that quarter. Day, month and year costs are the sums of these. Nothing is prorated, and fixed charges aren't added (see *Fixed charges*). Months are calendar months.

### Example: a day/night plan

A plan with two zones, **Day** 07:00–23:00 at 0.30 and **Night** 23:00–07:00 at 0.15 per kWh, a **Grid fee** of 0.05 per kWh and **VAT** 20 %.

| Energy | Zone | Cost |
|---|---|---|
| 2 kWh at 14:00 | Day | (2 × 0.30 + 2 × 0.05) × 1.20 = **0.84** |
| 2 kWh at 02:00 | Night | (2 × 0.15 + 2 × 0.05) × 1.20 = **0.48** |

The same 2 kWh cost less at night because the zone's buy price is lower; the fee and VAT apply in both zones.

## Where you see cost

- **Economics:** cost, export value and net for a point and a period, and a split by zone since the hub's last restart. → [Economics](economics.md)
- **History:** the **Money** metric per point. → [History](history.md)
- **Overview:** cost per hour now, today, month to date, a month forecast and the **Tariff · today** ribbon. → [The Overview](dashboard.md)

## Backup

The tariff plan is part of the hub's **restore point** (**Settings → Restore point**): a copy of the hub's settings, points, tariffs, time and modules, kept on the hub. There's one restore point; a new one replaces the previous. It isn't a downloadable file, and there's no separate plan export.

## Troubleshooting

**The wrong zone applies**
- Check the hub's time and time zone (see [Time](time.md)). Until the hub's clock is set, cost may be priced in the wrong zone.
- When zones overlap, the higher-priority type wins (list above). A Weekend zone wins only on Saturday and Sunday.
- Check the zone's **Days** and that it's **On**.
- Open **The week by zones** to see the zone in effect for each hour, and which hours fall to the default zone.

**Cost is zero**
- Check that the zones have a buy price above 0.
- Check that the point has measured energy in that period.

**Export has no value, or a value you didn't expect**
- Exports are valued at the zones' sell prices or the selling ratio, with net metering on or off (see *Export valuation*).
- Check the sell prices and the ratio the page shows; set them to your contract's price, or 0. Leave **Net metering** as your contract has it.

**I can't save**
- See *Saving*: a change to save, a plan name, a default zone (or every hour covered), at least one day on each enabled zone, and no field errors.
