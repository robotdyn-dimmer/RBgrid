# Points

## Overview

A **point** is one thing you measure: your supply, a circuit, an appliance. Each point is bound to a channel of an rbAmp module. The hub keeps its live values, its history, and its cost at your tariff.

There are three kinds of point:

- **Voltage bus**: the voltage measured by the module. It's the reference for every point's power.
- **Input (Main)**: your supply, the connection to the grid. It shows your whole connection: what you import, export and pay. (Cost is computed for every point.)
- **Regular points**: everything else, such as circuits and appliances.

The input and the voltage bus are created for you and can't be deleted: *"The main input and the voltage bus cannot be deleted."* By default the input is module 0, channel I0. The input role can't be changed from the app.

The modules themselves, and how to add one, are on the [Sensors and modules](web-sensors.md) page.

## The Points page

Open **Management → Points**. The page lists your rbAmp modules, your points and the free channels. At the top are **Add a module** (see [Adding modules](web-sensors.md#adding-modules)) and **Refresh**.

![Points: modules and bound points](https://www.rbgrid.io/assets/img/webapp-docs/wa-points-page.webp)

While the page loads you may see "Device is initializing — retrying…" (the hub has just started), or "Couldn't load sensors." with **Retry**. "No sensors detected." means the hub found no module.

### Bus warnings

The modules share one I2C bus. If something on it goes wrong, a warning appears at the top:

| Warning | What to do |
|---|---|
| **A module bus line is held low — no module can be read.** A module may be without power, set to another mode, or wired wrong. | Fix the cause (a module or cable holding the line). The warning clears within a few seconds, and the modules come back without a restart. |
| **The module bus is stuck — no readings for *n* s.** A module wired without power blocks the bus. | Check the module connections, then restart the hub with **Restart now**. |

### Sections

The page has these sections, in order:

1. **Remote nodes (ESP-NOW)**: on a hub that can run nodes this section sits at the top — the hub's radio for nodes and one card per paired node. A node's channels are points like any other (see [Remote nodes](web-nodes.md)).
2. **rbAmp modules (*n*)**: one card per module (see [Module cards](web-sensors.md#module-cards)).
3. **Points (*n*)**: your bound points, each as a live tile. Under each tile is its address, "module *m* · *channel* · I2C bus *b* · 0x*addr* · *model*" (for a three-phase input: "module *m* · L1–L3 (3 phases) · I2C bus *b*"), and an **Unbind** button.
4. **Free sensors (*n*)**: channels not bound to a point yet, each with a **Bind** button. A channel that the fitted module doesn't have isn't offered. When every channel is taken, the page says: "No free channels. To add a point, connect another rbAmp module or unbind a point you don't need."

### Unbinding a point

**Unbind** removes a point from its channel. The input, a backup input and the voltage bus have no **Unbind** button, and their role can't be changed from the app.

The app asks you to confirm. The point stops being metered and leaves the overview; the sensor channel stays free and can be bound again. **Unbind** and **Delete point** (on the point's page) are the same action: both remove the point and **erase its stored history for good**. If you may need the point's records later, download them first in [History](web-history.md#downloads) (CSV).

If the hub can't be reached, the app says "Couldn't unbind the point" or "Unbind request failed (device unreachable?)".

## Binding a point

Press **Bind** on a free sensor. The dialog **Bind sensor as a point** shows the sensor's address at the top ("Module *m* · *channel* · I2C bus *b* · 0x*addr*").

![Bind a free sensor as a point: name, point ID, role, phase, line rating, location](https://www.rbgrid.io/assets/img/webapp-docs/wa-points-bind-dialog.webp)

| Field | What it does |
|---|---|
| **Name** | Required. How the point appears everywhere in the app. Home Assistant uses it for the device name. |
| **Point ID (optional)** | Latin letters `a–z`, digits `0–9` and `_ . -`, up to 32 characters. Leave it empty and the hub makes one from the name. Home Assistant and MQTT know the point by this ID. |
| **IDs of removed points — can be reused** | Shown when the hub remembers IDs of removed points. Tap one to reuse it (see below). |
| **Role** | What the point is (below). |
| **Phase** | Which supply line the channel is measured against (below). |
| **Line rating, kW** | Optional. Used for the point's load bar. |
| **Location (optional)** | Where it is, for your own reference. |

At the bottom: "Home Assistant entities for the new point appear after the hub restarts."

### Roles

| Role | Use it for |
|---|---|
| **Breaker** | a panel group behind one breaker |
| **Appliance** (default) | a single line or appliance: kitchen, water heater, pump |
| **Load** | a whole room or floor |
| **Generation** | something that produces energy: solar panels, wind |
| **Storage** | a battery that charges and discharges |
| **Charging station** | an EV charger |
| **Heating/cooling** | heating, air conditioning, floor heating |
| **Smart appliance** | an appliance with its own controller |

**Generation** and **Storage** aren't offered on BASIC module channels. **Grid input** and **Backup input** aren't in this list: the input role can't be changed from the app.

#### Metering a generation point

**Fit the clamp exactly as you fit every other one: the arrow in the normal direction of flow, which on a generation circuit is from the inverter towards the house.** There is no reversed clamp, no special wiring and no software switch to set. The **role** does the rest: from firmware **v1.1.5** a Generation point counts flow in the arrow's direction — from the source towards the consumers — as **production**, so it reads negative power and its energy goes into **Energy out**.

> ⚠️ **Negative power on a generation point is correct — don't "fix" it.** It is what the role is for. Turning the clamp round to make the sign look positive is the one way to break a correctly fitted generation point: production then lands in **Energy in**, and *Produced* stays at zero.

Two things that do go wrong:

- **`invert_sign` left over from an older firmware.** Before v1.1.5 a Generation point needed it to read production at all. After updating, clear it (`POST /api/v1/units/<id> {"invert_sign": false}`) or the point reads consumption again: `invert_sign` still flips the result on top of the role.
- **The wrong kind of channel.** A BASIC module's channels count **consumption only** — they don't see export — which is why **Generation** and **Storage** aren't offered on them. Production needs a channel that measures direction. Which modules measure direction: [the rbAmp module guide](https://www.rbamp.com/products/configuration?utm_source=github&utm_medium=referral&utm_campaign=rbgrid).

### Phase

The phase sets how the channel's load is attributed to a line in per-line summaries. On a BASIC module every channel uses the module's one voltage input, whichever phase you pick.

The options follow your supply:

- **no mains system set yet**: not set, L1, L2, L3, **3 phases**;
- **single-phase**: not set, L1;
- **three-phase**: not set, L1, L2, L3, **3 phases**, **L1–L2 (two-pole)**, **L2–L3 (two-pole)**, **L3–L1 (two-pole)**.

An option marked "not validated on hardware" hasn't been checked on real hardware yet. A three-phase input is one STD-U3I3 module set with **Use as the three-phase input** on its module card (see [Sensors and modules](web-sensors.md#use-as-the-three-phase-input)).

### Reusing an ID

If you pick the ID of a removed point, the dialog warns that Home Assistant and MQTT know the point by its ID and will continue its old history as one point. If the channel now feeds a different load, give it a new ID. The button then reads **Bind with the previous ID**. From firmware **v1.1.6** the hub clears a removed point's net-metering record along with the point, so a reused ID starts its credit at zero; on **v1.1.5 and earlier** it inherited the removed point's credit bank and period balance.

### Messages

- "This point will show live values only: the hub keeps history for 12 points." — all 12 history slots are taken. The point still shows live values.
- "ID «*id*» is already used by another point." — choose another ID.
- "Could not bind" / "Bind request failed (device unreachable?)" — check the connection to the hub and try again.

A point you bind while the hub is running starts keeping history after the next restart. The hub stores every point's history by the hour and builds days, months and years from the hours, so there is no history setting to choose when you bind.

## The point's page

Tap a point tile to open its page.

![A point's page: live values, chart, counters](https://www.rbgrid.io/assets/img/webapp-docs/wa-points-detail.webp)

### Header

**Back**, the point's name, and a line "*role* · *source* · *status*". The source is **rbAmp** or **rbAmp BASIC**. The status is **normal**, **no data**, or **no such channel on the module**.

Chips under the header say what the point can measure:

- **measures import and export**, **consumption only**, or **consumption only (BASIC module)**;
- **current only — monitoring, not energy metering**: a current-only module;
- **energy kept across restarts**;
- **phase: *phase***.

### Alerts

When something needs attention, an alert appears under the header:

| Alert | What to do |
|---|---|
| The channel isn't on the fitted module | The point and its history are kept; data resumes when a module with this channel is back. Don't unbind the point while you wait. |
| The current sensor seems fitted backwards (BASIC module) | A BASIC module meters forward flow only, so this load is **not** being metered. Turn the sensor round on the wire, with the arrow on its body towards the load. |
| Readings not corrected for the current transformer | Values may differ from the real ones. The message points you at **Point settings → Current sensor**: set the clamp that is actually fitted. You can also check the model on the module card (see [Sensors and modules](web-sensors.md#module-cards)). On app v1.1.4 and earlier it pointed at the CT model on the module card instead. |
| Live values only | The point keeps no stored history (see [History](web-history.md#incomplete-periods-restarts-and-live-only-points)). |
| The current hits the sensor's limit | Everything above the limit is clipped and metering under-counts. Fit a sensor with a higher rating. |
| The load is near the sensor's limit | Above the limit the current is clipped and metering under-counts. |

### Live values

**Voltage** (on a three-phase point, the phase mean), **Current** (the sum of phases), **Power**, and, if the source measures them, **Reactive** and **PF**. Voltage more than 10 % away from nominal is marked **low** or **high** in red. Export shows as "↑ *W*" in the export colour.

A three-phase input adds a **Per phase (U3I3)** table: L1, L2, L3 with voltage, current, power, reactive power and PF.

### Trend

A small chart with **Power**, **Voltage** or **Energy**, by **Hours**, **Days** or **Weeks**. It shows the last 48 points for lines and the last 24 bars for energy. Generation points draw their output as positive. **As a table** opens the same data in [History](web-history.md#links-to-a-view).

### Counters

Not shown for current-only modules.

- **Imported today**, with "≈ *money*" at the day's average buy price;
- **Exported today** (or **Produced today** for a generation point), with its value, when above zero;
- **Total imported**;
- **Total exported** (or **Produced in total**), when above zero; not on BASIC modules.

### Charging station

A point with the **Charging station** role also shows **Current session**: **Status** and **Energy today**.

**Status is derived from the point's power**, on any channel — an ordinary rbAmp one included; no station controller is involved, and the label carries the tooltip "derived from power". It reads **Charging** while the point's power is above the charging threshold — the station's own, otherwise the hub's setting, otherwise 500 W — and **Available** below it. **Energy today** is the point's import today, and the hub doesn't keep a per-session charging history.

### Point setup

At the bottom, **Point setup** opens the point's settings (see [Point settings](#point-settings)), the **Technical details** (IDs and channel, useful for support) and **Delete point**.

**Delete point** asks you to confirm: the point stops being metered and leaves the overview and the points list, and its stored history is **erased for good** — from firmware **v1.1.6**, its net-metering record with it. The sensor channel becomes free and can be bound again. You then return to **Points**. If you may need the point's records later, download them first in [History](web-history.md#downloads) (CSV). The input and the voltage bus can't be deleted.

## Point settings

**Point settings** saves only the fields you changed. While you have unsaved edits, **Save** and **Cancel** stay at the bottom of the screen; if you try to leave, the app asks first. There is no separate edit mode.

![A point's page, «Point setup» opened: name, role, phase, line rating, breaker, current sensor, group](https://www.rbgrid.io/assets/img/webapp-docs/wa-points-settings.webp)

> **Fixed in v1.1.4:** on hub firmware v1.1.3 and earlier, on ESP32 chip revisions before v3, renaming a point could make it disappear after the next restart, together with its history. Update the hub to v1.1.4 before renaming points. To see your chip's revision, connect the board over USB and run `python -m esptool --chip esp32 -p <PORT> chip_id`: the output shows, for example, "ESP32-D0WD (revision v1.1)".

| Field | Shown for | What it does |
|---|---|---|
| **Name** | every point | Shows how much of the limit you've used. Warns if another point already has the same name. The hub sets the limit in bytes, so letters outside the Latin alphabet take more room. See the known issue below before renaming. |
| **Short name (for diagrams)** | every point | Used on the energy-flow diagram. Also limited in bytes. Empty = taken from the name. |
| **Role** | all but the voltage bus | Decides whether the point counts as generation, a charging station, and so on. The input's role can't be changed from the app. BASIC channels can't be Generation or Storage. |
| **Phase** | every point | The supply line (see [Phase](#phase)). |
| **Input limit, kW** (input) / **Line rating, kW** (others) | all but the voltage bus | The input's permitted power (the basis for "headroom"), or the line's rating for its load bar. |
| **Input breaker, A** / **Line breaker, A** | all but the voltage bus | The breaker's rated current. Empty = not set. |
| **Current sensor** | SCT channels | The clamp model on the channel, with its accurate range. See below. |
| **This station's «charging» threshold, W** / **idle after a session, min** | Charging station points, when the hub offers them | Empty or 0 uses the hub-wide values from [Settings → Charging stations](web-settings.md#charging-stations). |
| **Group** and **Group trunk meter** | regular points | The group is a tab on the energy-flow diagram; create groups in [Settings → Consumer groups](web-settings.md#consumer-groups). A trunk meter measures the whole group; the group's other points are its breakdown and don't add on top. |
| **Measured inside** | regular points, from firmware **v1.1.8** | Names the point that already measures this one — a charger inside a metered sub-panel. It must be an existing point other than this one, it can't be the input, and it can't make a loop. See *Points inside other points* below. |

### Points inside other points

From firmware **v1.1.8** a point can be marked as measured **inside** another one. Use it whenever one of your clamps already includes another: a sub-panel and a circuit fed from it, a trunk meter and a load behind it. The field says it in one line: *"If this load sits behind another point's meter (a sub-panel with its own meter, say), pick that point. Its power then counts once, in that point, and not again in the rest of the input."*

![Point settings for a car charger, with «Measured inside» set to «Garage sub-panel»](https://www.rbgrid.io/assets/img/webapp-docs/wa-points-inside.webp)

**What it changes, and what it doesn't:**

- the hub's own totals **don't** change — they are taken at the input, and they never depended on your points;
- the **unmetered** figure stops subtracting the inner load twice. Without the mark, the sub-panel and the load inside it are both counted against the input, so unmetered reads lower than it is, and on a well-metered board it can go negative. → [Economics](web-economics.md#balance-input-and-lines)

> ⚠️ **Mark the inner point, not the outer one.** The setting says "this point is already inside that one". Pointing it the other way describes a board you don't have.

Deleting a point — including by removing the node that carried it — makes the points inside it top-level again, so the figure corrects itself rather than silently keeping a parent that is gone.

Over the API this is `PATCH /api/v1/units/{id}` with `{"inside": <point id>}`; `0`, `""` or `null` clears it, and `units-config` reports `inside` for every point.

### Checks on limits and breakers

- Over 1000 kW: the app asks you to check the units (the field is in kW) and won't save.
- More than the breaker allows, or (for the input) well below it: the app warns and offers a matching value worked out from the breaker, the nominal voltage and the number of conductors.
- If the breaker is above the sensor's limit, the app warns that current above the limit is clipped and metering under-counts: fit a sensor with a higher rating. The limit is the current at which the clamp starts clipping, as the hub reports it — for SCT-013 clamps about 16 % above the rating, so roughly 11.6 A for a 10 A clamp — and it is the upper end of the "accurate … A" range shown on the module card and in Point settings. A clamp rated at the breaker doesn't warn. Where the hub reports no such figure there is no range and no warning, which is the case for an STD module's factory-paired sensors. On app v1.1.4 and earlier the check used a limit 10 % below the clamp's rating, so it warned even when the rating equalled the breaker.

### Current sensor

On an **SCT** channel, pick the clamp actually fitted. The new model applies at once and the module restarts; the hub doesn't. If the channel was factory-calibrated with your sensor, that calibration is lost and can't be restored, so change the model only if a different sensor is really fitted. On a **CT** series module, or on an uncalibrated module, the model can't be chosen. The supported models are listed under [Channel CT model](web-sensors.md#channel-ct-model).

### After saving

- "Point settings saved."
- After a role change: "Saved. The role applies immediately; history depth for the new role takes effect after the device reboots."
- "Not saved: …" with the reason. If the hub needs an admin token, the app says so.

## What the hub keeps for each point

Each record holds:

- **electrical values**: average voltage, current and power, reactive and apparent power, power factor, frequency, and the minimum and maximum of voltage, current and power;
- **energy**: imported and exported in the period, plus lifetime counters;
- **cost** and **export value** at your tariff;
- a **quality flag**: for example a partial period, an unsynced clock or a gap.

### How long history is kept

| Resolution | Kept for |
|---|---|
| Hour | 60 days |
| Day | about 13 months |
| Week | 8 weeks |
| Month | 3 years |
| Year | 10 years |

All of it is in the hub's own flash; no storage module is needed.

### How many points keep history

**Up to 12 points** keep history (the first twelve you added). Points beyond twelve show live values but keep no history.

A point you bind while the hub is running starts keeping history after the next restart: the app reminds you ("Restart the hub to start its history").

## Reading the data

- **History** shows a point by hour, day or month (see [History](web-history.md)). The trend on the point's page can also show weeks.
- **Economics** shows a point's cost and export value by period (see [Tariffs](web-tariffs.md)).
- The REST API and CSV export give you the same records (see [Source & API](https://www.rbgrid.io/source)).

## Troubleshooting

**A point shows no data**
- Check that the module is connected and its card shows in **Points → rbAmp modules** (see [Sensors and modules](web-sensors.md)).
- Check that the point is bound to the channel the clamp is on.
- If the page shows a bus warning, see [Bus warnings](#bus-warnings).

**A point's readings look wrong**
- Check the clamp model on SCT channels (**Current sensor**), and that the clamp is on its own channel.
- Check the clamp's direction against the arrow marking (see the rbAmp module docs).

**A point disappeared after a restart**
- If you had renamed it on hub firmware v1.1.3 or earlier: that's a known issue on ESP32 chip revisions before v3, fixed in v1.1.4 (see [Point settings](#point-settings)). Update the hub, then bind the channel again as a new point.

**A point has live values but no history**
- If you added it while the hub was running, restart the hub.
- It may be the thirteenth point or later: only twelve points keep history, the hub's own and its nodes' together.
- Check that the hub's clock is set (see [Time](web-time.md)). Records without a real date are skipped.

**The unmetered figure is negative, or far too small**
- One of your points is already measured by another one — a sub-panel and a circuit behind it. Mark the inner one as **measured inside** the outer (firmware v1.1.8, see *Points inside other points* above).
- A producing circuit bound with the wrong role lands in the wrong block: generation is not a line and is never subtracted. → [Economics](web-economics.md#balance-input-and-lines)

**A generation point reads consumption**
- On firmware v1.1.5 and later the role itself sets the direction; an `invert_sign` left over from an older firmware flips it back. Clear it (see *Roles*).

**A node's point lost its name, role or ratings after a restart**
- That was fixed in firmware **v1.1.8**: node points are rebuilt from the node list at boot, and until v1.1.8 their name, role, ratings, clamp orientation and **measured inside** were reset to defaults. On v1.1.7 and earlier, set them again after each restart — or update the hub. → [Remote nodes](web-nodes.md)

**I can't set «Measured inside»**
- The target must be an existing point, not this one, not the input, and the chain must not loop back. The field appears only from firmware v1.1.8.
