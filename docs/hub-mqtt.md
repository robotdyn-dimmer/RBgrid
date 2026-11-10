# MQTT broker

The hub can publish its live measurements to an MQTT broker on your network and take commands from it. Home Assistant uses the same connection: the hub announces its sensors over MQTT discovery.

MQTT is off until you set a broker. The hub connects to it only when it's on your home Wi-Fi, not while it runs only its own access point.

## Connect the hub to a broker

In the app, open **Settings → Home Assistant** and fill in the **MQTT broker** form: the broker's address, port (usually 1883), login and password. Save and restart the hub when the app asks; the broker settings apply after a restart.

Over the API instead: `POST /api/system/mqtt-config` with `{"broker_uri":"mqtt://user:password@host:1883"}`. Reading it back shows the password masked.

Use a plain `mqtt://` address, or `mqtts://` for a secure connection:

- **from v1.1.7**, with a certificate from a **public** authority — one on the Mozilla root list, such as Let's Encrypt or DigiCert — that matches the broker's host name;
- **from v1.1.8**, also with **your own** certificate authority: a home broker with a private CA, or a self-signed certificate, which covers Mosquitto inside Home Assistant with its own certificate. See *Your own broker CA* below.

On **v1.1.7** a private or self-signed certificate could not be used at all, and such a broker needed a plain `mqtt://` address on a network you trust.

### Your own broker CA

**Settings → Home Assistant** has a **Broker CA (for mqtts)** card. While nothing is uploaded it reads *"None uploaded: the hub trusts public certificate authorities only."* **Upload CA certificate** takes the PEM file of your authority — the file that begins with `-----BEGIN CERTIFICATE-----` — and the hub answers *"Certificate saved on the hub. It is used after a restart."* The card then lists what it holds: subject, issuer and the validity dates.

The hub refuses a file that isn't right, and says which:

| What you get | What to do |
|---|---|
| *"This isn't a PEM certificate. Choose the file that starts with «-----BEGIN CERTIFICATE-----»."* | export the CA in PEM, not DER or PKCS#12 |
| *"The file holds a private key. Never upload a key: choose the file with the CA certificate only."* | upload the certificate alone — the key never leaves your broker |
| *"The file is larger than 8 KB. Upload only the CA certificate."* | the limit is 8 KB; a chain of a few certificates fits, a whole bundle does not |
| *"not a CA certificate"* | the file is a server certificate, not the authority that signed it |

> ⚠️ **Removing the CA cuts off a broker that depends on it.** **Remove the broker CA?** warns that *"After a restart the hub connects only to brokers with a public certificate. A broker that uses this CA stops connecting."* After it: *"Certificate removed. After a restart the hub trusts public certificate authorities only."*

The uploaded CA **replaces** the public bundle rather than adding to it, and it applies at the next restart. Over the API: `POST /api/system/mqtt-ca` with the PEM, `GET` to see what is stored, `DELETE` to go back to the public bundle.

### Why a secure connection was refused

From **v1.1.8** the hub checks the certificate's **dates** as well as its authority and host name, and says what it found. The card shows one of these:

| What the app says | What it means |
|---|---|
| *"Secure connection to the broker: certificate verified."* | working |
| *"Connecting to the broker over TLS…"* | in progress, or back off after the broker dropped the connection |
| *"The broker's certificate has expired. Renew it on the broker."* | past its validity |
| *"The broker's certificate isn't valid yet. Check the hub's time, or the certificate's start date."* | not valid yet — and the hub's own clock is the usual culprit |
| *"The certificate is for another host name. Use the broker address the certificate was issued for."* | the address you typed isn't the one in the certificate |
| *"The hub doesn't know this certificate's authority. Upload your broker's CA below."* | a private CA, not yet uploaded |
| *"Waiting for the clock: the hub checks the broker's certificate once it knows the time."* | the hub has no time yet, see below |
| *"The secure connection to the broker failed. Check the port (usually 8883) and that the broker speaks TLS."* | TLS didn't start at all |

> ⚠️ **A hub without a clock will not open a TLS connection at all, and that is deliberate.** A clock that isn't set makes every certificate look "not valid yet", so from v1.1.8 the hub waits until it has the time — by NTP or set by hand — before it tries. The event log says *"mqtt: TLS broker — waiting for the clock"*. A plain `mqtt://` broker is **not** delayed. → [Time](web-time.md)

Each refusal also writes a line to the event log. Over the API the same state is in `GET /api/system/mqtt-config` → `tls: {state, ca, ca_uploaded}`, where `state` is `ok`, `expired`, `not_yet_valid`, `name_mismatch`, `unknown_ca`, `waiting_clock`, `connecting`, `tls_error`, or `off` for a plain connection, and `ca` is `custom` or `bundle`.

If the hub can't verify the broker it keeps retrying, and nothing else stops working: **Settings → MQTT** shows MQTT as not connected, and the hub's event log names the reason. → [Events](web-events.md#hub-log)

> **Keep the broker private.** Anyone who can read and write on the broker can read the hub's settings through the command channel, and an admin token sent with a command is visible to every client subscribed to it. Use a broker with its own users and access rules (ACL), on your own network: give the hub its own broker user, and limit access to `rbgrid/<mac>/#` to the hub and the clients you trust. Never use a public broker.

## Topics

`<mac>` is the hub's Wi-Fi MAC address: 12 lowercase hex digits, for example `a0b1c2d3e4f5`. The hub connects with the client id `rbgrid-<mac>`.

| Topic | Direction | QoS | Retained | What it carries |
|---|---|---|---|---|
| `rbgrid/<mac>/telemetry` | hub → broker | 0 | no | A live frame every **3 seconds** (JSON, frame version 2). |
| `rbgrid/<mac>/status` | hub → broker | 1 | yes | `online` when the hub connects. `offline` is the hub's last will: the broker publishes it when the hub drops (within about a minute). A planned restart publishes `offline` first. |
| `rbgrid/<mac>/cmd` | you → hub | 1 | — | Commands, as JSON (see *Commands*). |
| `rbgrid/<mac>/cmd/resp` | hub → broker | 1 | no | The answer to each command. |
| `rbgrid/<mac>/node/<NODEMAC>/status` | hub → broker | 1 | yes | One per remote node: `online`, signal (dBm), lost packets (%) and last heard, refreshed every minute (see [Remote nodes](web-nodes.md)). |
| `rbgrid/<mac>/event` | hub → broker | 0 | no | Node events: `paired`, `removed`, `node_online`, `node_offline`. |
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

Energy values (`e_in`, `e_out`, `e_day_in`, `e_day_out`) are in **Wh**; money values (`cost_*`, `rev_*`) are in the tariff's currency. On modules that count energy on board (`"energy_mode": "on_device"`), the energy totals advance once per 15-minute period; power, current and voltage update every 3 seconds. Frames are sent only while the hub is connected. If the broker is down, live frames are dropped (nothing is buffered or replayed). The history stays on the hub and can be read later over HTTP or with the `history` command.

### Example

A real frame from a test hub, shortened to one point: a kitchen line drawing about 400 W. On the wire it's one compact line. The energy total is still small because this point counts energy on the module, and the 15-minute period with the load hadn't closed yet.

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

## Reading data without a token in the URL

From **v1.1.7**, MQTT reads need the **read** token when the hub has read and admin tokens set, the same as HTTP reads: `cmd get` and `history` are refused without it. For a browser client that shouldn't carry a token in the address, `POST /api/v1/ws-ticket` returns a single-use ticket valid for 30 seconds. → [Settings → Security](web-settings.md#security)

## Troubleshooting

**Nothing arrives on the broker**
- Check that the broker address, port, login and password are right, and that you restarted the hub after saving them.
- The hub must be on your home Wi-Fi; in access-point-only mode MQTT doesn't start.
- Check `rbgrid/<mac>/status`: `online` means the hub is connected.

**Home Assistant shows the sensors as unavailable**
- The hub isn't sending: check the status topic and the broker connection. Sensors come back as soon as frames arrive again.
