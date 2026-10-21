# RBgrid Firmware Flashing Guide

Complete guide for flashing RBgrid firmware to the hub (ESP32-WROVER, 16 MB flash, PSRAM) using esptool.

> Layout note (2026-10-19): the firmware uses a **16 MB** partition table with **two application slots** (`ota_0`, `ota_1`) since the partition revision of 2026-10-17; there is no `factory` slot any more. Boards flashed with an earlier 4 MB layout need a full flash with the commands below once (the 4 MB layout and flash size are not compatible with this firmware). Source of truth for the layout: `partitions.csv` in the repository root.

## Prerequisites

### Install esptool

**Windows:**
```bash
pip install esptool
```

**Linux/macOS:**
```bash
pip3 install esptool
```

On Windows, if `esptool.py` is not recognized, use `python -m esptool` in place of `esptool.py` in every command below.

### Download Firmware

Download the latest firmware from [GitHub Releases](https://github.com/robotdyn-dimmer/RBgrid/releases).

Extract the release archive (`RBgrid_vX.Y.Z.zip`) to get the following files:
- `bootloader.bin`
- `partition-table.bin`
- `ota_data_initial.bin`
- `main.bin`

### Hardware

The hub is an **ESP32-WROVER module with PSRAM and 16 MB flash**. Boards without PSRAM, or with 4 MB / 8 MB flash, are not supported: the firmware requires PSRAM and the partition table needs 16 MB.

## Flashing Instructions

### Step 1: Connect ESP32

Connect the hub to the computer via USB and identify the COM port:

**Windows:**
- Check Device Manager → Ports (COM & LPT)
- Typically shows as `COM3`, `COM4`, etc.

**Linux:**
```bash
ls /dev/ttyUSB*
# Usually /dev/ttyUSB0
```

**macOS:**
```bash
ls /dev/cu.usbserial-*
# Usually /dev/cu.usbserial-0001
```

### Step 2: Erase Flash (first installation only)

Erase a fresh board, or a board that ran other firmware or the old 4 MB layout, once:

```bash
esptool.py --chip esp32 --port COM3 erase_flash
```

Replace `COM3` with your actual port. **Erasing also removes any RBgrid configuration, tokens and history on the hub**: do not erase for a routine update (use the over-the-air update instead, see below).

### Step 3: Flash Firmware

Navigate to the folder where you extracted the firmware files and run:

**Windows:**
```bash
esptool.py --chip esp32 --port COM3 --baud 460800 ^
  --before default_reset --after hard_reset ^
  write_flash -z --flash_mode dio --flash_freq 40m --flash_size 16MB ^
  0x1000 bootloader.bin ^
  0x10000 partition-table.bin ^
  0x17000 ota_data_initial.bin ^
  0x20000 main.bin
```

**Linux/macOS:**
```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  --before default_reset --after hard_reset \
  write_flash -z --flash_mode dio --flash_freq 40m --flash_size 16MB \
  0x1000 bootloader.bin \
  0x10000 partition-table.bin \
  0x17000 ota_data_initial.bin \
  0x20000 main.bin
```

`--flash_size 16MB` is not optional. If esptool reports a flash size mismatch, the board is not a 16 MB module and is not supported.

### Step 4: Verify

After flashing completes, the ESP32 will automatically reset. You should see:

```
Hash of data verified.

Leaving...
Hard resetting via RTS pin...
```

The very first boot on a blank flash takes about twenty seconds longer than later boots: the hub lays out its statistics area once. The hub then starts its setup access point.

### Application-only reflash

To replace only the application over USB (for example to return to a known image), write the OTA selector and the application; configuration, tokens and history are kept:

```bash
esptool.py --chip esp32 --port COM3 --baud 460800 write_flash -z 0x17000 ota_data_initial.bin 0x20000 main.bin
```

Writing `ota_data_initial.bin` makes the bootloader start the slot you just wrote (`ota_0`) instead of the other slot. A hub that still runs the previous single-OTA-slot table (its `GET /api/system/version` names a `factory` partition) needs `partition-table.bin` at `0x10000` added to this command once; the data partitions are at the same offsets in both tables and keep their contents.

## Memory Layout

The firmware uses the following partition table (`partitions.csv`, 16 MB):

| Name | Type | Offset | Size | Description |
|------|------|--------|------|-------------|
| bootloader | - | 0x1000 | ~30 KB | ESP32 bootloader |
| partition-table | - | 0x10000 | 3 KB | Partition table |
| nvs | data/nvs | 0x11000 | 24 KB | Non-volatile storage (Wi-Fi credentials, counters snapshot) |
| otadata | data/ota | 0x17000 | 8 KB | OTA selector: which application slot boots |
| phy_init | data/phy | 0x19000 | 4 KB | PHY init data |
| ota_0 | app/ota_0 | 0x20000 | 3 MB | Application slot A (a USB flash writes this one) |
| ota_1 | app/ota_1 | 0x320000 | 3 MB | Application slot B |
| stats | data/0x81 | 0x620000 | 4 MB | Statistics rings (history), raw partition |
| storage | data/spiffs | 0xA20000 | 5 MB | Web UI assets |
| logs | data/spiffs | 0xF20000 | 512 KB | Event log |
| config | data/spiffs | 0xFA0000 | 384 KB | Configuration files, tokens, backup slot |

Every over-the-air update is written to the application slot that is not running and keeps the previous image for rollback; `GET /api/system/version` → `partition` shows the running slot. Statistics, the web UI, the event log and the configuration each have their own partition and survive application updates and application-only reflashes.

## Troubleshooting

### Connection Failed

**Error:** `Failed to connect to ESP32`

**Solutions:**
1. Hold BOOT button while connecting
2. Try lower baud rate: `--baud 115200`
3. Check USB cable (data cable, not charge-only)
4. Install USB-to-UART drivers (CP210x or CH340)

### Permission Denied (Linux)

```bash
sudo usermod -a -G dialout $USER
# Log out and log back in
```

Or use sudo:
```bash
sudo esptool.py ...
```

### Flash Size Mismatch

**Error:** `A fatal error occurred: Flash size mismatch`

**Solution:** The hub must have 16 MB flash. Check the module with:

```bash
esptool.py --chip esp32 --port COM3 flash_id
```

A module that reports 4 MB or 8 MB cannot run this firmware.

### Boots, then reboots in a loop

PSRAM not detected (wrong module) or a partially written flash: erase and write all four images again.

### The hub keeps booting the old firmware after an app-only reflash

You wrote `main.bin` without `ota_data_initial.bin`; the bootloader still selects the other slot. Write both as shown above.

## Monitoring Serial Output

After flashing, monitor the serial output to verify successful boot:

```bash
# Using ESP-IDF monitor
idf.py -p COM3 monitor

# Or using screen (Linux/macOS)
screen /dev/ttyUSB0 115200

# Or using PuTTY (Windows)
# Serial, COM3, 115200 baud
```

Press `CTRL+]` to exit screen, or `CTRL+C` then `CTRL+]` in idf.py monitor.

## OTA Updates

After the initial USB flash, updates are installed over the air (`POST /api/system/ota` with the new `main.bin`). The hub writes the image into the slot it is not running from, restarts, and confirms the new image one minute after it has joined Wi-Fi; an image that fails before that is rolled back by the bootloader. See the *Updating* page of the user documentation for the procedure and the checks (`image_md5`, event log).

## Additional Resources

- [esptool documentation](https://docs.espressif.com/projects/esptool/en/latest/)
- [ESP32 Partition Tables](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/partition-tables.html)
- [RBgrid Releases](https://github.com/robotdyn-dimmer/RBgrid/releases)

## Support

For issues or questions, please open an issue on [GitHub](https://github.com/robotdyn-dimmer/RBgrid/issues).
