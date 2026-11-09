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
| `price` | Dynamic prices: state, the HA store and the resolved price per 15-min period (see "Dynamic prices") | |
| `time` | The hub's own time zone (tariff zones, local days, cheap windows): `{"type":"time","timezone","time_valid","now","local"}`. A fresh hub runs on `UTC0` until a zone is set | |
| `dynprice` | Price provider config, status and the provider catalogue (see "Dynamic prices") | |

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
| `{"cmd":"set","section":"price","data":{"buy":0.12,"sell":0.05,"from":unix,"until":unix}}` | Home Assistant price push for [`from`, `until`); `"prices":[...]` sets many at once, `{"clear":true}` drops them all. See "Dynamic prices" |
| `{"cmd":"set","section":"time","data":{"timezone":"CST-8"}}` | The hub's time zone, a POSIX TZ with an offset (`"CET-1CEST,M3.5.0,M10.5.0/3"`, `"<UTC+9>-9"`); saved and applied at once. Names like `Asia/Shanghai` are refused |
| `{"cmd":"set","section":"dynprice","data":{...}}` | Price providers (primary / backup), zone, contract formula. See "Dynamic prices" |

### Points: `{"cmd":"unit","op":...}`

These are the web app's point routes. The reply carries `data.code` (HTTP-style) and `data.reply`.

| Op | Body | Notes |
|---|---|---|
| `create` | `"data":{"sensor":{"iface":"i2c","bus":0,"module":0,"ch":0},"name":"Main input","role":"MAIN_SUPPLY"}` | 201 with `unit_id`, `point`. 409 if the sensor is already bound |
| `patch` | `"unit":"0100","data":{"name":..,"short_name":..,"role":..,"rated_power_w":..,"max_current_a":..,"phase":..,"group_id":..,"invert_sign":..,"inside":"0101"}` | Applies at once and persists. Supply roles are not patchable (409) |
| `rebind` | `"unit":"0100","data":{"module":0,"ch":1}` | Move a point to another channel |
| `rename` | `"unit":"0100","data":{"point":"kitchen"}` | Change the point slug |
| `delete` | `"unit":"0100"` | Its history stays on flash, unreachable |

### Dynamic prices: providers, `dynprice`, `price`

Design: `hello_world/documents/architecture/DYNAMIC_TARIFFS_DESIGN.md`. The hub owns the price loop. The user picks a
**primary** and an optional **backup** provider. For each 15-min period the hub uses a primary entry if one covers
the period's midpoint, else a backup entry, else the tariff plan's zone.

**Providers:**
- `energy_charts` (network): the hub fetches it itself, 15-min day-ahead, EUR/MWh → EUR/kWh, spot class. Only the
  CC BY 4.0 zones are offered: AT, BE, CH, CZ, DE-LU, DK1, DK2, FR, HU, IT-North, NL, NO2, PL, SE4, SI. It needs a plan
  in EUR (no FX in P1).
- `nordpool` (network, **unofficial** endpoint, P2): Nord Pool data portal. Nordic + Baltic areas SE1–4 (SEK),
  NO1–5 (NOK), DK1–2 (DKK), FI, EE, LV, LT (EUR). 15-min, /MWh → /kWh in the area's currency, spot. Tomorrow appears
  ~13:00 CET (HTTP 204 before).
- `elprisetjustnu` (network, P2): SE1–4, SEK/kWh excl. VAT, 15-min, spot. One file per day.
- `hvakosterstrommen` (network, P2): NO1–5, NOK/kWh excl. VAT, **hourly**, spot. One file per day.
- `energyzero` (network, P2): NL, 15-min, **all-in incl. VAT** (the plan's tax/VAT are not added).
- `octopus` (network, P2): UK Agile, GSP regions A–P, GBP, 30-min, **all-in incl. VAT**. Config `product` (default
  `AGILE-24-10-01`) and `export_product` (default `AGILE-OUTGOING-19-05-13`, `""` = none). The export rates become
  the entries' sell, so use `sell_mode: "source"` to apply them.
- `ha` (local): Home Assistant pushes with `set price`. Its class (`spot` / `all_in`) is set in the config.

The zone is shared by the primary and the backup network provider (e.g. `nordpool` + `elprisetjustnu` on SE3).
The plan's currency must equal the zone's currency for every network provider selected.

**`{"cmd":"get","section":"dynprice"}`** returns the config, the status and the provider catalogue:
```
{"type":"dynprice","enabled":false,"source":"ha","backup_source":"none","zone":"","country":"",
 "ha_class":"spot","factor":1,"markup":0,"zones_stack":false,"hourly_mean":false,"floor_buy":null,"floor_sell":null,
 "sell_mode":"source","k_sell":1,"feedin_fee":0,"token_set":false,"backup_token_set":false,"currency":"EUR",
 "price_state":"disabled","source_now":"plan","backup_now":false,"valid_until":0,"last_fetch":0,"next_fetch":0,
 "last_error":"none",
 "providers":[{"id":"energy_charts","name":"…","class":"spot","local":false,"available":true,"needs_token":false,
   "resolution_s":900,"zones":[{"id":"NL","name":"Netherlands","country":"NL","currency":"EUR"},…],
   "terms_url":"…","attribution":"Bundesnetzagentur | SMARD.de / Energy-Charts (CC BY 4.0)"},
  {"id":"ha","name":"Home Assistant","class":"spot","local":true,"available":true,…}]}
```
- `available` for `ha` is true in the ESPHome build, or once a push has arrived.
- `attribution` is shown verbatim wherever a price from that provider is shown.

**`{"cmd":"set","section":"dynprice","data":{…}}`** merges any of these fields:

| Field | Meaning |
|---|---|
| `enabled` | Off (default): only HA pushes apply, as a spot price with factor 1 and no markup (the behaviour before `dynprice`). On: the provider config below applies |
| `source`, `backup_source` | Provider ids; backup may be `"none"` and must differ from the source |
| `zone` | Zone id of the network provider(s). It must be one of the provider's zones, and its currency must equal the plan's currency |
| `ha_class` | `spot` \| `all_in` — what HA pushes |
| `factor`, `markup` | Spot → energy rate = spot × factor + markup (currency/kWh) |
| `zones_stack` | Add the plan zone's rate to the spot (time-of-use grid fees on top of spot) |
| `hourly_mean` | Bill each 15-min period at the mean of its hour's four 15-min prices |
| `floor_buy`, `floor_sell` | Lowest final buy / sell price; `null` = none (negative prices pass through) |
| `sell_mode` | `source` (the entry's sell, else the plan's) \| `plan` \| `formula` (raw × `k_sell` − `feedin_fee`) |
| `product`, `export_product` | Octopus product codes (import / export; export `""` = none) |

- **Final buy price.** spot class: (energy rate + the plan's per-kWh fees) × (1 + VAT). all_in class: the price as is.
  Then `floor_buy` applies. Negative values are real prices.
- **Errors** name the field: `{"result":"error","message":"…","field":"zone"}`. Through the ESPHome action the error
  text starts with `[field=zone] `. Over HTTP the same rejection is `400 {"error":"…","field":"zone"}`.

**`price_state`** (also the `Price state` entity):

| Code | Meaning |
|---|---|
| `ok` | The primary covers now |
| `backup` | The backup covers now |
| `today_only` | After 15:00 UTC, tomorrow is not in yet |
| `stale` | HA is the primary and nothing covers now |
| `plan_fallback` | A network primary and nothing covers now |
| `disabled` | Off |

- States follow coverage: a missed push or a failed fetch changes nothing while stored entries still cover now.
- **`last_error`:** `none | fetch_failed | http_status | parse | auth | no_data_for_zone | rate_limited`.

**Fetch schedule:**
- **Today:** whenever now is not covered.
- **Tomorrow:** from 11:00 UTC (= 12:00 CET / 13:00 CEST), every 15 min until 15:00 UTC, then hourly, until tomorrow's
  noon is covered. A failure retries on the same steps.
- **Persistence:** stored prices of every provider are saved to NVS (≤ 1 min after a change) and restored at boot, so
  a reboot keeps them.

**`{"cmd":"get","section":"price","from":unix?,"until":unix?}`** returns the state plus:
- `prices[]`: the HA store, the legacy shape `{from, until, buy, sell?}`;
- `resolved[]`: the price per 15-min period as the hub books it, `{from, until, buy, sell, raw?, prov, hourly_mean?}`.
  - `prov`: `source | backup | ha_push | plan`;
  - default range: now − 1 h → end of coverage; at most 200 periods.

**`{"cmd":"set","section":"price","data":{"prices":[{"buy":0.112,"sell":0.04,"from":…,"until":…},…]}}`** is the Home
Assistant push:
- `buy` and `sell` may be −100…100; `sell` missing or `null` = none.
- A pushed entry with the same `from` replaces the old one. Entries that ended over an hour ago drop. Up to 200 are
  kept (when full, the oldest start goes).
- Order does not matter, and pages add up. `{"clear":true}` drops them all.
- Errors: missing `buy`/`from`/`until`, values outside ±100, `until` ≤ `from`. Entries before the bad one stay stored.

**`{"cmd":"get","section":"price_log","from":unix?,"until":unix?}`** is the audit log of the price the hub applied to
each closed 15-min period:
- Reply: `{"type":"price_log","currency","cols":["from","buy","sell","prov"],"rows":[[…]],"more"}`; `buy` is all-in.
- `prov` is `source | backup | ha_push | plan`, with `+hourly_mean` appended when the hourly mean applied.
- Default range: the last 24 h; at most 400 rows per reply (`more`: ask again from the last `from` + 900).
- Kept 40 days, one file per UTC day on the config partition. The period in progress at a boot is not logged.

**Live state** without polling:
- Telemetry `system.tariff` (3 s, WS/MQTT) carries `dynamic`, `price_state`, `price_source`, `price_valid_until`, and
  `price_gen` (bumps on every change of stored prices or state); `buy_rate_all_in` / `sell_rate` are the price in
  force now.
- Each state change writes an event line `price: state A -> B (source …, valid until …)`, which reaches HA as
  `rbgrid_event` kind `price`. A failed fetch gives `FAIL price: …`.

**Cheap windows: `{"cmd":"get","section":"rules"}` / `{"cmd":"set","section":"rules","data":{…}}`.** The hub computes
and notifies; it controls nothing. Rules act on the all-in buy price of each 15-min period (dynamic price, or the plan
zone where none covers). Up to 8 rules; `set` with `"rules":[…]` replaces them all.

| `kind` | Fields | Active when |
|---|---|---|
| `threshold` | `on_below`, `off_above` (≥ on_below) | buy ≤ `on_below`; stays on until buy ≥ `off_above` |
| `cheapest` | `from_h` 0…23, `to_h` 0…24 (local; `to_h` ≤ `from_h` crosses midnight, equal = 24 h, 0…24 = the calendar day; `get` returns what was set), `periods` 1…96, `consecutive` | the current period is one of the `periods` cheapest of that day's window (or of the cheapest consecutive block) |
| `deadline` | `deadline_h` 0…23, `periods`, `consecutive` | the same, over the 24 h ending at `deadline_h` ("N periods of run time before 07:00") |

- Common fields: `id` (1…255), `name` (≤ 23 chars), `enabled` (default true).
- **`get rules`** returns each rule with `state: {active, active_until, next_start, window_from?, window_until?}`
  (UTC s).
- **Recompute:** windows are recomputed every ~30 s from the stored prices. Periods without a stored price count at the
  plan's rate, so a window can move when tomorrow's prices arrive.
- **Events:** each on/off writes `price: window 'NAME' on until T` / `off, next T`, which reaches HA as `rbgrid_event`
  kind `price`.
- **Signal flags:** `"signal":{"buy_cheap_below":x|null,"sell_low_below":y|null}`.

**Price notification: `{"cmd":"get","section":"signal"}`** returns the same JSON the hub publishes to MQTT
`rbgrid/<mac>/price` (retained, at each 15-min boundary and on change; web-app build with MQTT on):
```
{"v":1,"ts","period_from","period_until","currency","buy","sell","price_state","provenance",
 "rank_buy","rank_sell","periods_today","windows":[{"rule_id","name","active","active_until","next_start"}],
 "sell_low","buy_cheap"}
```
- `buy` / `sell` are all-in and may be negative.
- `rank_*` ranks the period within its local day, 1 = cheapest (92/100 on DST days); `periods_today` gives the count.
- `windows[]` ≤ 8; payload < 1 KB.
- Consumers (ACRouter, automations) map it to their own actions; when `price_state` ≠ `ok` or
  now > `period_until`, they fall back to their defaults.

**`{"cmd":"get","section":"peaks"}`** returns the import peaks at the grid input per billing cycle, the base for capacity
tariffs. It is metering only and changes no money.
- Reply: `{"type":"peaks","cycle_day","cycle_start","max_15min_w","max_15min_end","max_hour_w","max_hour_start",
  "top_days":[{"hour_start","w"}×3],"top3_days_mean_w","hour_start","hour_wh_so_far","prev":{…}}`.
- Every MAIN_SUPPLY point's import is summed per 15-min period (three phases = one peak).
- The cycle starts on `cycle_day` at local midnight.
- `{"cmd":"set","section":"peaks","data":{"cycle_day":15}}` changes the start day (1…28); `{"reset":true}` clears
  the figures.

**HTTP** (web-app build): the same functions.
- `GET`/`POST /api/v1/dynprice`;
- `GET /api/v1/price?from=&until=` (add `&log=1` for the price log);
- `POST /api/v1/price` (body as `data` above).

`GET /api/v1/schema` → `capabilities.dynamicTariffs: true`.

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
| External price | binary, diagnostic: a dynamic price covers now (Buy / Sell price then come from it) |
| Provider price | currency/kWh, measurement: the provider's raw price now (spot or all-in); unknown when none covers now |
| Price state, Price source | text, diagnostic: `price_state` code; provider in use now (`energy_charts`, `ha`, `plan`; "(backup)" appended) |
| Price valid until | timestamp, diagnostic: end of the continuous dynamic-price coverage from now |
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
| `kind` | `reverse_flow`, `module_absent`, `module_restored`, `module_poll_fail`, `bus_stuck`, `node_online`, `node_offline`, `boot`, `tariff`, `firmware`, `node_disabled`, `price` (dynamic price state change or fetch failure), `diag` (reboot-gap / period reconciliation detail); otherwise `fail` or `event` |
| `level` | `fail` (an invariant / error line, "FAIL …"), `debug` (a `diag` line that is not a failure: hide by default) or `event` |
| `message` | The log line as the device wrote it |

HA receives events only while it is connected; after a reconnect read the missed lines with `get events` (`since` = the last `seq` seen). `node_disabled` (a record from a node the user switched off, not counted) comes with level `event`.

Blueprints trigger on `event_type: esphome.rbgrid_event` with `event_data: {kind: …}`. Events from the device are not gated by the integration's "allow service calls" option. The device never calls HA actions.
