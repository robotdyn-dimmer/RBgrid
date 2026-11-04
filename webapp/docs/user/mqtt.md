# MQTT broker

The hub can publish its live measurements to an MQTT broker on your network and take commands from it. Home Assistant uses the same connection: the hub announces its sensors over MQTT discovery.

MQTT is off until you set a broker. The hub connects to it only when it's on your home Wi-Fi, not while it runs only its own access point.

## Connect the hub to a broker

In the app, open **Settings → Home Assistant** and fill in the **MQTT broker** form: the broker's address, port (usually 1883), login and password. Save and restart the hub when the app asks; the broker settings apply after a restart.

Over the API instead: `POST /api/system/mqtt-config` with `{"broker_uri":"mqtt://user:password@host:1883"}`. Reading it back shows the password masked.

Use a plain `mqtt://` address. Encrypted connections (`mqtts://`) aren't supported yet.

> **Keep the broker private.** Anyone who can read and write on the broker can read the hub's settings through the command channel, and an admin token sent with a command is visible to every client subscribed to it. Use a broker with its own users and access rules (ACL), on your own network: give the hub its own broker user, and limit access to `rbgrid/<mac>/#` to the hub and the clients you trust. Never use a public broker.

## Topics

`<mac>` is the hub's Wi-Fi MAC address: 12 lowercase hex digits, for example `a0b1c2d3e4f5`. The hub connects with the client id `rbgrid-<mac>`.

| Topic | Direction | QoS | Retained | What it carries |
|---|---|---|---|---|
| `rbgrid/<mac>/telemetry` | hub → broker | 0 | no | A live frame every **3 seconds** (JSON, frame version 2). |
| `rbgrid/<mac>/status` | hub → broker | 1 | yes | `online` when the hub connects. `offline` is the hub's last will: the broker publishes it when the hub drops (within about a minute). A planned restart publishes `offline` first. |
| `rbgrid/<mac>/cmd` | you → hub | 1 | — | Commands, as JSON (see *Commands*). |
| `rbgrid/<mac>/cmd/resp` | hub → broker | 1 | no | The answer to each command. |
| `homeassistant/sensor/rbgrid_<mac>_<key>/<metric>/config` | hub → broker | 0 | yes | Home Assistant discovery, re-sent on every connect. |

## The telemetry frame

Each frame is one JSON object. The main fields:

| Field | Meaning |
|---|---|
| `v` | Frame version (`2`). |
| `ts`, `sntp`, `time_src`, `sync_age_s` | Timestamps and where the hub's time came from. |
| `seq`, `boot_id` | Frame counter and an id that changes at each boot (to spot restarts and gaps). |
| `freq` | Mains frequency, Hz. |
| `net_pwr`, `net_dir` | Net power at the input, W, and its direction (import or export). |
| `system` | Hub totals: active and reactive power, energy in and out (lifetime and today), the current tariff (`zone`, buy and sell rate, currency), and the cost and export-value totals. `tariff.placeholder: true` means no tariff has been set yet. |
| `units` | One entry per measurement point, by id: current `i`, active power `p_act`, reactive power `p_rea`, power factor `pf`, direction, energy in and out (lifetime and today), and which module and channel it comes from. |
| `main_unit`, `vbus_unit` | The ids of the input point and the voltage bus. |

Energy values (`e_in`, `e_out`, `e_day_in`, `e_day_out`) are in **Wh**; money values (`cost_*`, `rev_*`) are in the tariff's currency. On modules that count energy on board (`"energy_mode": "on_device"`), the energy totals advance once per 15-minute quarter; power, current and voltage update every 3 seconds. Frames are sent only while the hub is connected. If the broker is down, live frames are dropped (nothing is buffered or replayed). The history stays on the hub and can be read later over HTTP or with the `history` command.

### Example

A real frame from a test hub, shortened to one point: a kitchen line drawing about 400 W. On the wire it's one compact line. The energy total is still small because this point counts energy on the module, and the quarter with the load hadn't closed yet.

```json
{
  "v": 2,
  "sntp": 1793756448,
  "tq": 1,
  "time_src": "sntp",
  "seq": 707,
  "boot_id": 1158837418,
  "freq": 50,
  "system": {
    "tariff": {
      "zone": "NIGHT",
      "buy_rate": 0.2,
      "sell_rate": 0.12,
      "buy_rate_all_in": 0.27104,
      "currency": "EUR",
      "placeholder": true
    },
    "units": 3,
    "present": 2
  },
  "units": {
    "0102": {
      "point": "kitchen",
      "v": 222.85,
      "i": 1.804,
      "p_act": 402.22,
      "p_rea": 0,
      "pf": 1,
      "dir": 1,
      "present": true,
      "module": 1,
      "ch": 0,
      "ch_label": "I0",
      "energy_mode": "on_device",
      "e_in": 0.0223,
      "e_day_in": 0.0223,
      "e_out": 0,
      "e_day_out": 0,
      "load_pct": 6,
      "type": "CURRENT_LOAD",
      "role": "DEVICE_LOAD"
    }
  },
  "main_unit": "0006",
  "vbus_unit": "0007"
}
```

`"placeholder": true` here means the test hub still had the factory example tariff.

## Home Assistant discovery

On every connect the hub publishes retained discovery messages, so Home Assistant creates the devices and sensors by itself. Each sensor reads its value from the telemetry topic and its availability from the status topic (a sensor goes *unavailable* about 30 seconds after the hub stops sending).

- **Each measurement point**: current (A), power (W), reactive power (var), power factor, energy in and energy out (Wh, lifetime and today).
- **The hub**: net power (W) and frequency (Hz).
- **The voltage bus**: voltage (V).

The energy sensors (`total_increasing`, Wh) go straight into the Energy dashboard; see [Integrations](https://www.rbgrid.io/integrations).

When you delete (or unbind) a point, the hub clears its discovery messages, and Home Assistant removes its entities. This works only while the hub is connected to the broker. If you delete a point while MQTT is off or the broker is down, its entities stay in Home Assistant as unavailable: remove the device by hand (**Settings → Devices & services → MQTT**, the device, **Delete**).

## Commands

Publish a JSON command to `rbgrid/<mac>/cmd`, with an `id` of your choice. The answer arrives on `rbgrid/<mac>/cmd/resp` with the same `id`. Several clients can share `cmd/resp`, so match answers by `id`.

**Read data:**

```json
{"cmd": "get", "section": "summary", "unit": "0x0100", "period": "day", "id": "q1"}
```

`section` can be `system`, `telemetry` (settings), `telemetry_data` (the live frame), `sensors`, `units`, `tariff`, `schema`, `summary` (with `unit`, `period` = `hour|day|week|month|year`, optional `from`/`to` as Unix time) or `economics` (with `unit`).

**Read history:**

```json
{"cmd": "history", "unit": "0x0100", "period": "hour", "type": "energy", "offset": 0, "limit": 50, "id": "h1"}
```

`type` is `electrical`, `energy`, `economics`, `quality` or `direction`; `limit` is up to 720. The answer is the same JSON as `GET /api/v1/history`.

**Set the tariff:**

```json
{"cmd": "set", "section": "tariff", "data": { "...": "the same body as the HTTP tariff call" }, "token": "<admin token>", "id": "t1"}
```

The `token` is needed once an admin token is set on the hub. Remember that it's visible to everyone subscribed to `cmd`.

A successful answer is the same JSON as the matching HTTP endpoint. An error looks like this:

```json
{"type": "cmd_ack", "data": {"cmd": "frobnicate", "result": "error", "message": "unknown cmd (use 'get' / 'set' / 'history')"}, "id": "q2"}
```

Identify hubs by `<mac>` in the topic: the `device_id` inside the answers is the same on every hub.

## Troubleshooting

**Nothing arrives on the broker**
- Check that the broker address, port, login and password are right, and that you restarted the hub after saving them.
- The hub must be on your home Wi-Fi; in access-point-only mode MQTT doesn't start.
- Check `rbgrid/<mac>/status`: `online` means the hub is connected.

**Home Assistant shows the sensors as unavailable**
- The hub isn't sending: check the status topic and the broker connection. Sensors come back as soon as frames arrive again.
