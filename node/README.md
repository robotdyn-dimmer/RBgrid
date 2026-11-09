# RBgrid Node

Firmware for a remote metering node. The node reads rbAmp metering modules and sends their data to an RBgrid hub over ESP-NOW. It needs no Wi-Fi network of its own and no channel setting.

Works with RBgrid hub firmware 1.1.7 or later.

## Hardware

- **ESP32** module with **4 MB flash**. PSRAM is not needed.
- **rbAmp modules** on I2C, at addresses `0x50`, `0x51` and/or `0x52` (the modules' own setting). The node finds the ones present at start.
- **I2C pins:** SDA = GPIO 25, SCL = GPIO 26. Other pins: build with `-DRBN_I2C_SDA=<n> -DRBN_I2C_SCL=<n>`.
- The node must be within ESP-NOW range of the hub.

The hub uses up to 4 inputs per node and up to 6 nodes. The hub keeps history for 12 points in total, its own points and node points together; a point beyond that shows live values only.

## Build

ESP-IDF 5.5.1 (5.4.1 also builds).

```
idf.py set-target esp32
idf.py build
```

The rbAmp library is fetched at build time; `dependencies.lock` pins its version.

No key file is needed. The node gets its key from the hub when you pair it. `espnow_keys.example.h` is only for development builds that pair with a pre-shared key; release hubs don't accept that.

## Flash

Over USB, with the module in download mode:

```
idf.py -p <PORT> flash
```

or with esptool, from `build/`:

```
esptool.py --chip esp32 -p <PORT> -b 460800 write_flash 0x1000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 rbgrid_node.bin
```

The node has no update over the air. Update it over USB.

## Pair with the hub

1. In the RBgrid app, open **Points → Nodes** and press **Pair**. The pairing window stays open for the time you choose (10–600 s).
2. Power up the node, or reset it.
3. The node finds the hub's channel, and the two exchange keys. The node appears in the list, and its points appear at once.

The key is kept on the node. After a restart it reconnects without pairing again. To move the node to another hub, remove it on the old hub, erase the node's flash (`idf.py -p <PORT> erase-flash`), flash it again and pair it with the new hub.

## What the node does

- Follows the hub's channel. If the router moves the hub to another channel, the node finds it again.
- Keeps its clock in step with the hub and closes a period every 15 minutes, on the hub's 15-minute boundaries.
- Sends live readings, and one record per period with the energy from the modules' own counters.
- While the hub is out of reach, it keeps closing periods on time and holds up to 8 of them. They are sent when the link is back. A power cut on the node loses the period that was open.
- Takes its settings from the hub: each input on or off, and the clamp model.

## Development builds

`rbgrid_node_main.c` also has development variants (`RBN_ROLE_MASTER`, `RBN_FREERUN`, `RBN_OUTPUT_NODE`). They are not part of the release and are not supported.

## Licence

PolyForm Noncommercial License 1.0.0 (see `LICENSE`). Commercial use needs a separate licence (see `COMMERCIAL.md`). Third-party components: see `NOTICE`.
