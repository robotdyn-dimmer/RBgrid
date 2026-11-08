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
  - The device keeps the whole reply until the next other command, or for 120 s after the last part was fetched.
  - A stale or unknown `rid` fails with "no such part … repeat the command".
  - Real sizes: `get schema` is 9.1 KB (3 parts), `get tariff` 2.4 KB, `get units` 1.3 KB per point or so, `history` grows with `limit`.

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
| `espnow` | `/api/v1/espnow`: nodes, pairing state | |

### Change

| Command | Does |
|---|---|
| `{"cmd":"set","section":"tariff","data":{...}}` | Apply + save a tariff plan (same body as the HTTP POST) |
| `{"cmd":"set","section":"units","data":{...}}` | Save the whole points config (applies after a restart) |
| `{"cmd":"set","section":"i2c","data":{"op":"scan","bus":0}}` | Devices answering on a bus |
| `{"cmd":"set","section":"i2c","data":{"op":"add","bus":0,"addr":80}}` | Add a scanned rbAmp module |
| `{"cmd":"set","section":"i2c","data":{"op":"address","module":0,"addr":81}}` | Change a module's address |
| `{"cmd":"set","section":"i2c","data":{"op":"pins","bus":0,"sda":8,"scl":9}}` | Bus pins (after a restart) |
| `{"cmd":"set","section":"espnow","data":{"action":"pair","seconds":180}}` | ESP-NOW actions (`pair`, `pair_cancel`, `configure`, `remove`, `disable`, `enable`, `latch`) |

### Points: `{"cmd":"unit","op":...}`

These are the web app's point routes. The reply carries `data.code` (HTTP-style) and `data.reply`.

| Op | Body | Notes |
|---|---|---|
| `create` | `"data":{"sensor":{"iface":"i2c","bus":0,"module":0,"ch":0},"name":"Main input","role":"MAIN_SUPPLY"}` | 201 with `unit_id`, `point`. 409 if the sensor is already bound |
| `patch` | `"unit":"0100","data":{"name":..,"short_name":..,"role":..,"rated_power_w":..,"max_current_a":..,"phase":..,"group_id":..,"invert_sign":..,"inside":"0101"}` | Applies at once and persists. Supply roles are not patchable (409) |
| `rebind` | `"unit":"0100","data":{"module":0,"ch":1}` | Move a point to another channel |
| `rename` | `"unit":"0100","data":{"point":"kitchen"}` | Change the point slug |
| `delete` | `"unit":"0100"` | Its history stays on flash, unreachable |

### History: `{"cmd":"history","unit":"0100","period":"hour|day|week|month|year","type":"electrical|energy|economics|quality|direction","offset":0,"limit":50}`

Same flat `{status, request, meta, data}` as `/api/v1/history`. `limit` ≤ 720.

The 15-minute period series and a `from`/`to` row format for HA charts (`[t, wh_in, wh_out, cost_in, cost_out, zone]`) are planned. The current call returns the HTTP format.

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
| Point N name / power / energy in / energy out / current (voltage optional) | 12 slots; a new point takes the first free slot. With `slot_devices: true` each slot is its own HA device `Point N` (declared under `esphome: devices:` as `rbgrid_point_N`), and its entity names are short: Name, Power, Energy in, Energy out, Current |

Energy in/out **today** also qualify in the Energy dashboard pickers; pick the lifetime Energy in/out there. The first adoption fixes HA's entity ids: ship `friendly_name` (and `slot_devices`) before a device is first added, or the ids stay mixed.

Energy steps at the 15-minute period boundaries: the module counters are the truth, and a period is booked when it closes. The money unit is fixed at build time (`currency:` in YAML) and must match the tariff plan's currency; the device logs a warning if they differ.

docs: none (internal; user docs when an ESPHome build ships)
