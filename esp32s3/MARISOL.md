# MARISOL.md — Moment PIMSLO Camera

## Project Overview

**Moment PIMSLO** is a triggered point-and-shoot camera firmware for the Seeed Studio XIAO ESP32-S3 Sense. When D0 (GPIO 0) is pulled LOW externally, it captures a maximum-quality JPEG and saves it to a FAT32 SD card. HTTP API for remote capture, photo download, and OTA firmware updates. No continuous recording, no BLE, no audio — just photos.

- **MCU**: ESP32-S3 (Xtensa dual-core, 240 MHz, 8MB flash, 8MB PSRAM)
- **Camera**: OV5640 or OV3660 (auto-detect) on Sense expansion board
- **Storage**: MicroSD via SPI, FAT32 filesystem
- **Build System**: PlatformIO + ESP-IDF framework (NOT Arduino)
- **WiFi**: Hardcoded dual-SSID (primary + backup)

## Build & Run

```bash
# Build firmware
pio run -e xiao_esp32s3

# Flash via USB (hold BOOT, press RESET, release BOOT first)
pio run -e xiao_esp32s3 --target upload --upload-port /dev/ttyACM0

# Flash via OTA (device must be on WiFi)
curl -X POST http://<ip>/api/v1/ota/upload \
  --data-binary @.pio/build/xiao_esp32s3/firmware.bin \
  -H "Content-Type: application/octet-stream"
```

## Framework: ESP-IDF (not Arduino)

- Entry point is `app_main()` in `src/main.cpp`
- FreeRTOS tasks created with `xTaskCreatePinnedToCore()`
- No `Serial.println()` — use `ESP_LOGI(TAG, "message")`
- CRC, NVS, WiFi, HTTP server all via ESP-IDF APIs

## Architecture

```
D0 Trigger ──► Capture Task ──► Camera (fb_get) ──► Save to /sdcard/DCIM/IMG_NNNN.jpg
                                     │
HTTP POST /capture ──────────────────┘
HTTP GET /photos ──────────────────────────────── list files from SD
HTTP GET /photo/IMG_0001.jpg ──────────────────── serve file from SD
HTTP POST /ota/upload ──────────────────────────── flash new firmware
```

## HTTP API

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/v1/status` | Device info, camera, SD, photo count |
| POST | `/api/v1/capture` | Trigger photo capture |
| GET | `/api/v1/photos` | List all photos (JSON) |
| GET | `/api/v1/photo/<name>` | Download a photo (wildcard URI match) |
| POST | `/api/v1/ota/upload` | Upload firmware binary directly |
| POST | `/api/v1/ota` | OTA from URL: `{"url": "http://..."}` |
| POST | `/api/v1/sd/format` | Format SD (requires `{"confirm": true}`) |
| POST | `/api/v1/factory-reset` | Clear NVS + reboot (requires `{"confirm": true}`) |

## RTOS Task Layout

```
Task              Core  Priority  Stack  Notes
────────────────────────────────────────────────
Trigger monitor   0     10        4KB    Polls D0, captures on falling edge
StatusLED         0     5         4KB    LED animation + periodic IP/status log (every 10s)
```

## WiFi

Hardcoded dual-SSID:
- Primary: "The Garden" / "SanDiego2019!"
- Backup: "NYCR24" / "clubmate"

Auto-fallback after 5 failures on primary. Reconnects automatically. Power save disabled for HTTP server responsiveness.

## Key Design Decisions

- **No BLE**: WiFi creds hardcoded, no provisioning needed. Saves ~200KB flash.
- **FAT32 not raw circular buffer**: Photos are individual JPEG files, readable on any computer. No custom format, no parser needed.
- **Single fb_count=1**: Point-and-shoot doesn't need double-buffering. One frame buffer, capture on demand.
- **Max resolution + quality 4**: OV5640 → 2592x1944 (5MP), OV3660 → 2048x1536 (3MP). Quality 4 = lowest compression = best detail.
- **OTA via direct POST upload**: No need for external HTTP server. Just `curl --data-binary @firmware.bin` to the device.
- **GPIO 0 trigger**: Strapping pin, but works as trigger with pull-up + momentary pulse. Must not be held LOW during reset.

## SD Card Notes

- FAT32 filesystem via `esp_vfs_fat_sdspi_mount()`
- Photos at `/sdcard/DCIM/IMG_NNNN.jpg`, counter in NVS
- 128MB SDSC cards work (tested). Speed: 400kHz (probing mode for old cards)
- `format_if_mount_failed = true` — auto-formats non-FAT32 cards
- 128MB card holds ~200-400 photos at max quality

## sdkconfig Gotchas

**`sdkconfig.xiao_esp32s3` caching**: PlatformIO generates this file on first build and NEVER regenerates it. `pio run -t clean` does NOT delete it. Changes to `sdkconfig.defaults` are silently ignored until you `rm sdkconfig.xiao_esp32s3`. Fixed by adding `board_build.sdkconfig_defaults` to platformio.ini and gitignoring the generated file.

## WSL2 Serial Gotchas

- **DTR/RTS ioctl timeout**: WSL2's USB passthrough doesn't reliably support DTR/RTS control. esptool's auto-reset often fails with `OSError: [Errno 62] Timer expired`. Fix: put device in download mode manually (hold BOOT, press RESET, release BOOT).
- **USB CDC output**: The XIAO ESP32-S3 uses native USB CDC. Serial output only flows after boot; reconnecting mid-session may not show output. Replug USB to trigger reboot + capture.
- **`upload_flags = --before=no_reset`**: Added to platformio.ini to skip the failing auto-reset.

## File Quick Reference

| File | What It Does |
|------|-------------|
| `src/main.cpp` | Entry point, trigger monitor, capture logic, NVS counter |
| `src/config/Config.h` | All constants: pins, WiFi creds, quality, paths |
| `src/camera/CameraManager.*` | OV5640/OV3660 auto-detect, max-res JPEG capture |
| `src/storage/SDCardManager.*` | FAT32 mount, save/list/read photos |
| `src/network/WiFiManager.*` | Dual-SSID WiFi with fallback |
| `src/network/HttpServer.*` | REST API (8 endpoints including OTA upload) |
| `src/ota/OTAManager.*` | OTA via HTTP POST or URL download |
| `src/led/StatusLED.*` | NeoPixel state machine (6 states) |
| `scripts/patch_camera_lib.py` | Pre-build: patches esp32-camera GDMA + PLL |

## Pipeline History

| Date | Phase | Result |
|------|-------|--------|
| 2026-03-15 — 2026-03-18 | Moment v1 | Continuous-recording camera: 720p@30fps MJPEG, raw circular buffer, BLE provisioning, Android companion app, clip export, film overlays. See main branch. |
| 2026-04-13 | PIMSLO rewrite | Complete rewrite as point-and-shoot. Removed: BLE, audio, circular buffer, frame index, stream muxer, clip exporter, camera effects, preview, ConfigStore. Added: FAT32 SD, D0 trigger, direct OTA upload endpoint, hardcoded WiFi. Camera: OV3660 @ 2048x1536, quality 4. 8 HTTP endpoints. |
| 2026-04-13 | Hardware testing | Flashed 2 devices. OV3660 detected, 2048x1536 JPEG @ 250-500KB. SD card (128MB SDSC) mounted as FAT32. WiFi connected. All API endpoints verified: status, capture, photo list, photo download, OTA upload (version change + reboot + rollback). StatusLED stack overflow fixed (2KB→4KB). |
| 2026-04-13 | OTA verified | Direct firmware upload via `POST /api/v1/ota/upload` — 1.09MB binary uploaded, device rebooted to new version, all services came back. Two-way OTA tested (v0.2.0 → v0.2.1-ota → v0.2.0). Photos preserved across OTA reboots. |
