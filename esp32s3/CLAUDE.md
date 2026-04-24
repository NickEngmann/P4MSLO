# CLAUDE.md — Instructions for AI Assistants

## Project

Moment PIMSLO (P4MSLO) — a triggered point-and-shoot camera on the XIAO ESP32-S3 Sense. NOT a continuous recorder. Captures max-quality JPEG on D0 trigger, saves to FAT32 SD card. HTTP API for remote control and OTA updates.

**4 devices are currently deployed and running on WiFi.**

## Build

```bash
pio run -e xiao_esp32s3                    # Build
pio run -e xiao_esp32s3 --target upload    # Flash via USB (device #1 on /dev/ttyACM0)
```

**WSL2 USB flashing**: The DTR/RTS reset often fails. Put device in download mode first: **hold BOOT button, press RESET, release BOOT**. Then run the upload command.

## Framework

ESP-IDF (NOT Arduino). Entry point: `app_main()` in `src/main.cpp`. Use `ESP_LOGI()` not `Serial.println()`.

## Source Layout

```
src/main.cpp                 # Entry point, trigger monitor, capture logic
src/config/Config.h          # All constants: pins, WiFi creds, quality
src/camera/CameraManager.*   # OV5640/OV3660 auto-detect, max-res JPEG capture
src/storage/SDCardManager.*  # FAT32 mount, save/list/read photos
src/network/WiFiManager.*    # Dual-SSID WiFi with fallback
src/network/HttpServer.*     # REST API (8 endpoints)
src/ota/OTAManager.*         # OTA via POST upload or URL download
src/led/StatusLED.*          # NeoPixel (6 states)
```

## HTTP API

Base URL: `http://<device-ip>/api/v1`

| Method | Endpoint | Purpose |
|--------|----------|---------|
| GET | `/status` | Device info: version, camera, SD space, photo count, uptime |
| POST | `/capture` | Trigger a photo capture, returns `{"status":"capture_triggered"}` |
| GET | `/photos` | List all photos as JSON: `{"photos":["IMG_0001.jpg",...],"count":N}` |
| GET | `/photo/IMG_0001.jpg` | Download a specific JPEG photo (wildcard match) |
| POST | `/ota/upload` | Upload firmware binary in request body → flash + reboot |
| POST | `/ota` | OTA from URL: send `{"url":"http://..."}` |
| POST | `/sd/format` | Format SD card: send `{"confirm":true}` |
| POST | `/factory-reset` | Clear NVS + reboot: send `{"confirm":true}` |

## Active Devices (LIVE RIGHT NOW)

| Device | IP Address | Camera | SD Card | Status |
|--------|------------|--------|---------|--------|
| #1 | **192.168.1.119** | OV5640 2560x1920 | SD disabled (SPI mode) | Running |
| #2 | **192.168.1.248** | OV5640 2560x1920 | SD disabled (SPI mode) | Running |
| #3 | **192.168.1.66** | OV5640 2560x1920 | SD disabled (SPI mode) | Running |
| #4 | **192.168.1.38** | OV5640 2560x1920 | SD disabled (SPI mode) | Running |

All on WiFi "The Garden". Cameras switched from OV3660 (3MP) to OV5640 (5MP).
SD card is disabled when SPI slave mode is active (shares pins).

## How to Verify All Devices Are Up

```bash
for ip in 192.168.1.119 192.168.1.248 192.168.1.66 192.168.1.38; do
    echo -n "$ip: "
    curl -s --connect-timeout 2 "http://$ip/api/v1/status" | python3 -c "import sys,json; d=json.load(sys.stdin); print(f'UP | {d[\"sensor\"]} | Photos: {d[\"photo_count\"]} | Uptime: {d[\"uptime_ms\"]//1000}s')" 2>/dev/null || echo "OFFLINE"
done
```

## How to Take a Photo (Remote Trigger)

```bash
# Single device
curl -X POST http://192.168.1.119/api/v1/capture

# All 4 devices simultaneously
for ip in 192.168.1.119 192.168.1.248 192.168.1.66 192.168.1.38; do
    curl -s -X POST "http://$ip/api/v1/capture" &
done
wait
```

## How to Download Photos

```bash
# List photos on a device
curl http://192.168.1.119/api/v1/photos

# Download a specific photo
curl http://192.168.1.119/api/v1/photo/IMG_0001.jpg --output photo.jpg

# Download all photos from a device
for photo in $(curl -s http://192.168.1.119/api/v1/photos | python3 -c "import sys,json; [print(p) for p in json.load(sys.stdin)['photos']]"); do
    curl -s "http://192.168.1.119/api/v1/photo/$photo" --output "$photo"
done
```

## How to OTA Update All Devices

```bash
# 1. Build the firmware
pio run -e xiao_esp32s3

# 2. Flash all 4 devices via OTA (parallel)
for ip in 192.168.1.119 192.168.1.248 192.168.1.66 192.168.1.38; do
    echo "Flashing $ip..."
    curl -s -X POST "http://$ip/api/v1/ota/upload" \
        --data-binary @.pio/build/xiao_esp32s3/firmware.bin \
        -H "Content-Type: application/octet-stream" \
        --max-time 120 &
done
wait
echo "All devices flashed. Waiting for reboot..."

# 3. Verify all devices came back
sleep 15
for ip in 192.168.1.119 192.168.1.248 192.168.1.66 192.168.1.38; do
    echo -n "$ip: "
    curl -s --connect-timeout 3 "http://$ip/api/v1/status" | python3 -c "import sys,json; d=json.load(sys.stdin); print(f'v{d[\"version\"]} UP')" 2>/dev/null || echo "OFFLINE"
done
```

## How to Flash via USB (Device #1 Only)

Device #1 is connected via USB at `/dev/ttyACM0`:
```bash
# Put in download mode: hold BOOT, press RESET, release BOOT
pio run -e xiao_esp32s3 --target upload --upload-port /dev/ttyACM0
```

## WiFi Credentials

Hardcoded in `src/config/Config.h`:
- **Primary**: SSID `The Garden` / PSK `SanDiego2019!`
- **Backup**: SSID `NYCR24` / PSK `clubmate`

## Key Gotchas

- **sdkconfig caching**: PlatformIO caches `sdkconfig.xiao_esp32s3` and ignores `sdkconfig.defaults`. Delete the cached file to apply changes. It's gitignored.
- **WSL2 USB serial**: DTR/RTS ioctl fails. Put device in download mode manually before flashing.
- **GPIO 0 strapping**: D0 is the boot mode pin. External trigger must be momentary LOW pulse, not sustained.
- **StatusLED stack**: Must be 4KB+ (not 2KB) due to periodic status logging.
- **128MB SD cards**: SDSC type, probing speed (400kHz). These are small old cards.
- **No BLE**: Bluetooth disabled. WiFi creds hardcoded.
- **Camera GDMA patch**: `scripts/patch_camera_lib.py` patches esp32-camera v2.0.4 GDMA allocation.
- **OTA direct upload**: The preferred OTA method is `POST /api/v1/ota/upload` with the firmware.bin as the request body. No external HTTP server needed.
- **Photo quality**: JPEG quality factor 4 (lowest number = highest quality). Never go below 4 — quality 2 produces non-standard Huffman tables that software decoders can't handle.
- **OV5640 vs OV3660**: Firmware auto-detects both. OV5640 produces 2560×1920 JPEGs (~500-900KB at quality 4). OV3660 produces 2048×1536. Both use 4:2:2 subsampling.
- **SPI slave chunk size**: 4096 bytes. The P4 master MUST use the same chunk size or data will be corrupted (slave advances by its full chunk size regardless of how much the master read).
- **SPI slave stuck in DATA mode**: If the P4 master disconnects mid-transfer or uses the wrong clock speed, the S3 SPI slave can get stuck. OTA reflash to reset. If `cam_wifi_on all` broadcast from the P4 leaves that one camera's HTTP endpoint unreachable, its WiFi task is not even starting and OTA won't help — physical USB reflash required (or check wiring — missing 330 Ω MISO series resistor on a single cam can cause persistent silence on that CS line even after reboot).
- **SPI capture has NO retries** — `spi_camera_send_control` still burst-retries the *command byte* 10× to tolerate the slave's busy windows, but the `poll_and_get_size` → `spi_receive_jpeg_chunked` flow runs **once** per camera per trigger on the P4 side (`SPI_MAX_RETRIES 0` in `spi_camera.c`). User choice: retries were masking slow/stuck slaves and making tests run for 20+ minutes. If a cam doesn't respond within the 2000 ms poll timeout, the pipeline accepts whatever ≥2 cams did respond and enqueues the encode from those. Reintroducing retries requires bumping `SPI_MAX_RETRIES` and revalidating the end-to-end test suite for total-runtime regressions.
- **Status LED reflects capture activity** — `src/main.cpp` registers `setTransferStartCallback` / `setTransferEndCallback` hooks on `spiSlave` that flip `statusLED` to `CAPTURING` (blue) when the P4 reads a JPEG and back to `READY` (green) when the transfer completes. Lets you see which cameras actually saw the trigger + transferred during a `spi_pimslo` run without opening the serial log. Hooks run from the SPI task context; avoid anything blocking or heap-allocating inside the callback.
- **Max-perf S3 build** — `sdkconfig.defaults` sets `CONFIG_COMPILER_OPTIMIZATION_PERF=y` (global `-O2`) and `platformio.ini` sets `-DLOG_LOCAL_LEVEL=ESP_LOG_INFO`. The S3's SPI slave needs every cycle to keep up with the 16 MHz master clock. Don't drop this to `-Og` "to debug" — the slave misses edges and the P4 master times out even though wiring is fine. If you add a new `sdkconfig.defaults` knob, remember PIO caches `sdkconfig.xiao_esp32s3` — delete that cached file for the new default to apply.
