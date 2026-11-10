# Sensors & Units

RBgrid measures through two layers:

- **Sensors** — the physical channels the hub can see. In a v1.1.x hub these are the channels of the **rbAmp I²C modules**: a voltage input and one or more current channels per module. There is no built-in ADC; the modules measure and the hub reads them over I²C.
- **Units** — the measurement **points** you create by **binding** a channel. A unit has its own identity, name, role, statistics and history. Moving a unit to another channel (rebind) keeps its id, name, history and history slot.

You do not write any code to add a sensor or a unit. Channels appear on their own when a module is connected; you turn a channel into a unit from the web app or the REST API at runtime.

## Sensors — the physical channels

Every channel of a connected module is listed, whether or not it is in use. A channel that is not bound to a unit is simply **free**: it produces no data, is not summed and raises no alert. A module can be used with any subset of its channels, in any order.

Each channel carries:

- **`ch_label`** — the channel as printed on the module housing: `I0`, `I1`, … for current channels (numbered from 0), `V` for the module's voltage input, and `L1`–`L3` on a phase-aware module.
- **CT model** — for a current channel, the current-transformer model in use (`ct_code` / readable `ct_name`, e.g. `SCT-013-050, 50 A`; `ct_name` is left out when the model is unknown) and the module's limits for it. In the app it's the point's **Current sensor**. On a module that takes a clamp-on CT of your choice, you set the model (see *Choosing the CT model* below); on a module whose CT is paired with the channel at the factory, the model is fixed.
- **Correction status** — whether the module has applied its correction for the chosen CT model. If it has not (`readings not corrected`), the interface shows a warning and the reason (the channel is not calibrated · its gain does not match the CT model · the module does not know this CT model). Until the correction is applied, the readings may differ from the real ones.

The modules themselves come from the hub's module configuration; one module at the default address (`0x50`) is found without any setup (see [Get started](https://www.rbgrid.io/get-started)). The rbAmp module families, their channel make-up and CT lineup are documented with the modules — see the [rbAmp module docs](https://github.com/rb-amp/rbamp-docs).

## Units — the measurement points

A **unit** is created by binding one channel to a point. It carries:

| Field | What it is |
|---|---|
| `unit_id` | the unit's identity (a 4-digit hex id, e.g. `0006`, `0100`) |
| `point` | a stable slug used by external systems (MQTT, API) |
| `name` / `short_name` | display names |
| `role` | the circuit's role: `DEVICE_LOAD`, `ZONE_LOAD`, `CIRCUIT_BREAKER`, `RENEWABLE_SOURCE`, `STORAGE_SYSTEM`, `EV_CHARGING`, `HEATING_COOLING`, `SMART_APPLIANCE`, and the supply roles `MAIN_SUPPLY` and `ALTERNATIVE_SUPPLY` |
| `location` | where the circuit is |
| `stat_class` | kept for compatibility; the app always sends `1HOUR`, and it's the default if you leave it out. It doesn't decide whether a unit keeps history (see *Statistics*). |
| `rated_power_w` / `max_current_a` | the circuit's rated values (for the main supply: contracted power and the main breaker) |
| `phase` | which line the channel belongs to (see *Phase* below) |

In the app the roles are named Appliance, Load, Breaker, Generation, Storage, Charging station, Heating/cooling, Smart appliance, and for the supply roles Grid input and Backup input.

Two units are special **supply** units — the **main supply** (the grid connection) and an optional **alternative supply** (internal generation). The app never offers the supply roles when binding and doesn't let you change them, and the hub locks both: a patch that touches either supply role is refused (HTTP 409). They carry the site's economic tracking. The **voltage bus** (the module's `V` input) and the main input cannot be deleted.

## Configuring at runtime — web app or REST

Everything is configured on the running hub. Nothing is set in C code.

**Web app** — *Points → Free sensors → Bind* turns a free channel into a unit; the unit's fields are edited in its settings.

**REST** — the same operations are available over HTTP:

| Operation | Endpoint |
|---|---|
| List the physical channels | `GET /api/v1/sensors` (open unless a read token is set) |
| Bind a channel → new unit | `POST /api/v1/units` |
| Change a unit's parameters (live) | `POST /api/v1/units/{id}` |
| Move a unit to another channel | `POST /api/v1/units/{id}/rebind` |
| Change a unit's slug | `POST /api/v1/units/{id}/rename` |
| Remove a unit | `DELETE /api/v1/units/{id}` |

Write operations take the hub's **admin token** when one is set — set one with `POST /api/system/token` and send it on writes; this is recommended. Until you set a token, writes are open on the local network.

Binding a channel, for example:

```http
POST /api/v1/units
{
  "sensor": { "iface": "i2c", "bus": 0, "module": 0, "ch": 1 },
  "name": "Kitchen", "point": "kitchen",
  "role": "ZONE_LOAD", "stat_class": "1HOUR", "location": "ground floor"
}
```

The reply returns the new `unit_id`. New ids are allocated from `0100` upward. Renaming a point retires the old slug, so an external system does not silently pick up a different point under the same key.

Deleting (or unbinding) a unit **erases its stored history for good**; its channel becomes free again. The ID of a removed point can be reused on purpose: the app offers it when binding (**Bind with the previous ID**). The hub then starts the history afresh, but Home Assistant and MQTT continue the old series as one point.

Parameters such as `name`, `role`, `phase`, `rated_power_w`, `max_current_a` and the CT model are changed live with the patch endpoint and are saved across a reboot. The full field reference lives in the [units & sensors API](https://github.com/rb-amp/rbamp-docs).

### Choosing the CT model

On a channel whose module takes a clamp-on CT of your choice (SCT series), set the CT model to the clamp actually fitted: the point's **Current sensor** in its settings, or `ct_code` via the patch endpoint. The model is written into the module together with its type calibration, and kept bound to that module's identity, so a different module in the same slot keeps its own stored model.

**Change it only if a different clamp is really fitted.** Writing the model replaces the channel's factory calibration, and that calibration can't be restored. On a factory-paired channel (CT series) the model is fixed: the hub refuses the change. On an uncalibrated module the model can't be chosen.

## Phase

`phase` labels which supply line a channel belongs to. On a single-phase supply all channels use the default (`1P` / L1). **Single-phase and three-phase supplies are supported.** Single-phase power and energy are validated on hardware. Three-phase power and energy are checked on hardware too, with the three voltage inputs on one bench phase; real 120° three-phase mains has not been on the bench. A three-phase input is one STD-U3I3, and its module card is where you choose whether the input is metered as **one point with the phases netted** or as **one point per phase**, each phase with its own import and export. The supply systems validated on your hub are listed in `GET /api/v1/schema` → `enums.grid_system_validated`.

`phase` is only a label. The hub never multiplies or divides a unit's values by the number of phases: a unit with one current transformer reports what that one conductor carries, whatever its `phase` says. A unit set to **3 phases** on a three-phase load is the measured energy of **one** leg — do not divide or multiply it by three.

## Statistics

Each unit keeps 3-second and 15-minute data in RAM only; they have no stored history. Stored history starts at the hourly level: the hub keeps hourly records on its own flash, which survive a reboot, and builds days, weeks, months and years from them. This is the same for every unit, whatever its `stat_class`. History is kept for up to 12 units; units beyond twelve show live values only. A unit added while the hub runs starts its history after the next restart. Lifetime energy totals are kept separately from the history.

## When a channel is near its limit

A point flags its state on its tile on the Overview, in **Load lines**, in the points table and in **Needs attention**; the point's page shows an alert:

- **`clipping`** — the current has reached the sensor's clip point (its full-scale current, which is above its nominal rating). Above it the reading is **under-counted**, and the module cannot tell on its own. Point settings also warn in advance when the line breaker is above the sensor's limit, and History rows note the clipping.
- **`sensor limit`** — the current is near the sensor's clip current: the hub flags it at about 90 % of that current. For SCT-013 clamps the clip current is about 116 % of the rating (58 A on an SCT-013-050), so the warning comes at about 104 % of the rating (52 A). Up to the clip current, readings are correct. STD modules' own sensors carry no such flags; they read correctly to about 130 % of their rating.
- A **reversed** CT on a BASIC module: the point reads **sensor reversed — not metered**. A BASIC module meters forward flow only, so the load isn't metered until you turn the clamp round.

The event log gets one line when the state changes: "near its limit", "clipping" or "back within range". Only **clipping** marks the hour's record (a quality flag for over-range); "near its limit" is in the log only. See [The Overview](web-dashboard.md) and [Points](web-points.md) for where these appear.
