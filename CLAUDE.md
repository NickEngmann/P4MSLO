# CLAUDE.md — P4MSLO Build & Test Guide

## Quick Reference

- **Language**: C (ESP-IDF v5.5.3 framework)
- **Target**: ESP32-P4X-EYE development board
- **UI**: LVGL 8.3.11 with SquareLine Studio (4 screens, 7 fonts, 21 images)
- **Tests**: 60 tests across 6 suites, 17 mock headers
- **CI**: GitHub Actions — host tests, Docker tests, ESP-IDF cross-compilation

## Build & Run

### Host Tests (no ESP-IDF needed)
```bash
cd test && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
for t in test_*; do [ -x "$t" ] && ./"$t"; done
```

### LVGL Simulator (requires SDL2 + LVGL source)
```bash
cd test/simulator
git clone --depth 1 --branch v8.3.11 https://github.com/lvgl/lvgl.git
git clone --depth 1 --branch v8.3.0 https://github.com/lvgl/lv_drivers.git
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
./p4eye_sim              # Interactive SDL2 window (720x720)
./p4eye_sim --screenshot # Headless mode — dumps PPM framebuffer screenshots
```

### ESP-IDF Build (factory_demo firmware)
```bash
# Requires ESP-IDF v5.5.3 or espressif/idf:v5.5.3 Docker image
cd factory_demo
idf.py set-target esp32p4
idf.py build
```

### Docker
```bash
# Lightweight test container
docker build -f Dockerfile.test -t p4mslo-test .
docker run --rm p4mslo-test

# Full CI image (tests + ESP-IDF cross-compile)
docker build -t p4mslo-ci .
docker run --rm p4mslo-ci
```

## Testing

### Test Suites (60 tests)

| Suite | File | Tests | What it covers |
|-------|------|-------|----------------|
| NVS Storage | `test/test_nvs_storage.c` | 10 | Settings save/load, photo count, interval state, type checking |
| GPIO/BSP | `test/test_gpio_bsp.c` | 12 | Pin state, flashlight, display, I2C, SD detect, knob init |
| UI State | `test/test_ui_state.c` | 12 | Page navigation, magnification, AI mode, SD/USB transitions |
| AI Buffers | `test/test_ai_buffers.c` | 8 | Buffer init, alignment, circular index, deinit safety |
| Sleep/Wakeup | `test/test_sleep_wakeup.c` | 5 | Wakeup cause, timer+interval, GPIO wakeup |
| UI Simulator | `test/simulator/test_ui_simulator.c` | 13 | Full UI workflow, knob debounce, menu wrap, USB interrupt |

### Mock Infrastructure (17 headers in `test/mocks/`)

**Hardware**: `nvs.h`, `nvs_flash.h`, `driver/gpio.h`, `driver/i2c_master.h`, `iot_button.h`, `iot_knob.h`
**BSP**: `bsp/esp32_p4_eye.h`, `bsp/esp-bsp.h`, `bsp/display.h`, `bsp/bsp_err_check.h`
**System**: `esp_timer.h`, `esp_sleep.h`, `esp_system.h`, `esp_memory_utils.h`, `esp_log.h`, `esp_err.h`, `esp_check.h`
**UI**: `esp_lvgl_port.h`, `ui_extra.h`
**Config**: `sdkconfig.h`, `freertos/FreeRTOS.h`, `freertos/task.h`

### LVGL Simulator

Compiles real SquareLine Studio UI code against LVGL 8.3.11 with SDL2 display backend. Hardware calls stubbed in `sim_hal.c` (includes fake colored album photos). Generates 44 PNG screenshots covering all pages in `test/simulator/screenshots/`.

- **Interactive**: SDL2 window (240x240), keyboard-to-button mapping
- **Headless**: `--screenshot` generates PNG screenshots via libpng, navigates all pages using `ui_extra_goto_page()`
- **Config**: `lv_conf.h`, `lv_drv_conf.h`, `sim_config.h`
- **Screenshot map**: See `docs/simulator-screenshots.md`

## CI Pipeline (GitHub Actions)

Three parallel jobs in `.github/workflows/ci.yml`:

1. **host-tests** — Build + run 59 unit tests on `ubuntu-latest` (~30s)
2. **docker-test** — Build `Dockerfile.test`, run tests in container
3. **idf-build** — Cross-compile factory_demo in `espressif/idf:v5.5.3` container, upload firmware artifacts (~4.5min)

## Project Structure

```
P4MSLO/
├── factory_demo/              # ESP-IDF application
│   ├── main/
│   │   ├── app/              # Application logic (camera, storage, ISP, video)
│   │   ├── ui/               # SquareLine Studio generated UI
│   │   │   ├── screens/      # 4 screen definitions
│   │   │   ├── fonts/        # 7 custom fonts
│   │   │   └── images/       # 21 UI assets
│   │   └── main.c            # Entry point
│   ├── components/            # AI detection models (coco, face, pedestrian)
│   ├── sdkconfig.defaults
│   └── CMakeLists.txt
├── common_components/
│   └── esp32_p4_eye/          # Board support package
├── test/
│   ├── test_*.c               # 5 unit test files (47 tests)
│   ├── simulator/             # LVGL simulator (13 tests)
│   │   ├── sim_main.c         # SDL2 main loop + headless PNG screenshot engine
│   │   ├── sim_hal.c          # Hardware stub layer (includes fake album photos)
│   │   ├── test_ui_simulator.c
│   │   └── screenshots/       # 44 PNG screenshots covering all pages
│   ├── mocks/                 # 17 ESP-IDF mock headers
│   └── CMakeLists.txt
├── docs/                      # Documentation
│   ├── device-info.md         # ESP32-P4X-EYE hardware details, flash commands
│   └── simulator-screenshots.md # Screenshot map (44 screenshots)
├── .github/workflows/ci.yml   # CI pipeline
├── Dockerfile                 # Full CI image (tests + ESP-IDF)
└── Dockerfile.test            # Lightweight test-only image
```

## PIMSLO Stereoscopic 3D GIF Pipeline

4 ESP32-S3 cameras (OV5640 2560×1920) triggered simultaneously via ESP32-P4 GPIO34, producing oscillating 3D GIFs. Background capture + queued GIF encoding lets the user keep taking photos without waiting.

### Camera Positions (persisted in NVS)
| Device | IP | Sensor | Position |
|--------|-----|--------|----------|
| #1 | 192.168.1.119 | OV5640 5MP | 1 (leftmost) |
| #2 | 192.168.1.248 | OV5640 5MP | 2 |
| #3 | 192.168.1.66 | OV5640 5MP | 3 |
| #4 | 192.168.1.38 | OV5640 5MP | 4 (rightmost) |

Set via: `curl -X POST http://<ip>/api/v1/config/position -d '{"position":N}'`

### How It Works
1. **Photo button** → takes P4 camera photo + triggers background SPI capture
2. **SPI capture task** (Core 0) → GPIO34 trigger → receives 4 JPEGs via SPI → saves to `/sdcard/p4mslo/P4M0001/pos{1-4}.jpg`
3. **GIF encode queue** (Core 1) → picks up capture folders → encodes PIMSLO GIF → saves to `/sdcard/p4mslo_gifs/P4M0001.gif` → deletes raw JPEGs

User can take photos every ~3-4 seconds. GIF encoding happens asynchronously in background.

### Manual Serial Commands
```bash
# Trigger + SPI capture + save + encode (all-in-one)
spi_pimslo 150 0.05

# Encode from existing files on SD card
pimslo 150 0.05
# (reads /sdcard/pimslo/pos{1-4}.jpg, 150ms frame delay, 0.05 parallax)

# Check pipeline status
status
# → pimslo_queue=N pimslo_encoding=0/1
```

### Parallax Algorithm
- Square center crop from source (1920×1920 from 2560×1920)
- Horizontal parallax offset per camera position within the square
- GIF sequence: 1→2→3→4→3→2→1 (6 encoded frames, 2 replayed from PSRAM cache)
- Parallax strength 0.05 works best for separate cameras (vs 0.2 for quad-lens)

### JPEG Decoding
- **tjpgd** software decoder (standalone from LVGL, renamed to `gif_jd_prepare`/`gif_jd_decomp`)
- Handles 4:2:0 AND 4:2:2 subsampled JPEGs
- Decodes directly into crop-sized output buffer (no full-image intermediate)
- ~2.2s per frame at 2560×1920
- ESP32-P4 HW JPEG decoder only works with 4:2:0 — cannot be used for OV5640/OV3660

### SPI Bus Architecture

**Bus**: SPI3_HOST on ESP32-P4, shared MISO/MOSI/CLK with per-camera CS lines.

| Signal | P4 GPIO | Direction | Notes |
|--------|---------|-----------|-------|
| CLK | 37 | P4→S3 | |
| MOSI | 38 | P4→S3 | |
| MISO | 50 | S3→P4 | 330Ω series resistor per S3 required |
| CS0 | 51 | P4→S3 #1 | |
| CS1 | 52 | P4→S3 #2 | |
| CS2 | 53 | P4→S3 #3 | |
| CS3 | 54 | P4→S3 #4 | Dynamic slot swap (SPI3 max 3 HW CS) |
| TRIGGER | 34 | P4→all S3 | Active LOW pulse, shared by all cameras |

**Speed**: 16MHz max stable. Tested: 20/26/40MHz all fail (S3 SPI slave can't keep up).

**Throughput**: ~1.3 MB/s effective per camera. A typical 600KB JPEG transfers in ~470ms.

### SPI Signal Integrity

**Required**: 330Ω series resistors on each S3 camera's MISO output line. Without these, the shared MISO bus has contention when multiple cameras drive it (only one CS is active at a time, but floating MISO outputs cause noise).

**Recommended additional measures** (for long wires or unreliable connections):
- **100pF capacitor** on CLK near each S3 — filters high-frequency ringing on clock edges. Place between CLK and GND at the S3 end.
- **Pull-up resistor (10KΩ)** on each CS line — ensures cameras stay deselected when P4 boots. The P4 firmware pre-drives CS HIGH at init, but external pull-ups add a safety margin.
- **Keep wires short** — under 15cm for the SPI bus. Longer wires increase capacitance and reduce max clock speed.
- **MOSI**: No resistor needed (single driver, P4 only).
- **CLK**: No series resistor (single driver), but a small capacitor at each S3 end helps.

**Chunk size**: Master and slave MUST use matching 4KB (4096 byte) chunk sizes. If mismatched, the slave advances by its chunk size while the master reads less, causing data gaps that appear as zeros at the end of the transfer.

### Pipeline Timing (OV5640, 4 cameras working)

| Step | Time |
|------|------|
| GPIO34 trigger + OV5640 capture | ~600ms |
| SPI transfer × 4 cameras @ 16MHz | ~2,000ms |
| Save 4 JPEGs to SD card | ~800ms |
| **Capture + save total** | **~3,400ms** |
| | |
| GIF pass 1: palette (4 decodes) | ~11s |
| GIF pass 2: 4 forward frames | ~32s |
| GIF pass 2: 2 replayed frames | ~7s |
| **GIF encode total** | **~50s** |
| | |
| **Full pipeline (capture + encode)** | **~54s** |

GIF encoding runs in the background — the user only waits for the capture phase (~3.4s).

### SD Card Layout
```
/sdcard/
├── p4mslo/                    # PIMSLO capture directories
│   ├── P4M0001/               # First capture
│   │   ├── pos1.jpg           # Camera 1 JPEG
│   │   ├── pos2.jpg           # Camera 2 JPEG
│   │   ├── pos3.jpg           # Camera 3 JPEG
│   │   └── pos4.jpg           # Camera 4 JPEG
│   ├── P4M0002/               # Second capture (cleaned up after GIF encode)
│   └── ...
├── p4mslo_gifs/               # Completed GIF files
│   ├── P4M0001.gif            # GIF from capture 0001
│   ├── P4M0002.gif
│   └── ...
└── esp32_p4_pic_save/         # P4 camera photos (existing)
    ├── pic_0001.jpg
    └── ...
```

### S3 API Additions
| Method | Endpoint | Purpose |
|--------|----------|---------|
| POST | `/api/v1/config/position` | Set camera position 1-4 (persisted in NVS) |
| GET | `/api/v1/latest-photo` | Download the most recent JPEG directly |

## Known Issues

- **ESP-IDF v5.5.3 required** — v5.5.1/5.5.2 fail component resolution
- **PlatformIO unsupported** — ESP32-P4 requires native ESP-IDF CMake
- **Simulator camera** — Viewfinder renders empty (no camera feed, expected)
- **SDL2 + libpng dependencies** — LVGL simulator needs `libsdl2-dev` and `libpng-dev`
- **LVGL/lv_drivers** — Must be manually cloned into `test/simulator/` (gitignored)
- **Chip revision** — ESP32-P4X-EYE dev boards are rev v1.3; ESP-IDF v5.5.3 defaults to rev v3.01+ minimum
- **ESP32-P4 HW JPEG decoder** — Cannot decode 4:2:2 subsampled JPEGs (OV5640/OV3660 output). Use tjpgd software decoder instead.
- **PSRAM fragmentation** — 32MB PSRAM but largest contiguous block is ~8.26MB after boot. Buffers over 8MB fail to allocate regardless of total free memory.
- **SPI max 16MHz** — ESP32-S3 SPI slave can't sustain 20MHz+. Tested 20/26/40MHz — all fail with timeouts.
- **Single HW JPEG decoder** — The album module and GIF encoder cannot both hold a `jpeg_decoder_engine` simultaneously. The GIF encoder releases the album's decoder before encoding and reacquires after.

## Flash via Docker

```bash
docker run --rm -v $(pwd):/workspace -w /workspace/factory_demo \
  --device=/dev/ttyACM0 espressif/idf:v5.5.3 \
  bash -c ". \$IDF_PATH/export.sh && idf.py -p /dev/ttyACM0 flash"
```

## ABSOLUTE REQUIREMENTS

- **NEVER downscale resolution.** All GIFs must be encoded at the full source resolution (square crop from OV5640 2560×1920 → 1920×1920, or full 1920×1080 from P4 camera). Image quality is the #1 priority. If memory is tight, find another way (free buffers, process one frame at a time, use smaller intermediate structures) — but NEVER reduce the output resolution.
- **Camera JPEG quality minimum 4.** Never set `CAMERA_JPEG_QUALITY` below 4. Quality 2 produces non-standard Huffman tables that software decoders (tjpgd, esp_new_jpeg) cannot handle.
- **SPI chunk size must match.** P4 master and S3 slave MUST use the same chunk size (4096 bytes). Mismatch causes data corruption (slave advances by its chunk size, skipping data).

## Gotchas

1. The Dockerfile uses `espressif/idf:v5.5.1` but CI uses `v5.5.3` — use v5.5.3
2. LVGL simulator executable is `p4eye_sim`, not `p4mslo_sim`
3. NVS mock has 64-entry limit — keep test datasets small
4. GPIO mock tracks 64 pins — verify pin numbers match hardware
5. Sleep/wakeup tests need manual timer setup in mocks
6. AI buffer alignment is critical — use `esp_memory_utils` mock
7. Chip rev v1.3 needs `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and `CONFIG_ESP32P4_REV_MIN_100=y` in sdkconfig.defaults
8. `ui_extra_clear_page()` must hide `ui_PanelCameraSettings` — otherwise ISP sliders bleed into other pages
9. The popup timer (`lv_popup_timer`) is shared across pages — call `ui_extra_cancel_popup_timer()` when programmatically switching pages
10. OV5640 cameras produce 4:2:2 JPEGs at QSXGA — the P4 HW JPEG decoder rejects these with `ESP_ERR_INVALID_STATE (0x103)`. Use tjpgd software decoder.
11. SPI clock speeds above 16MHz cause all S3 cameras to timeout — the SPI slave firmware can't keep up. Don't try 20MHz+.
12. After failed high-speed SPI tests, S3 cameras may get stuck in DATA mode. OTA reflash to reset their SPI state.
13. System time defaults to 2026-01-01 (set in `main.c` via `settimeofday`). Without RTC, files would otherwise get 1980 timestamps.
