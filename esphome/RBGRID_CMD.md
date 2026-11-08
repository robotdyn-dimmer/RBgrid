# `rbgrid_cmd`: the RBgrid command action (ESPHome build)

**Status:** 1.2.0-dev, branch `dev/esphome`, 2026-11-08. Owner: rbgrid-dev:root. Checked on the ESP32-S3 test board over the native API.

The ESPHome build has no RBgrid web server. Everything the web app does through HTTP goes through one API action:

```
esphome.<device>_rbgrid_cmd   field: command (string, a JSON command)
```

The command format is the same as RBgrid's MQTT `cmd` channel (`rbgrid_command_dispatch` in the core). The device replies in the action response; Home Assistant needs 2026.1 or later to receive it.

## Reply

```json
{"ok": true, "schema": 1, "type": "<device reply type>", "config_version": "0a42dd62", "data": { ... }}
```

- `data` is the device reply as an object, the same JSON the standalone hub's HTTP endpoint returns.
- `config_version` is the CRC32 of the active points + tariff configuration, point names and settings included. The `Config version` entity carries the same value. It is recomputed at once after every changing command (`set`, `unit`), and every 30 s otherwise.
- **Errors:** the action fails (`success: false`), and the device message is the error text, so HA raises. A device error is:
  - an error acknowledgement (`data.result == "error"`);
  - a flat `{"status":"error"}`;
  - an HTTP-style code ≥ 400 from a point operation.

  The reply body still carries `ok: false` and the device reply.
- **Large replies come in parts.** ESPHome's JSON builder caps an action response at 5119 bytes (`components/json/json_util.cpp`, 2026.5.3); a longer reply arrived cut off. A device reply over 4000 bytes therefore goes out in base64 parts of 3072 bytes:

  ```json
  {"ok": true, "schema": 1, "config_version": "...", "rid": 40213, "parts": 3, "part": 0, "bytes": 9067, "b64": "eyJhcGlf..."}
  ```

  - Fetch parts 1…parts−1 with `{"cmd":"part","rid":40213,"part":1}`, concatenate the decoded bytes and parse the JSON.
  - The reassembled JSON is the device reply itself, i.e. what a plain reply carries in `data`. The wrapper fields
    (`ok`, `schema`, `config_version`, `type`) come with part 0; every part carries `rid`, `parts`, `part`, `bytes`.
  - The device keeps the whole reply until the next other command, or for 120 s after the last part was fetched.
  - A stale or unknown `rid` fails with "no such part … repeat the command".
  - Real sizes: `get schema` is 9.1 KB (3 parts), `get tariff` 2.4 KB, `get units` 1.3 KB per point or so, `history` grows with `limit`.

**Reply schemas:** `esphome/schemas/` (JSON Schema per reply, version = `schema`).

## Commands

### Read: `{"cmd":"get","section":S}`

| Section | Reply (same as the HTTP endpoint) | Extra fields |
|---|---|---|
| `schema` | `/api/v1/schema`: enums, capabilities, quality bits | |
| `sensors` | `/api/v1/sensors`: modules, channels, CT models, which point each is bound to | |
| `units` | `/api/system/units-config`: every point with its settings (role, ratings, `inside`, …) | |
| `telemetry_data` | `/api/telemetry/latest`: live frame (system, tariff now, points) | |
| `tariff` | `/api/system/tariff-plan-config`: the tariff plan | |
| `system` | `/api/system/system-config` | |
| `economics` | `/api/v1/economics` | `"unit":"0100"` |
| `summary` | `/api/v1/summary` | `"unit"`, `"period"` (hour/day/month…), `"from"`, `"to"` (unix) |
| `i2c` | `/api/v1/i2c`: buses, board pin profile, modules | |
| `espnow` | `/api/v1/espnow`: nodes, pairing state. `nodes_total` / `nodes_online` = paired nodes / online now; `paired_count` = nodes paired in the current pairing window (a restart gives them their points) | |
| `events` | The device's event log since a cursor: `"since":seq` (0 = oldest in RAM), `"limit"` ≤ 100 (default 50), `"level"` `event` (default: events + fails) \| `fail` \| `all`. Reply `{"type":"events","boot","first","next","more","lines":[{"seq","ts","up","lvl","msg"}]}`; pass `next` as the next `since`. This boot's RAM ring only. Use it after a reconnect to pick up what fired while HA was away (boot lines go out before HA connects) | |
| `price` | External prices stored (see below): `{"type":"price","now","active","prices":[{"from","until","buy","sell"?}]}` | |

### Change

| Command | Does |
|---|---|
| `{"cmd":"set","section":"tariff","data":{...}}` | Apply + save a tariff plan. `data` = the whole `get tariff` reply, or `{"list":[plan]}`, or one plan object (as in `list[0]`). The plan is merged into the running one: fields left out keep their values, zones and fixed payments are rebuilt from the body |
| `{"cmd":"set","section":"units","data":{...}}` | Save the whole points config (applies after a restart) |
| `{"cmd":"set","section":"i2c","data":{"op":"scan","bus":0}}` | Devices answering on a bus |
| `{"cmd":"set","section":"i2c","data":{"op":"add","bus":0,"addr":80}}` | Add a scanned rbAmp module |
| `{"cmd":"set","section":"i2c","data":{"op":"address","module":0,"addr":81}}` | Change a module's address |
| `{"cmd":"set","section":"i2c","data":{"op":"pins","bus":0,"sda":8,"scl":9}}` | Bus pins (after a restart) |
| `{"cmd":"set","section":"i2c","data":{"op":"pause","module":0,"seconds":120}}` | Test only: stop polling a module for up to 3600 s (0 = resume). A poll hole: `Modules online` drops, the event log gets "poll paused / resumed (test)"; no `module_absent` (a real unplug raises that) |
| `{"cmd":"set","section":"espnow","data":{"action":"pair","seconds":180}}` | ESP-NOW actions (`pair`, `pair_cancel`, `configure`, `remove`, `disable`, `enable`, `latch`) |
| `{"cmd":"set","section":"espnow","data":{"action":"configure","mac":"E8:DB:84:3D:42:E8","config":{"period_s":900,"channels":[{"ch":5,"enabled":false}]}}}` | A node's input settings. `channels[]`: `ch` (the node's channel id, as `get espnow` lists it), optional `enabled`, optional `ct_code` 1…7; only the listed channels change. `period_s` is optional and must be 900. `channels` may also sit next to `action` without `config`. At least one channel must stay on. Applies when the node acknowledges (`config_state` pending → applied in `get espnow`) |
| `{"cmd":"set","section":"espnow","data":{"action":"disable","mac":"…"}}` | `disable` / `enable` / `remove` a node by `mac`; `pair` opens the window for `seconds`; `latch` closes the current period on every node now |
| `{"cmd":"set","section":"price","data":{"buy":0.12,"sell":0.05,"from":unix,"until":unix}}` | External energy price for [`from`, `until`); `"prices":[...]` sets many at once, `{"clear":true}` drops them all. See below |

### Points: `{"cmd":"unit","op":...}`

These are the web app's point routes. The reply carries `data.code` (HTTP-style) and `data.reply`.

| Op | Body | Notes |
|---|---|---|
| `create` | `"data":{"sensor":{"iface":"i2c","bus":0,"module":0,"ch":0},"name":"Main input","role":"MAIN_SUPPLY"}` | 201 with `unit_id`, `point`. 409 if the sensor is already bound |
| `patch` | `"unit":"0100","data":{"name":..,"short_name":..,"role":..,"rated_power_w":..,"max_current_a":..,"phase":..,"group_id":..,"invert_sign":..,"inside":"0101"}` | Applies at once and persists. Supply roles are not patchable (409) |
| `rebind` | `"unit":"0100","data":{"module":0,"ch":1}` | Move a point to another channel |
| `rename` | `"unit":"0100","data":{"point":"kitchen"}` | Change the point slug |
| `delete` | `"unit":"0100"` | Its history stays on flash, unreachable |

### External price (Nord Pool, Tibber, … through HA)

`{"cmd":"set","section":"price","data":{"prices":[{"buy":0.112,"sell":0.04,"from":1794139200,"until":1794142800}, ...]}}`

- `buy` replaces the tariff zone's **energy rate** (currency/kWh, 0…100) for [`from`, `until`). The plan's per-kWh fees
  (grid usage, energy tax, levy) and VAT are still added, so the `Buy price` entity shows the all-in rate. `sell`
  (optional) replaces the feed-in rate; leave it out to keep the plan's.
- A 15-min period is priced at its midpoint, like the zones. Energy is booked when the period closes, so a price
  must cover the period's middle by then; sending the day-ahead prices once a day is the intended use.
- An entry with the same `from` is replaced; entries that ended more than a day before a new one are dropped; up to 48
  are kept (the oldest start goes first).
- **Outside every entry the tariff plan's zone rates apply** (the fixed tariff is the fallback): if HA stops sending,
  money keeps accruing at the plan. The `External price` entity is on while an entry covers now.
- Prices live in RAM: after a restart, send them again (HA can re-send on the device's reconnect).
- Errors: missing `buy`/`from`/`until`, `buy` outside 0…100, `until` ≤ `from`. Entries before the bad one stay stored.

### History: `{"cmd":"history","unit":"0100","period":"hour|day|week|month|year","type":"electrical|energy|economics|quality|direction","offset":0,"limit":50}`

Same flat `{status, request, meta, data}` as `/api/v1/history`. `limit` ≤ 720.

### Chart series: `{"cmd":"series","unit":"0100","period":"hour|day|week|month|year","from":unix,"to":unix,"limit":168}`

Energy and money per stored period, joined on the device:

```json
{"type":"series","unit":"0100","period":"hour","currency":"EUR",
 "cols":["ts","dur","wh_in","wh_out","cost","revenue","q"],"rows":[[1794124800,3600,12.3,0,0.0048,0,1], ...]}
```

- `ts` is the **end** of the covered interval (unix), the history contract's rule. `dur` is the seconds it covers, so the interval is [`ts − dur`, `ts`). A full hour row stamped 09:00 is 08:00–09:00. A short row (e.g. `dur` 267 stamped 08:00) is the part of an hour after a boot.
- `cost` / `revenue` are stored in minor units (cents) in flash, so a stored row is exact to 0.01 of the currency. Finer money is only live (entities).
- `q` holds the quality bits; `get schema` → `quality_bits` names them:

  | Bit (value) | Name |
  |---|---|
  | 0 (1) | PARTIAL |
  | 1 (2) | TIME_UNSYNC |
  | 2 (4) | CLOCK_STEP |
  | 3 (8) | SRC_GAP |
  | 4 (16) | RECOMPUTED |
  | 5 (32) | ESTIMATED |
  | 6 (64) | UNSIGNED_SRC — a BASIC module (consumption only) |
  | 7 (128) | OVERRANGE |
  | 8 (256) | MIGRATED |
  | 9 (512) | SRC_RESTART |
  | 10 (1024) | SRC_FW |
  | 11 (2048) | REVERSE_LOST |
  | 12 (4096) | OUTAGE |
- Rows come newest first, filtered to [`from`, `to`).
- **Resolution:** the hour is the finest stored period. 15-minute periods are live only and are not kept in flash.
- About 40 bytes per row; more than ~90 rows arrive in parts.

## Entities (S3 test YAML, `esphome/rbgrid-hub-s3.yaml`)

| Entity | Class |
|---|---|
| Grid power | W, power, measurement |
| Energy in / out | Wh, energy, total_increasing (lifetime, grid boundary) |
| Energy in / out today | Wh, energy, total_increasing (restarts at midnight) |
| Cost, Export value | currency, monetary, **total** (lifetime) |
| Cost today, Export value today | currency, total_increasing, no device class (ESPHome has no `last_reset`) |
| Buy price (all-in: fees + VAT), Sell price | currency/kWh, measurement; rounded to 5 decimals on the device (float32 transport noise remains below that) |
| Tariff zone | text |
| Config version | text, diagnostic |
| Clock set, Demo tariff | binary, diagnostic |
| External price | binary, diagnostic: an external price covers now |
| Modules, Modules online | count, diagnostic: configured rbAmp modules / those that answer (fewer than 3 failed polls in a row) |
| Nodes, Nodes online | count, diagnostic: paired ESP-NOW nodes / those online (a disabled node is not online) |
| Point N name / power / energy in / energy out / current (voltage, cost optional: `slot_voltage`, `slot_cost`; cost = monetary, total; `Online` = connectivity, diagnostic: the point's source answers, unknown on an empty slot, `slot_online` default on) | 12 slots; a new point takes the first free slot. With `slot_devices: true` each slot is its own HA device `Point N` (declared under `esphome: devices:` as `rbgrid_point_N`), and its entity names are short: Name, Power, Energy in, Energy out, Current |

A slot is released (Name "", values unknown) when its point is deleted, or when the user switches the point off: the whole node disabled, or that input disabled in the node settings (`set espnow configure`). Switching it back on gives the point the first free slot again, which may be a different one. A point whose source is merely absent (module or node silent) keeps its slot and name; only its live values go unknown.

Energy in/out **today** also qualify in the Energy dashboard pickers; pick the lifetime Energy in/out there. The first adoption fixes HA's entity ids: ship `friendly_name` (and `slot_devices`) before a device is first added, or the ids stay mixed.

Energy steps at the 15-minute period boundaries: the module counters are the truth, and a period is booked when it closes. The money unit is fixed at build time (`currency:` in YAML) and must match the tariff plan's currency; the device logs a warning if they differ.

docs: none (internal; user docs when an ESPHome build ships)

## Events: `esphome.rbgrid_event`

Every line of the device's event log, except the periodic trace, is sent to HA as an event while HA is connected (`events: true`, the default). The device keeps the line in its own log either way. Data:

| Field | Value |
|---|---|
| `kind` | `reverse_flow`, `module_absent`, `module_restored`, `module_poll_fail`, `bus_stuck`, `node_online`, `node_offline`, `boot`, `tariff`, `firmware`, `node_disabled`, `diag` (reboot-gap / period reconciliation detail); otherwise `fail` or `event` |
| `level` | `fail` (an invariant / error line, "FAIL …"), `debug` (a `diag` line that is not a failure: hide by default) or `event` |
| `message` | The log line as the device wrote it |

HA receives events only while it is connected; after a reconnect read the missed lines with `get events` (`since` = the last `seq` seen). `node_disabled` (a record from a node the user switched off, not counted) comes with level `event`.

Blueprints trigger on `event_type: esphome.rbgrid_event` with `event_data: {kind: …}`. Events from the device are not gated by the integration's "allow service calls" option. The device never calls HA actions.
