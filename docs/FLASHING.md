# RBgrid Firmware Flashing Guide

Complete guide for flashing RBgrid firmware to ESP32 using esptool.

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

### Download Firmware

Download the latest firmware from [GitHub Releases](https://github.com/robotdyn-dimmer/RBgrid/releases).

Extract `RBgrid_v1.0.1.zip` to get the following files:
- `bootloader.bin`
- `partition-table.bin`
- `ota_data_initial.bin`
- `main.bin`

## Flashing Instructions

### Step 1: Connect ESP32

Connect your ESP32 to the computer via USB and identify the COM port:

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

### Step 2: Erase Flash (Recommended)

Before flashing new firmware, it's recommended to erase the flash:

```bash
esptool.py --chip esp32 --port COM3 erase_flash
```

Replace `COM3` with your actual port.

### Step 3: Flash Firmware

Navigate to the folder where you extracted the firmware files and run:

**Windows:**
```bash
esptool.py --chip esp32 --port COM3 --baud 460800 ^
  --before default_reset --after hard_reset ^
  write_flash -z --flash_mode dio --flash_freq 40m --flash_size 4MB ^
  0x1000 bootloader.bin ^
  0x10000 partition-table.bin ^
  0x17000 ota_data_initial.bin ^
  0x20000 main.bin
```

**Linux/macOS:**
```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  --before default_reset --after hard_reset \
  write_flash -z --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 bootloader.bin \
  0x10000 partition-table.bin \
  0x17000 ota_data_initial.bin \
  0x20000 main.bin
```

### Step 4: Verify

After flashing completes, the ESP32 will automatically reset. You should see:

```
Hash of data verified.

Leaving...
Hard resetting via RTS pin...
```

## Memory Layout

The firmware uses the following partition table:

| Name | Type | Offset | Size | Description |
|------|------|--------|------|-------------|
| bootloader | - | 0x1000 | ~30KB | ESP32 bootloader |
| partition-table | - | 0x10000 | 3KB | Partition table |
| nvs | data/nvs | 0x11000 | 24KB | Non-volatile storage |
| otadata | data/ota | 0x17000 | 8KB | OTA data partition |
| phy_init | data/phy | 0x19000 | 4KB | PHY init data |
| factory | app | 0x20000 | 1472KB | Main application |
| ota_0 | app/ota | 0x190000 | 1472KB | OTA update partition |
| storage | data/spiffs | 0x300000 | 832KB | Statistics storage |
| logs | data/spiffs | 0x3D0000 | 128KB | Log files |
| config | data/spiffs | 0x3F0000 | 64KB | Configuration |

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

**Solution:** Ensure your ESP32 has 4MB flash. For different sizes, adjust `--flash_size`:
- 2MB: `--flash_size 2MB`
- 8MB: `--flash_size 8MB`
- 16MB: `--flash_size 16MB`

### Verify Flash

To read and verify what's on the flash:

```bash
esptool.py --chip esp32 --port COM3 flash_id
```

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

After initial flashing, future updates can be done wirelessly via OTA (Over-The-Air) if your network is configured.

## Additional Resources

- [esptool documentation](https://docs.espressif.com/projects/esptool/en/latest/)
- [ESP32 Partition Tables](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/partition-tables.html)
- [RBgrid Releases](https://github.com/robotdyn-dimmer/RBgrid/releases)

## Support

For issues or questions, please open an issue on [GitHub](https://github.com/robotdyn-dimmer/RBgrid/issues).
