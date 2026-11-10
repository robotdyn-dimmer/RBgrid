# RBgrid for ESPHome

The RBgrid metering core as an ESPHome external component. It turns an ESP32 hub with rbAmp measurement modules into
an RBgrid hub that Home Assistant talks to over the ESPHome native API. Everything is metered, stored and priced on
the hub, as in the standalone RBgrid firmware:
- live power per circuit and 15-min periods;
- history on the hub's flash;
- the tariff engine, with dynamic prices and cheap windows;
- remote ESP-NOW nodes.

Home Assistant is the interface.

**Status:** 1.2.0-dev. Repository: https://github.com/rb-amp/rbgrid-esphome. Licence: MIT,
© Zhuhai RobotDyn Technology Co., Ltd. Home Assistant integration: https://github.com/rb-amp/rbgrid-ha.

## How it differs from the standalone firmware

| | Standalone RBgrid | RBgrid for ESPHome |
|---|---|---|
| Wi-Fi, OTA, clock (SNTP), logs | RBgrid | ESPHome |
| Web app, REST, WebSocket, MQTT | yes | no (not in this package) |
| Configuration | web app / REST / MQTT | the `rbgrid_cmd` action (Home Assistant panel) |
| Live values | telemetry | ESPHome entities |
| History, tariffs, dynamic prices, nodes | yes | yes, same core |

The metering core is the same code. The package leaves out RBgrid's own web server, WebSocket, MQTT, Wi-Fi manager and
the bench tools.

## Requirements

- **Hub:** **16 MB flash and PSRAM** (required: the core keeps its large tables in PSRAM). Tested:
  - ESP32-WROVER (quad PSRAM);
  - ESP32-S3 N16R8 (octal PSRAM).
- **Measurement:** rbAmp I²C modules (see [rbamp.com](https://www.rbamp.com/)), up to 8 on one bus.
  - Default bus pins: ESP32 **SDA 25 / SCL 26**; ESP32-S3 **SDA 8 / SCL 9**.
  - Remote modules can hang off ESP-NOW nodes (node firmware 1.5, see the RBgrid node README).
- **ESPHome 2026.5.3** or later, framework **ESP-IDF**.
- **Home Assistant 2026.1** or later: the action responses that `rbgrid_cmd` uses.

## Install

1. **Copy one of the example configurations** from `esphome/` in this repository next to your ESPHome configs:
   - `rbgrid-hub-c14.yaml`: ESP32-WROVER;
   - `rbgrid-hub-s3.yaml`: ESP32-S3 N16R8;
   - `rbgrid-hub-3p.yaml`: ESP32-S3 with a three-phase STD-U3I3 module.
2. **`secrets.yaml`:** `wifi_ssid`, `wifi_password`, and, optionally, an API encryption key and an OTA password.
3. **Point `external_components` at this repository** and keep `partitions_rbgrid_esphome.csv` beside the YAML:

   ```yaml
   external_components:
     - source:
         type: git
         url: https://github.com/rb-amp/rbgrid-esphome
         ref: v1.2.0
         path: esphome/components
       components: [rbgrid]
   ```

   The component builds the metering core from `components/` of the same checkout. For a local copy use
   `type: local` with `path: <checkout>/esphome/components`.
4. **First flash over USB:** `esphome run rbgrid-hub-s3.yaml`. After that, updates go over the air.
5. **In Home Assistant:**
   - the hub appears as an ESPHome device; add it with its encryption key if you set one;
   - then install **RBgrid** via HACS: custom repository `https://github.com/rb-amp/rbgrid-ha` (category
     Integration);
   - then Settings → Devices & services → Add integration → RBgrid. A hub on this component is also found
     automatically. Issues: https://github.com/rb-amp/rbgrid-ha/issues.
   - The integration sets the time zone and the currency and opens the RBgrid panel. Its own README covers the wizard,
     the Energy dashboard and prices from HA.

## Configuration (YAML)

Keep these blocks as in the examples. Each of them is needed:

```yaml
esphome:
  name: rbgrid-hub            # any name; the HA action becomes esphome.<name>_rbgrid_cmd
  project:
    name: rbgrid.hub          # must start with "rbgrid": the HA integration discovers hubs by it
    version: 1.2.0
esp32:
  flash_size: 16MB
  partitions: partitions_rbgrid_esphome.csv
  framework:
    type: esp-idf
    advanced:
      loop_task_stack_size: 16384   # rbgrid_cmd runs in the loop task; 8 KB (default) overflows
psram:                        # required
wifi:
  power_save_mode: none       # ESP-NOW nodes need the radio awake
  reboot_timeout: 0s          # a meter must not restart because Wi-Fi is down
api:
  reboot_timeout: 0s
  actions:
    - action: rbgrid_cmd      # the whole RBgrid configuration goes through this action
      variables: { command: string }
      then:
        - lambda: 'id(rb).run_command(command);'
        - api.respond:
            success: !lambda 'return id(rb).last_ok();'
            error_message: !lambda 'return id(rb).last_error();'
            data: !lambda 'id(rb).fill_reply(root);'
ota:
  - platform: esphome
time:
  - platform: sntp            # the system clock; RBgrid's tariff days use its own time zone (rbgrid_cmd "time")
```

**The `rbgrid:` block:**

| Option | Default | Meaning |
|---|---|---|
| `id` | — | used by the action above (`rb` in the examples) |
| `currency` | `EUR` | label of the money entities; must match the tariff plan's currency (HA does not convert) |
| `point_slots` | `12` | fixed number of point entity groups; points take free slots at runtime, no YAML change |
| `slot_current` / `slot_voltage` / `slot_cost` / `slot_online` | `true` / `false` / `false` / `true` | extra entities per slot |
| `slot_devices` | `false` | each slot under its own HA sub-device `rbgrid_point_<n>` (declare them under `esphome: devices:`, as in the examples) |
| `events` | `true` | send event-log lines to HA as `esphome.rbgrid_event` |
| `nvs_partition` | `rbnvs` | RBgrid's own NVS partition (ESPHome's NVS erase can't touch the energy anchors) |
| system entities | — | `power`, `energy_in`, `energy_out`, `energy_day_in`, `energy_day_out`, `cost_total`, `revenue_total`, `cost_day`, `revenue_day`, `buy_price`, `sell_price`, `provider_price`, `tariff_zone`, `price_state`, `price_source`, `price_valid_until`, `config_version`, `time_valid`, `tariff_placeholder`, `external_price`, `modules_total`, `modules_online`, `nodes_total`, `nodes_online` |

The component also sets the ESP-IDF options the core needs:
- RBgrid's own interfaces off;
- PSRAM allocation rules;
- TLS buffers in PSRAM, the full certificate bundle and certificate date checks;
- an IRAM-safe I²C interrupt and the rbAmp I²C timeout.

**Entity names:** keep the names of the examples:
- `Grid power`, `Energy in`, `Energy out`, `Cost`, `Export value`;
- the `… today` sensors;
- the per-point names under each point device (`Name`, `Power`, `Energy in`, `Energy out`, `Current`, `Voltage` with
  `slot_voltage`, `Cost`, `Online`);
- `Reset reason`, `Free heap`.

The Home Assistant integration finds them by name, for its Overview and the Energy dashboard. Change entity ids in HA
if you like, not the names in the YAML.

**Partitions** (`partitions_rbgrid_esphome.csv`, 16 MB):
- two 3 MB app slots;
- 4 MB `stats` (history);
- `rbnvs`, `logs` (the event log, kept across restarts) and `config`.

They sit at the same addresses as on the standalone hub.

## Using it

- **From Home Assistant:** the RBgrid panel covers points, sensors, modules, tariffs, prices, cheap windows, nodes and
  time.
- **Directly:** call the action `esphome.<name>_rbgrid_cmd` with a JSON command, e.g.
  `{"cmd":"get","section":"units"}`. Full reference: [`RBGRID_CMD.md`](RBGRID_CMD.md).
  - Errors come back as the action's error, prefixed `[field=…][code=…]`.
  - Replies over 4 KB come in parts.
  - Reply schemas: [`schemas/`](schemas/).
- **Events:** `esphome.rbgrid_event` with `kind`, `level`, `message`, `seq`. Use them for automations and blueprints.
  After a reconnect, read what was missed with `get events` from the last `seq`.
- **Energy dashboard:** `Energy in` / `Energy out` (lifetime, `total_increasing`) and `Cost` / `Export value`
  (lifetime, monetary `total`).
  - The `… today` sensors restart at local midnight; they are for display, not for statistics.

## Updating

- Over the air from ESPHome or HA. Before the restart the hub flushes the open hour and saves the lifetime totals, and
  the boot line reads "planned restart".
- A new image that resets within about a minute after booting is rolled back by ESPHome's safe mode. Let the hub run
  a minute before restarting it again.
- A stats-format migration, when an update carries one, waits until the new image is confirmed.

## Troubleshooting

- **Event log:** `{"cmd":"get","section":"events","level":"all"}`.
  - `FAIL …` lines name the problem.
  - Each boot logs its reset reason; a "NOT planned" software reset or a panic is worth reporting.
  - At start it logs `heap at hub start`.
- **Nodes read offline although powered and paired:** check `{"cmd":"get","section":"espnow"}` → `running` and
  `inject_age_ms`. `-1` means the hub's node-data task is not running; report it with the event log.
- **Modules not found:**
  - check the bus pins (above) and the module addresses (`get i2c`);
  - change modules with the hub off;
  - a module left unpowered on the bus blocks the whole bus.
- **The panel is empty, or "action not found":** check the `api: actions:` block and the `project: name: rbgrid…` line.

## Known limits

- One currency: the hub's tariff currency is the currency of every money entity.
- A point slot freed and taken by a new point continues that slot's lifetime entities. HA statistics can show a step
  there; prefer the per-point series the integration imports.
- The ESPHome build has no web app. To use the RBgrid web app, run the standalone firmware instead.
- The "factory" image (one file, ESPHome Dashboard → Manual download → Factory format) is not produced yet: the app
  sits at 0x20000 in this partition table, and ESPHome's merge step assumes 0x10000. Use `esphome run` (USB) for the
  first flash, then OTA.
