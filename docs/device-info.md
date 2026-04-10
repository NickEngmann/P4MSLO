# ESP32-P4X-EYE Device Info

## Hardware

| Field | Value |
|-------|-------|
| Board | ESP32-P4X-EYE |
| Chip | ESP32-P4 (revision v1.3) |
| Crystal | 40 MHz |
| Flash | 16 MB |
| PSRAM | Yes (SPIRAM, 200 MHz) |
| USB | Built-in USB-Serial/JTAG |
| MAC | 30:ed:a0:e2:1d:9f |
| Display | 240x240 round LCD |
| Camera | OV2710 |

## USB Connection

The ESP32-P4 uses the built-in USB-Serial/JTAG peripheral (not an external USB-UART bridge).

| Mode | VID:PID | Device Node |
|------|---------|-------------|
| Application | `303A:1001` | `/dev/ttyACM0` |
| Bootloader | `303A:1001` | `/dev/ttyACM0` |

### Entering Bootloader Mode

Hold **BOOT**, press **RESET**, release **RESET**, release **BOOT**.

## Chip Revision

This device is chip revision **v1.3**. ESP-IDF v5.5.3 defaults to requiring rev v3.01+, so the build must set:

```
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
```

These are configured in `factory_demo/sdkconfig.defaults`.

## Physical Controls

| Control | Function |
|---------|----------|
| Rotary encoder (knob) | Menu scroll, zoom, settings adjust |
| Encoder press | Select / take photo / start recording |
| Menu button | Back / return to main menu |
| Up button | Navigate up / previous item |
| Down button | Navigate down / next item |

## Build & Flash (Docker)

```bash
# Build
docker run --rm \
  -v $(pwd):/workspace -w /workspace/factory_demo \
  espressif/idf:v5.5.3 \
  bash -c ". \$IDF_PATH/export.sh && idf.py set-target esp32p4 && idf.py build"

# Flash (device must be attached to WSL)
docker run --rm \
  -v $(pwd):/workspace -w /workspace/factory_demo \
  --device=/dev/ttyACM0 \
  espressif/idf:v5.5.3 \
  bash -c ". \$IDF_PATH/export.sh && idf.py -p /dev/ttyACM0 flash"

# Monitor serial output
docker run --rm -it \
  -v $(pwd):/workspace -w /workspace/factory_demo \
  --device=/dev/ttyACM0 \
  espressif/idf:v5.5.3 \
  bash -c ". \$IDF_PATH/export.sh && idf.py -p /dev/ttyACM0 monitor"
```

## WSL2 USB Passthrough

The ESP32-P4 USB device must be attached to WSL2 using `usbipd` from a Windows PowerShell (admin):

```powershell
# List USB devices
usbipd list

# Attach Espressif device to WSL
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>

# Auto-attach (persists across reconnects)
usbipd attach --wsl --busid <BUSID> --auto-attach
```
