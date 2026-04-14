# Moment PIMSLO — Point-and-Shoot Camera

> Trigger. Capture. Done.

Firmware for the Moment PIMSLO camera — a triggered point-and-shoot camera on the Seeed Studio XIAO ESP32-S3 Sense. When D0 is pulled LOW (external trigger), it captures a maximum-quality JPEG and saves it to a FAT32 SD card. HTTP API for remote capture, photo download, and OTA firmware updates.

## Hardware

| Component | Chip / Spec | Interface | Notes |
|-----------|------------|-----------|-------|
| **Board** | Seeed Studio XIAO ESP32-S3 Sense | — | Dual-core 240 MHz, 8MB flash, 8MB PSRAM |
| **Camera** | OV5640 or OV3660 (auto-detect) | DVP (parallel) | Max resolution JPEG, quality factor 4 |
| **Storage** | MicroSD (SPI mode, FAT32) | SPI (CS=21, SCK=7, MISO=8, MOSI=9) | Regular filesystem, JPEGs saved as files |
| **NeoPixel** | External WS2812 | GPIO 1 (RMT) | Status LED (green=ready, white=capturing, red=no SD) |
| **Trigger** | External signal on D0 | GPIO 0 (active LOW) | Pull-up enabled, debounced 100ms |
| **WiFi** | ESP32-S3 built-in | — | Hardcoded dual-SSID with fallback |

## Quick Start

```bash
# Install PlatformIO
pip install platformio

# Build firmware
pio run -e xiao_esp32s3

# Flash via USB (put device in download mode: hold BOOT, press RESET, release BOOT)
pio run -e xiao_esp32s3 --target upload --upload-port /dev/ttyACM0

# Flash via OTA (device must be running and on WiFi)
curl -X POST http://<device-ip>/api/v1/ota/upload \
  --data-binary @.pio/build/xiao_esp32s3/firmware.bin \
  -H "Content-Type: application/octet-stream"
```

## HTTP REST API

Base URL: `http://<device-ip>/api/v1`

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/status` | Device info: version, camera, SD space, photo count, uptime |
| POST | `/capture` | Trigger a photo capture |
| GET | `/photos` | List all photos as JSON array |
| GET | `/photo/IMG_0001.jpg` | Download a specific photo |
| POST | `/ota/upload` | Upload firmware binary directly (OTA update + reboot) |
| POST | `/ota` | Trigger OTA from URL: `{"url": "http://..."}` |
| POST | `/sd/format` | Format SD card (requires `{"confirm": true}`) |
| POST | `/factory-reset` | Clear NVS and reboot (requires `{"confirm": true}`) |

### Example Usage

```bash
# Check status
curl http://192.168.1.248/api/v1/status

# Take a photo remotely
curl -X POST http://192.168.1.248/api/v1/capture

# List photos
curl http://192.168.1.248/api/v1/photos

# Download a photo
curl http://192.168.1.248/api/v1/photo/IMG_0001.jpg --output photo.jpg

# OTA firmware update (direct upload)
curl -X POST http://192.168.1.248/api/v1/ota/upload \
  --data-binary @firmware.bin -H "Content-Type: application/octet-stream"
```

## Architecture

```
D0 Trigger (GPIO 0) ──► Capture Task ──► OV3660/OV5640 ──► JPEG ──► FAT32 SD Card
                                              │                        │
HTTP API ──► /capture ─────────────────────────┘                        │
         ──► /photos ──────────────────────────────────────────────── list
         ──► /photo/IMG_NNNN.jpg ──────────────────────────────────── read
         ──► /ota/upload ──► esp_ota_ops ──► reboot
```

### Boot Sequence

1. NeoPixel → yellow (booting)
2. NVS → load photo counter
3. Camera → auto-detect OV5640/OV3660, init at max resolution, quality 4
4. SD Card → FAT32 mount, create DCIM directory
5. WiFi → connect to primary SSID, fallback to backup after 5 failures
6. HTTP server + OTA → start on port 80

### RTOS Tasks

| Task | Core | Priority | Stack | Purpose |
|------|------|----------|-------|---------|
| Trigger monitor | 0 | 10 | 4KB | Polls D0, captures on falling edge |
| StatusLED | 0 | 5 | 4KB | LED animation + periodic status log |

### WiFi Configuration

Hardcoded dual-SSID with automatic fallback:
- **Primary**: "The Garden" / "SanDiego2019!"
- **Backup**: "NYCR24" / "clubmate"

Tries primary first. After 5 consecutive failures, switches to backup. Reconnects automatically on disconnect.

### LED States

| State | Pattern | Color | Meaning |
|-------|---------|-------|---------|
| Booting | Solid | Yellow | Initializing |
| Ready | Slow pulse | Green | Idle, waiting for trigger |
| Capturing | Flash | White | Taking a photo |
| No SD Card | Fast blink | Red | SD card missing or failed |
| WiFi Connecting | Rapid blink | Blue | Connecting to WiFi |
| OTA Update | Breathe | Orange | Firmware update in progress |

### Photo Storage

- Photos saved to `/sdcard/DCIM/IMG_NNNN.jpg`
- Counter persisted in NVS across reboots
- FAT32 filesystem — SD card readable on any computer
- 128MB SD card holds ~200-400 photos at max quality (300-500KB each)

### OTA Updates

Two methods:
1. **Direct upload**: `POST /api/v1/ota/upload` with firmware binary in body — simplest, works from any machine on the network
2. **URL download**: `POST /api/v1/ota` with `{"url": "http://..."}` — device downloads from specified URL

After successful OTA, device reboots to new firmware. Dual OTA partitions with automatic rollback if new firmware fails to validate within 30 seconds.

## Project Structure

```
Moment-v2/
├── platformio.ini              # Build config (xiao_esp32s3 only)
├── partitions.csv              # 8MB flash: dual OTA + NVS
├── sdkconfig.defaults          # ESP-IDF config (240MHz, 32KB TCP, no BT)
├── scripts/
│   └── patch_camera_lib.py     # Pre-build: patches esp32-camera GDMA + PLL
├── src/
│   ├── main.cpp                # Entry point, trigger monitor, capture loop
│   ├── config/Config.h         # All compile-time constants
│   ├── camera/CameraManager.*  # OV5640/OV3660 auto-detect, max-res capture
│   ├── storage/SDCardManager.* # FAT32 mount, save/list/read photos
│   ├── network/
│   │   ├── WiFiManager.*       # Dual-SSID WiFi with fallback
│   │   └── HttpServer.*        # REST API (8 endpoints)
│   ├── ota/OTAManager.*        # OTA via direct upload or URL download
│   └── led/StatusLED.*         # NeoPixel status LED (6 states)
└── docs/
```

## GPIO 0 (D0) Trigger — Important Note

GPIO 0 is the ESP32-S3 boot mode strapping pin. If held LOW during reset, the chip enters download mode. The external trigger circuit **must**:
- Use a pull-up resistor (internal pull-up is enabled in firmware)
- Provide only momentary LOW pulses (not sustained)
- Not hold D0 low during power-on or reset

## Dependencies

| Library | Purpose |
|---------|---------|
| `espressif/esp32-camera@^2.0.4` | Camera driver (GDMA-patched via `patch_camera_lib.py`) |
| `codewitch-honey-crisis/htcw_rmt_led_strip` | WS2812 NeoPixel via RMT |

## Tested Hardware

| Camera | Resolution | Photo Size | Status |
|--------|-----------|------------|--------|
| OV3660 | 2048x1536 (3MP) | 250-500KB | Verified |
| OV5640 | 2592x1944 (5MP) | 400-800KB | Supported (auto-detect) |

## License

MIT License
