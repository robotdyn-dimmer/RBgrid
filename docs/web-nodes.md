# Remote nodes (ESP-NOW)

## Overview

A **remote node** is a small ESP32 board with its own rbAmp modules. It measures where an I2C cable from the hub can't reach: a garage, a pool pump, a water heater in the basement, a sub-panel in an outbuilding. It sends its readings to the hub by radio, over **ESP-NOW**. The node is not on your Wi-Fi network and has no address of its own: it talks to its hub only. Its channels become ordinary **points** on the hub, with live values, history and cost like any other point.

This page is the complete documentation of nodes: the board and modules, wiring, flashing, pairing, how a node keeps time and energy, what the app shows, the hub's API and MQTT topics, and what to do when something is off. Remote nodes need hub firmware **v1.1.7 or later** and web app 1.1.7 or later. On a hub with older firmware the app shows no Nodes section at all; "Remote nodes are not available in this hub firmware." appears only for a hub whose firmware image can't run nodes.

A node is a sender. It has no display, no web page and no history you can read, and it has no update over the air: you flash it over USB. From firmware 1.5 it does keep finished 15-minute periods in its own flash until the hub has them — that is a queue, not a record you can open. The hub does the rest.

> For any work inside an electrical panel, call a qualified electrician.

## What you need

| | Requirement |
|---|---|
| **Node board** | An original **ESP32** module with **4 MB flash**. PSRAM is not needed. On v1.1.7 the node firmware is built and tested for the original ESP32 only: ESP32-S2, S3 and C3 boards are not built for and not tested. |
| **rbAmp modules on the node** | Up to **three** modules on the node's I2C bus, at addresses `0x50`, `0x51` and `0x52` (the module's own setting); SDA is GPIO 25 and SCL is GPIO 26. The hub uses up to **four** current channels per node. |
| **A voltage input on the node** | On v1.1.7 a node records energy only when one of its modules measures voltage. A current-only module on a node gives live current, but no 15-minute periods, so no history and no cost. Fit a **single-phase rbAmp set with a voltage input**. |
| **Phases** | On v1.1.7 a node measures single-phase points only. There is no phase grouping on a node and no "use as the three-phase input": that is for a module on the hub. An STD-U3I3 on a node is untested. |
| **The hub** | Firmware v1.1.7 or later, connected to your Wi-Fi. The hub's radio for nodes runs on the same channel as your router; a hub with no Wi-Fi can't run nodes. One hub takes up to **six** nodes. |
| **History slots** | Node points share the hub's existing **12 history slots** with the hub's own points; they don't add to them. The Nodes section shows "History: *used* of 12 points in use" before you pair. |
| **A computer with USB** | For flashing the node, and for every later update of the node firmware. |
| **Current clamps** | As for a module on the hub; the clamp model is set per channel from the hub (see [Node settings](#node-settings)). |

## Wiring the node

A node is wired like a small hub: its rbAmp modules sit on the node's I2C bus, and the current clamps go on the modules.

- **I2C:** four wires between the ESP32 board and the modules, SDA to GPIO 25, SCL to GPIO 26, GND and 3.3 V. Module addresses `0x50`, `0x51`, `0x52` are set on the modules; the node reads whichever are present when it starts, up to three. The bus is pulled up by the modules' on-board resistors; with up to three modules on short cables leave them as they are. Cable length and pull-ups for other cases are in the rbAmp wiring guide, the same as for modules on the hub.
- **Voltage input:** on v1.1.7 one module of the node must have its voltage input wired to the mains it meters; without it the node has live current but records no energy (see [What you need](#what-you-need)). The voltage input also gives the node the direction of each current, import or export.
- **Clamps:** one per current channel, on the conductor you want to meter, the arrow marking as the rbAmp module docs say. The clamp model is set per channel from the hub (see [Node settings](#node-settings)); a wrong model scales the readings wrongly.
- **Power:** the ESP32 board's usual supply, for example its USB port or a 5 V supply on the board's input. The node has no battery and no hold-up: a power cut stops it, and the energy of the period that was open is lost.
- **Place:** where it will live. A node does not have to be near the hub to pair; the link is checked where it stands (see [Checking the link](#checking-the-link)). Avoid metal enclosures between the node and the hub where you can.

> For any work inside an electrical panel, call a qualified electrician.

## Flash the node firmware

The node firmware is published with every hub release on the RBgrid Releases page on GitHub: `rbgrid-node_vX.Y.Z.bin` (the application alone) and `rbgrid-node_vX.Y.Z_full.zip` (boot loader, partition table and application). Use the version that matches your hub, or a newer one.

With the board in download mode and esptool installed (`pip install esptool`), write the full set:

```
python -m esptool --chip esp32 -p <PORT> -b 460800 write_flash 0x1000 bootloader.bin 0x8000 partition-table.bin 0x10000 rbgrid-node_vX.Y.Z.bin
```

> ⚠️ **Updating a node to firmware 1.5 writes all three files, not the application alone.** 1.5 adds the flash partition the kept periods live in. A node given only the new application runs 1.5 with nowhere to keep them and falls back to eight periods in memory, and the card does not say so. Writing all three is also what you want for any later update: it costs nothing, and a node paired under 1.4 does not need pairing again.

Building the firmware yourself, and the pins to change if your board wires I2C differently, are in `node/README.md` of the RBgrid repository.

The node needs no key file and no channel setting. It gets its key from the hub when you pair it, and it finds the hub's channel by itself.

### Building the node firmware yourself

The source is the `node/` folder of the RBgrid repository, under the same license as the hub firmware. It builds with ESP-IDF 5.5.1 (5.4.1 also builds):

```
cd node
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash
```

The rbAmp library is fetched at build time (`dependencies.lock` pins its version). If your board wires I2C to other pins, build with `-DRBN_I2C_SDA=<n> -DRBN_I2C_SCL=<n>`. No key file is needed. The development variants in the source (`RBN_ROLE_MASTER`, `RBN_FREERUN`, `RBN_OUTPUT_NODE`) are not part of the release and are not supported.

## Pair a node

Nodes are managed on **Management → Points**, in the **Remote nodes (ESP-NOW)** section. The header shows "*n* of 6 nodes" and the state of the radio for nodes, "radio on" or "radio off". On a hub that can run nodes the section sits at the top of the page, above **rbAmp modules**, and the setup wizard gets an optional **ESP-NOW nodes** step ("Step 5 of 8", after Sensors and before Tariff).

1. Place the node where it will live, with its modules and clamps fitted, but leave it unpowered.
2. In the app, choose the **Pairing time** (in minutes) and press **Pair a node**. The section says: "Power the node up now. It shows up below in a few seconds." and counts down: "Waiting for a node… *time* left". **Stop pairing** closes the window early.
3. Power the node up (or reset it). It scans the Wi-Fi channels for the hub, exchanges keys with it and appears in the list, marked **new**. Its points are added at once: "Paired in this window: *n*. Their points are already added; no restart needed."

> ⚠️ **While the pairing window is open, any RBgrid node in range that asks is paired.** There is no PIN. That includes a node someone else owns whose own hub has gone quiet: a paired node that gets no answer for about 150 seconds starts asking to pair by itself. Open the window only when you are about to power a node up, keep it short, and close it with **Stop pairing** once your node is in the list; then check that the list holds only the nodes you expect.

The keys are kept on the node and on the hub. After a restart, or after the radio was off, the node reconnects by itself; you pair it once.

Before you pair, check the two capacity lines in the section: "*n* of 6 nodes" (when the hub is full the button is replaced by "The hub takes up to 6 nodes. Remove one to pair another.") and "History: *used* of 12 points in use. Points beyond 12 show live values only." (or "History: all 12 points are in use. A new node's points will show live values only."). If the radio is off, the section says "Turn the radio for nodes on to pair one."

Each node brings up to four points. They are named after the node and its channels; rename them and give them roles on their own pages, like any other point (see [Points](web-points.md)). Node points can't be bound or unbound by hand: a channel turned off in the node's settings stops measuring, and removing the node removes its points.

## The node card

![The Remote nodes section: the radio on channel 6, the pairing control, and two node cards — one online with its signal, clock sync and two receiving channels, one offline for 90 minutes](https://www.rbgrid.io/assets/img/webapp-docs/wa-nodes-list.webp)

Each paired node has a card in the section.

| On the card | What it means |
|---|---|
| **online** / **offline** | The hub has heard the node within the last 15 seconds, or not. A node that is off (see below) shows **off**. |
| **new** | Paired in the current pairing window. |
| **Last heard** | "now", "*n* s ago", "*n* min ago", "*n* h ago", or "not heard since the hub started". |
| **Signal** | The strength of the node's last frame, in dBm, as the hub's radio saw it. |
| **Lost packets, last minute** | The share of the hub's time beacons the node missed over the last minute, in percent. It reads "measuring (1 min)" until a full minute has passed. |
| **Clock sync** | How closely the node's clock follows the hub's: "±*n* µs · *n* ppm". |
| **Firmware** | The node firmware version, once the node has reported it. |
| **Period** | "15 min". The period is fixed; the Settings dialog says so: "Energy records: every 15 min, fixed." |
| **Waiting to send** | Where the hub reports it: the number of finished 15-minute periods the node is holding because the hub was out of reach (see [How a node keeps time and energy](#how-a-node-keeps-time-and-energy)). Hub firmware v1.1.7 does not report it, so the row does not appear there. |
| **Channel *n*** | Each current channel of the node: the point it feeds (its name, a link to the point's page; "Point *key*" until the point has a name) and its state, "receiving", "no data" or "off (Settings)". |
| settings not applied yet / settings not applied | Settings were sent to the node and it has not confirmed them yet, or it refused them. |
| Signal, lost packets and clock sync missing | A node not heard since the hub started has none of them: the card leaves the three rows out and **Last heard** reads "not heard since the hub started". They appear with the node's first frame. |

The card has three buttons: **Settings**, **Turn off** / **Turn on**, and **Delete**.

## Node settings

**Settings** opens "Node *mac*: settings". For every channel it shows **Channel *n* measures** (on or off) and **Current clamp**, the clamp model fitted to that channel. "Pick the clamp actually fitted to each channel: a wrong one scales its readings wrongly. A channel turned off stops measuring." At least one channel has to stay on.

**Send to the node** sends the settings through the hub: "Sent. The node applies the settings on its next contact; the card shows «settings not applied yet» until it confirms." If the node is offline, the hub keeps the settings and hands them over when the node is back ("The node is offline. It takes the settings when it's back.").

The energy period of a node is fixed at 15 minutes ("Energy records: every 15 min, fixed."); it can't be changed.

## Turning a node off and on

**Turn off** keeps the node paired but makes the hub discard its data. The app asks first: "The hub discards the node's data while it's off: that energy won't be in History or the totals, and turning it back on doesn't bring it back. To pause a node and keep its energy, leave it on." While off, the card shows **off**, and its points show no live values. **Turn on** resumes.

Use it when a node measures something you don't want counted for a while. To stop a node for good, remove it.

## The radio for nodes

The **Radio for nodes** line in the section header shows one of three states:

- "On, channel *n*: the same as your Wi-Fi router's." The hub's radio for nodes is up. It runs on the hub's Wi-Fi channel, which your router chooses; if the router moves to another channel, the hub and its nodes follow.
- "Starts by itself when a node is paired (factory setting)." Nothing is running yet; the radio starts with the first pairing or when a node is already paired.
- "Off. Nodes can't send data." You switched it off.

Switching it off asks first: "The nodes stop sending: their points get no live values and no history until the radio is on again. The hub remembers this after a restart." Every node goes offline at once. While the radio is off, nodes keep closing their 15-minute periods and keep them until it is on again — on node firmware 1.5 about five days of them, in the node's own flash; on 1.4, eight periods, about two hours, in its memory. Older periods and the live values of that time are not kept.

## Removing a node

**Delete** asks "Remove node *mac*?": "The node and its points (channels: *n*) are removed, with their stored history, for good. Download the history first in History (CSV) if you need it. To use the node again, pair it again."

> ⚠️ **Removing a node deletes its points and all their stored history.** If you may need the records, download them first in [History](web-history.md#downloads).

The hub stops accepting the node's data at once and confirms: "Node removed, with its points: *n*." To use the node again, pair it again: open a pairing window and reset the node (or wait about 2½ minutes: a node that gets no answer from its hub for 150 seconds starts asking to pair by itself).

## Moving a node to another hub

A paired node keeps asking its hub; once it has had no answer for 150 seconds it marks itself unpaired and asks to pair again, and after a reset it asks at once. So moving a node to another hub, or putting a second-hand node into service, is a pairing:

1. **Delete** it on the old hub (if you have it).
2. Open a pairing window on the new hub and reset the node (or wait about 2½ minutes). The node takes the new hub's key.

Erasing the node's flash first (`python -m esptool --chip esp32 -p <PORT> erase_flash`, then [flash the firmware again](#flash-the-node-firmware)) is optional: a clean start, not a requirement.

## How a node keeps time and energy

- **Clock.** The hub is the clock. The node asks for the time every 5 seconds until it has it, then every minute, and keeps its own timer in between. The card's **Clock sync** shows how closely it follows.
- **Live values** are sent about once a second and shown on the node's points. They are not stored.
- **Energy** is recorded in **15-minute periods**, which the node closes by its own clock on the hub's 15-minute boundaries, with the energy from its modules' own counters. When your tariff zone changes, the hub asks every node to close its period at that moment too, so cost per zone is exact.
- **A period reaches the hub, or it is kept.** The node sends a finished period until the hub confirms it has recorded it, so records survive a weak link better than the live view does. The hub recognises a period it already has and never counts it twice.
- **Out of reach.** The node keeps closing periods on time and holds the finished ones until the hub confirms them. When the link is back they arrive together and are filed and priced by the time they were measured, not by the time they arrived. How much it can hold is the node's firmware, not the hub's:
  - **Node firmware 1.5** writes them to its own flash: **about five days** of periods, and they survive a power cut on the node.
  - **Node firmware 1.4** keeps **eight** of them (about two hours) in RAM; a ninth pushes the oldest one out, and a power cut on the node loses the held periods and the open one.

  Either way, once the hold is full the oldest period goes, and the node says so in its log.
- **A node that boots without its hub** closes no periods until it has heard the hub once. The energy is not lost: on v1.1.7 the first record after a start covers everything from the node's boot to the end of its first full 15-minute period, so that period can read higher than the load really was.

## Checking the link

There is no pass mark for signal or lost packets in the firmware, and this page gives none; the node is either reliable where it stands or it isn't. The honest test is behavioural:

1. Pair the node where it will live, with its load actually running.
2. Watch the card for about ten minutes: the node should stay **online**, **Lost packets, last minute** should stay at 0 and **Last heard** should stay at "now" or a few seconds.
3. If lost packets climb above zero, or the node flips between online and offline, the link is marginal. Move the node, or the hub, a little, and watch again.

Two thresholds exist in the firmware, and they are all that exists: a node is **offline** after 15 seconds without a frame (the hub logs `node_offline` and `node_online`), and a node that hears no beacon for 8 seconds starts scanning for the hub's channel again (this is how it follows a router that changes channel; no restart is needed).

A weak link shows up as lost packets above 0 and gaps in the live values while the records keep arriving; a worse one makes the node flap. **A node out of range and a node without power look the same** to the app and the API. Only the card's history before the drop tells them apart: a weak link drifts first (rising losses, falling signal, flapping), a power cut is clean.

### Knowing later that a node has gone quiet

The five-day hold only helps if you notice inside five days, and on firmware **v1.1.8** nothing in the app comes to find you: a node that stops reporting is shown on its card and in the Nodes list, and nowhere else. There is **no** entry in the app's Journal and **no** item in **Needs attention** — the hub writes that line to its own serial log only.

**With Home Assistant, you have a proper alarm and it costs one automation.** The hub publishes a per-node **Online** connectivity entity by MQTT discovery, as a sub-device of the hub, and it follows the node's retained status. Automate on that entity turning off and you will be told within the minute. The same transition is published as a `node_offline` event on the hub's event topic, if you prefer to watch events.

**Without MQTT, the card is all there is.** Open the Nodes section now and then — or give the node's points a glance in History, where a silent node leaves a growing gap.

What the transition actually is, so you can set the automation's patience:

- the edge fires after **15 seconds without a live frame** — the same threshold the card uses, and a node sends about one frame a second;
- there is **no debounce**: every gap longer than 15 s produces one offline/online pair, and gaps shorter than that produce nothing. A marginal link gives you a handful of pairs, not hundreds;
- the status topic is refreshed **once a minute** with signal and loss, as a state rather than an event — so a dashboard reading it is never more than a minute stale.

## Totals and the "unmetered" remainder

The house total can't be double-counted: the system totals are taken at the grid boundary, from the input point only. Load points, on the hub or on a node, never add into them.

The **unmetered** remainder on the Overview can. It is the input minus the sum of all line points, and node points are line points. If you meter a sub-panel on a node *and* a load inside that sub-panel on another channel, that load is subtracted twice, and the remainder can go negative.

**From firmware v1.1.8 you can tell the hub about it:** mark the inner point as measured **inside** the outer one, and it stops being subtracted a second time. That is the right answer for a workshop or garage sub-feed metered on a node with its loads metered behind it — you keep both figures and the remainder still means something. → [Points](web-points.md)

> On **v1.1.7** there was no such setting, and the advice was to meter either the sub-panel or the loads inside it, not both. The house total is unaffected either way: system totals are taken at the input.

## Where nodes appear elsewhere

- **Points:** a node's channels are points like any other, with their own pages, roles and names (see [Points](web-points.md)).
- **History and Economics:** by point, as for any point; node points share the 12 history slots (see [History](web-history.md)).
- **Events:** the Hub log records `node_online` and `node_offline` (see [Events](web-events.md)).
- **MQTT:** the hub publishes a retained status per node on `rbgrid/<mac>/node/<NODEMAC>/status` (`online`, signal, lost packets, last heard), refreshed every minute, and node events on `rbgrid/<mac>/event`; the same node actions are available over the command channel (see [MQTT broker](hub-mqtt.md)).
- **Home Assistant:** one device per node, with an **Online** sensor and a **Signal** sensor, by discovery. The node's points appear like local ones.
- **The hub's API:** `GET /api/v1/espnow` lists the radio state and every node with its health; `POST /api/v1/espnow` runs the same actions as the app (see below).

## The hub's API

Everything the app does with nodes goes through one endpoint of the hub, which you can use yourself.

**`GET /api/v1/espnow`** (the read token, as for other reads) returns the radio and the registry:

| Field | Meaning |
|---|---|
| `running`, `enabled` | the radio is up; `enabled` is `auto` (starts by itself when a node is paired), `on` or `off` |
| `keys`, `key_source` | `true` on every hub image from v1.1.7, and where the hub's key came from (`generated`: made by this hub the first time the radio started). A hub without `keys` can't run nodes |
| `max_nodes`, `max_channels_per_node` | 6 and 4 |
| `channel` | the radio channel, the Wi-Fi router's |
| `pairing_open`, `pairing_left_s`, `paired_count` | the pairing window, the seconds left in it, the number of paired nodes |
| `beacons_sent`, `rt_recv`, `stats_recv`, `periods_recv`, `periods_injected`, `periods_dropped`, `periods_dup` | counters since the hub started: time beacons sent, live frames and period frames received, periods recorded, dropped, and recognised as repeats |
| `nodes[]` | per node: `mac`, `online`, `disabled`, `last_seen_s`, `rssi_dbm`, `sync{ resid_sd_us, skew_ppm, fits, loss_pct }`, `channels[{ ch, ct_code, unit_id, present }]`, `config_state` (`none`, `pending`, `applied`, `failed`), `config[]`, `period_s`, `fw`. A node not heard since the hub started has no `last_seen_s` and no `sync{}` |

**`POST /api/v1/espnow`** (the admin token) with `{ "action": … }`:

| `action` | Fields | What it does |
|---|---|---|
| `hub_on` / `hub_off` | — | switch the radio for nodes on or off (kept across restarts); off marks every node offline at once |
| `pair` | `seconds` 10–600 | open the pairing window; `409 ESPNOW_DISABLED` while the radio is off |
| `pair_cancel` | — | close it early |
| `configure` | `mac`, `config: { channels: [ { ch, enabled, ct_code } ] }` | node settings; the hub resends until the node confirms. `400 PERIOD_UNSUPPORTED` for any `period_s` other than 900; `400 BAD_REQUEST` for an unknown channel, a `ct_code` outside 1–7 or every channel off |
| `disable` / `enable` | `mac` | turn a node off or on |
| `remove` | `mac` | remove the node and its points, with their history |
| `latch` | — | ask every node to close its current period now |
| `add` | `mac`, `channels: [ { ch, ct_code } ]` | register a node by hand, without pairing |

Errors come back as `{ "ok": false, "error": "BAD_REQUEST" | "ESPNOW_DISABLED" | "ESPNOW_OFF" | "ESPNOW_START_FAILED" | "REGISTRY", "message": "…" }`. Node points are ordinary points in every other endpoint (`src: "espnow"` in the live frame), and `GET /api/system/provisioning` reports `nodes_paired`.

**Over MQTT** the same works on the command channel: `{"cmd":"get","section":"espnow"}` returns the GET above, and `{"cmd":"set","section":"espnow","token":"<admin>","data":{"action":"pair","seconds":60}}` runs an action and answers with the hub's reply. The hub publishes a retained status per node on `rbgrid/<mac>/node/<NODEMAC>/status` (`{ "online", "rssi_dbm", "loss_pct", "last_seen_s" }`, refreshed every minute) and node events on `rbgrid/<mac>/event` (`paired`, `removed`, `node_online`, `node_offline`). In Home Assistant each node is a device with an **Online** and a **Signal** (dBm) sensor; a node not heard since the hub started reads "no data".

## Messages

| Message | What it means |
|---|---|
| Remote nodes are not available in this hub firmware. | This hub's firmware image can't run nodes. (A hub older than v1.1.7 shows no Nodes section at all.) |
| Turn the radio for nodes on to pair one. | The radio is switched off; turn it on first. |
| The hub takes up to 6 nodes. Remove one to pair another. | All six node slots are used. |
| The radio for nodes is off. Turn it on first. | An action needs the radio and it is off. |
| The hub couldn't start the radio for nodes. Try again; if it keeps failing, restart the hub. | The radio failed to start. |
| The hub couldn't change its node list (full, or the node isn't known). | The node registry refused the change. |
| The hub didn't accept the request. | The hub answered that the request was malformed. Reload the page and try again. |
| The hub refused: *message* | The hub's own reason. |
| The hub didn't answer. Check the connection and try again. | No reply from the hub. |
| The hub doesn't know this node any more. Reload the page. | The node was removed, perhaps from another browser. |
| The hub takes only the 15-minute period. | Only the 15-minute period is accepted for a node. |

## Troubleshooting

**I pressed Pair a node, but nothing appears**
- Power the node up, or reset it, *while* the window is open.
- Check the radio for nodes is not off, and that the hub is connected to your Wi-Fi.
- A node that was paired with another hub asks again once that hub has not answered it for 150 seconds, or at once after a reset (see [Moving a node to another hub](#moving-a-node-to-another-hub)).
- Check the node's power, and that its flash finished without errors.

**The node is offline, or keeps going online and offline**
- Nothing has been heard from it for 15 seconds. Check its power first; then its place. See [Checking the link](#checking-the-link).
- The hub's radio for nodes may be off: every node is offline then.

**Signal, lost packets and clock sync are missing from the card**
- The node has not been heard since the hub started ("Last heard: not heard since the hub started"). The rows appear with its first frame.

**The node's points show live values but no history**
- The hub's 12 history slots were already in use when the node was paired: "Points beyond 12 show live values only." Remove a point you don't need, then re-pair the node, or accept live-only points.
- On v1.1.7 a node records energy only with a module that measures voltage; a current-only module on a node gives live current but no records.

**Energy is missing for a while**
- The node was out of reach for longer than it can hold periods, or lost power. On node firmware 1.5 that is about five days, and a power cut does not lose what it holds; on 1.4 it is eight periods, about two hours, and a power cut loses both them and the period that was open.
- The node was **off** (Turn off) for that time: the hub discards a node's data while it is off.
- The node booted while the hub was unreachable: on v1.1.7 the energy from its boot lands in its first full 15-minute period, so an earlier period looks empty and that one looks high. Nothing is lost.

**My router changed its Wi-Fi channel**
- Nothing to do. The hub's radio for nodes follows the router, and a node that hears nothing for 8 seconds scans for the hub again.
