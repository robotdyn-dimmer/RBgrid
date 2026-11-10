# Hub API reference (firmware 1.1.8, API v1)

Generated from the hub's own `GET /api/v1/schema`. Do not edit by hand.

## Endpoints

| Path | Methods | Token | Restart to apply | Parameters |
|---|---|---|---|---|
| `/api/system/ap` | GET,POST | admin | no | GET: AP ssid/password (admin token, armed hub only); POST {password}: the user sets the AP password ("" = setup password) |
| `/api/system/backup` | GET,POST | admin | no |  |
| `/api/system/factory-reset` | POST | admin | yes | history(erase\|keep) |
| `/api/system/hardware-config` | GET,POST | admin | yes |  |
| `/api/system/lang` | GET,POST | admin | no | code (query, ISO 639-1); body=language pack JSON ≤ 200 KB; one pack kept; served at /lang/<code>.json |
| `/api/system/mqtt-ca` | GET,POST,DELETE | admin | yes | POST body = PEM (≤ 8192 B, one or more certificates); DELETE = back to the public CA bundle |
| `/api/system/mqtt-config` | GET,POST | admin | yes | broker_uri |
| `/api/system/ota` | POST | admin | yes | (binary) |
| `/api/system/provisioning` | GET | none | no |  |
| `/api/system/reboot` | POST | admin | no | delay_ms |
| `/api/system/restore` | POST | admin | yes |  |
| `/api/system/restore-system-config` | POST | admin | no | body = system-config JSON |
| `/api/system/security` | GET,POST | admin | no | dev_cors,origins |
| `/api/system/system-config` | GET,POST | admin | no |  |
| `/api/system/tariff-plan-config` | GET,POST | admin | no |  |
| `/api/system/telemetry-config` | GET,POST | admin | no | profile(lean\|full) |
| `/api/system/time-config` | GET | none | no | read-only time settings view (the app's time card) |
| `/api/system/time-sync` | GET,POST | admin | yes |  |
| `/api/system/token` | GET,POST | admin | no | token,role(admin\|read) |
| `/api/system/ui-commit` | POST | admin | no | force (query); body=ui-manifest.json |
| `/api/system/ui-upload` | POST | admin | no | path (query); body=raw file |
| `/api/system/units-config` | GET,POST | admin | no |  |
| `/api/system/version` | GET | none | no |  |
| `/api/system/wifi-config` | GET | none | no | current Wi-Fi settings (read-only view; setup goes through the hub's own Wi-Fi page) |
| `/api/telemetry/latest` | GET | none | no |  |
| `/api/v1/economics` | GET | none | no | unit |
| `/api/v1/espnow` | GET,POST | admin | no | action(pair\|add\|remove\|disable\|enable\|latch),mac,seconds,channels |
| `/api/v1/export.csv` | GET | none | no | unit,period,type,from,to |
| `/api/v1/history` | GET | none | no | unit_id,period,type(electrical\|energy\|economics\|all),offset,limit,from,to,since |
| `/api/v1/i2c` | GET | none | no | buses (pins, up), board pin profile, modules |
| `/api/v1/i2c/modules` | POST | admin | no | bus,addr: add a scanned rbAmp module (joins at once) |
| `/api/v1/i2c/modules/{m}/address` | POST | admin | no | addr: change the module's address (saved in the module, UID-verified) |
| `/api/v1/i2c/scan` | POST | admin | no | bus → devices answering: {addr, module\|null, kind, fw, uid} |
| `/api/v1/schema` | GET | none | no | this description |
| `/api/v1/sensors` | GET | none | no |  |
| `/api/v1/summary` | GET | none | no | unit,period,from,to |
| `/api/v1/system/events` | GET | none | no | since,limit(≤1000),level(all\|event\|fail) → {events[],boot,seq_last,next,more,persist,dropped,fail_boot,up,now,heap} |
| `/api/v1/units` | POST | admin | no | sensor,name,role,stat_class,point,reuse |
| `/api/v1/units/{hex}` | POST,DELETE | admin | no | name,short_name,phase,rated_power_w,max_current_a,role,group_id,role_trunk,invert_sign,charge_threshold_w,idle_timeout_min,ct_code (POST=params patch; invert_sign + ct_code apply live; ct_code = enums.ct_model, rbAmp current channels; EV overrides 0 = system.ev) |
| `/api/v1/units/{hex}/rebind` | POST | admin | no | module,ch |
| `/api/v1/units/{hex}/rename` | POST | admin | no | point,reuse |
| `/api/v1/ws-ticket` | POST | none | no | → {ticket,expires_s}; open /ws-telemetry?ticket=<ticket> once within expires_s |

## Transports

- **ws:** `/ws-telemetry (push + get/start/stop_telemetry)`
- **mqtt:** `rbgrid/<mac>/{telemetry,status,cmd,cmd/resp} + HA discovery`

## Quality bits (`q`)

| Bit | Name |
|---|---|
| 0 | `PARTIAL` |
| 1 | `TIME_UNSYNC` |
| 2 | `CLOCK_STEP` |
| 3 | `SRC_GAP` |
| 4 | `RECOMPUTED` |
| 5 | `ESTIMATED` |
| 6 | `UNSIGNED_SRC` |
| 7 | `OVERRANGE` |
| 8 | `MIGRATED` |
| 9 | `SRC_RESTART` |
| 10 | `SRC_FW` |
| 11 | `REVERSE_LOST` |
| 12 | `OUTAGE` |

## Capabilities

| Capability | This hub |
|---|---|
| `espnow` | true |
| `pointNesting` | true |
| `economics` | true |
| `summary` | true |
| `exportCsv` | true |
| `telemetryConfigPost` | true |
| `mqttConfig` | true |
| `adcUnboundDiscovery` | false |
| `historyTimeRange` | true |
| `auth` | true |
| `https` | true |
| `provisioning` | true |
| `configBackup` | true |
| `inputAggregate` | true |
| `wsTicket` | true |

## Units and money

- `energy`: Wh
- `power`: W
- `voltage`: V
- `current`: A
- `frequency`: Hz
- `money`: major
- `rate`: major_per_kwh
- money decimals: 2, rate decimals: 4 (currency = the tariff plan's; this hub: EUR)
- point keys: `HEX4` (four hex digits)

## Enumerations

- **role:** `MAIN_SUPPLY`, `ALTERNATIVE_SUPPLY`, `CIRCUIT_BREAKER`, `DEVICE_LOAD`, `ZONE_LOAD`, `RENEWABLE_SOURCE`, `STORAGE_SYSTEM`, `EV_CHARGING`, `HEATING_COOLING`, `SMART_APPLIANCE`
- **site_profile:** `home`, `industrial`, `ev`
- **ct_model:** `1` = SCT-013-005, 5 A, `2` = SCT-013-010, 10 A, `3` = SCT-013-030, 30 A, `4` = SCT-013-050, 50 A, `5` = SCT-013-100, 100 A, `6` = SCT-013-020, 20 A, `7` = SCT-013-060, 60 A
- **ct_model_ct:** `1` = Toroid 1:1000, 196 R burden, `2` = Toroid 1:2000, 370 R burden, `3` = Toroid 1:3000, 572 R burden, `4` = CT 2000:1, 92 R burden, `5` = Clamp 2000:1, 92 R burden, `6` = Clamp 2000:1, 1 A, `7` = Clamp 2000:1, 5 A, `8` = Clamp 2000:1, 10 A, `9` = Clamp 2000:1, 20 A, `10` = Clamp 2000:1, 30 A, `12` = Clamp 2000:1, 80 A
- **phase:** `0` = UNSET, `1` = L1, `2` = L2, `3` = L3, `4` = 3P, `5` = L1L2, `6` = L2L3, `7` = L3L1
- **grid_system:** `1P`, `SPLIT`, `3P`
- **grid_system_validated:** `1P`, `3P`
- **tariff_zone:** `NIGHT`, `MORNING_PEAK`, `DAY`, `EVENING_PEAK`, `LATE_EVENING`, `WEEKEND`, `HOLIDAY`, `CRITICAL_PEAK`
- **tariff_plan_type:** `SIMPLE`, `TWO_ZONE`, `THREE_ZONE`, `MULTI_ZONE`, `PROGRESSIVE`, `SEASONAL`, `DYNAMIC`, `CUSTOM`
- **tariff_condition_type:** `TIME_RANGE`, `WEEKDAY`, `WEEKEND`, `HOLIDAY`, `SEASON`, `MONTH`, `DATE_RANGE`, `SPECIAL_EVENT`

## Limits

- `short_name_bytes`: 24
- `name_bytes`: 31
- `origins_max`: 8

| Setting | Section | Min | Max |
|---|---|---|---|
| `logging_level` | system | 0 | 3 |
| `max_retry_attempts` | system | 0 | 255 |
| `retry_delay_ms` | system | 0 | 600000 |
| `ev.charge_threshold_w` | system | 0 | 100000 |
| `ev.idle_timeout_min` | system | 0 | 1440 |
| `ev.price_per_kwh` | system | 0 | 1000 |
| `adc.channels_count` | hardware | 0 | 8 |
| `adc.sampling_frequency` | hardware | 1000 | 50000 |
| `adc.adc_resolution` | hardware | 9 | 12 |
| `adc.adc_attenuation` | hardware | 0 | 3 |
| `network.nominal_voltage` | hardware | 90 | 480 |
| `network.nominal_frequency` | hardware | 45 | 65 |
| `grid.nominal_v` | system | 90 | 480 |
| `grid.nominal_hz` | system | 45 | 65 |
| `json_buffer_size` | telemetry | 1024 | 65536 |

## Live fields by source

- **rbamp:** `v`, `i`, `p_act`, `v_min`, `v_max`, `i_min`, `i_max`, `p_min`, `p_max`
- **espnow:** `v`, `i`, `p_act`
