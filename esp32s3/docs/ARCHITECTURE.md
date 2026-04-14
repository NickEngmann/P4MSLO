# Moment PIMSLO — Architecture

## System Overview

```
                    XIAO ESP32-S3 Sense
┌─────────────────────────────────────────────────────┐
│                                                     │
│  D0 (GPIO 0) ──► Trigger Task ──► esp_camera_fb_get │
│                       │                    │        │
│                       │              OV3660/OV5640   │
│                       │              2048x1536 JPEG  │
│                       ▼                    │        │
│               SDCardManager ◄──────────────┘        │
│               /sdcard/DCIM/IMG_NNNN.jpg             │
│                       │                             │
│  WiFi ◄──────► HttpServer (port 80)                 │
│  "The Garden"    │  GET /status                     │
│  "NYCR24"        │  POST /capture                   │
│                  │  GET /photos, /photo/*            │
│                  │  POST /ota/upload                 │
│                  │  POST /factory-reset              │
│                                                     │
│  NeoPixel (GPIO 1) ◄── StatusLED task               │
│                                                     │
└─────────────────────────────────────────────────────┘
```

## Boot Sequence

| Step | Component | What Happens |
|------|-----------|-------------|
| 1 | NeoPixel | Init on GPIO 1, set yellow (booting) |
| 2 | NVS | Load photo counter from flash |
| 3 | Camera | Auto-detect OV5640/OV3660, init at max resolution, quality 4 |
| 4 | SD Card | FAT32 mount via `esp_vfs_fat_sdspi_mount`, create /sdcard/DCIM |
| 5 | WiFi | Connect to primary SSID, fallback to backup after 5 failures |
| 6 | HTTP + OTA | Start HTTP server on port 80, mark firmware valid |

## Capture Flow

```
1. D0 goes LOW (external trigger) or POST /capture received
2. captureRequested flag set
3. Trigger task detects flag
4. LED → white (CAPTURING)
5. esp_camera_fb_get() → JPEG in PSRAM (~250-500KB)
6. SDCardManager::savePhoto() → /sdcard/DCIM/IMG_NNNN.jpg
7. esp_camera_fb_return()
8. Photo counter incremented + saved to NVS
9. LED → green (READY)
```

## OTA Update Flow

### Direct Upload (preferred)
```
1. Client sends: POST /api/v1/ota/upload with firmware.bin as body
2. HttpServer receives in 4KB chunks
3. Each chunk written to inactive OTA partition via esp_ota_write()
4. esp_ota_end() verifies the image
5. esp_ota_set_boot_partition() sets new boot target
6. JSON response sent: {"status": "ota_complete", "bytes": N}
7. esp_restart() after 2 second delay
8. Device boots new firmware, calls esp_ota_mark_app_valid_cancel_rollback()
```

### URL Download
```
1. Client sends: POST /api/v1/ota with {"url": "http://..."}
2. Separate FreeRTOS task spawned for download
3. esp_http_client downloads firmware
4. Written to OTA partition in 4KB chunks
5. Reboot on success
```

## Hardware Pin Map

| GPIO | Function | Notes |
|------|----------|-------|
| 0 | D0 Trigger | Active LOW, pull-up enabled, strapping pin |
| 1 | NeoPixel | External WS2812 data |
| 7 | SD SCK | SPI clock |
| 8 | SD MISO | SPI data in |
| 9 | SD MOSI | SPI data out |
| 10 | Camera XCLK | 20 MHz clock to sensor |
| 11-18 | Camera D0-D7 | DVP parallel data |
| 21 | SD CS | SPI chip select |
| 38 | Camera VSYNC | Vertical sync |
| 39 | Camera SCL | SCCB clock (I2C) |
| 40 | Camera SDA | SCCB data (I2C) |
| 47 | Camera HREF | Horizontal reference |
| 48 | Camera D7 | Data bit 7 |

## SD Card Details

- Filesystem: FAT32 via ESP-IDF VFS
- Mount point: `/sdcard`
- Photo directory: `/sdcard/DCIM/`
- Filename format: `IMG_0001.jpg`, `IMG_0002.jpg`, ...
- Counter persisted in NVS key `photo_cnt`
- SPI bus: SPI3_HOST at probing speed (400kHz for SDSC compatibility)
- Allocation unit: 4KB (optimized for small cards)
- Auto-format: if card is not FAT32, formats on first mount

## Memory Budget

| Resource | Usage |
|----------|-------|
| Flash (firmware) | ~1.09 MB / 1.88 MB (55%) |
| SRAM | ~42 KB / 327 KB (13%) |
| PSRAM | ~600 KB frame buffer + heap |
| Free heap at runtime | ~7.8 MB |

## Deployed Devices

| Device | MAC (last 4) | IP | SD Size |
|--------|-------------|-----|---------|
| #1 | D9:E0 | 192.168.1.119 | 120 MB |
| #2 | — | 192.168.1.248 | 120 MB |
| #3 | DA:A4 | 192.168.1.66 | 120 MB |
| #4 | — | 192.168.1.38 | 120 MB |
