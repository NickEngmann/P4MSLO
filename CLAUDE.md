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

4 ESP32-S3 cameras (OV3660 2048×1536) triggered simultaneously via ESP32-P4 GPIO34, producing oscillating 3D GIFs.

### Camera Positions (persisted in NVS)
| Device | IP | Position |
|--------|-----|----------|
| #1 | 192.168.1.119 | 1 (leftmost) |
| #2 | 192.168.1.248 | 2 |
| #3 | 192.168.1.66 | 3 |
| #4 | 192.168.1.38 | 4 (rightmost) |

Set via: `curl -X POST http://<ip>/api/v1/config/position -d '{"position":N}'`

### Capture + GIF Creation
```bash
# Full automated pipeline (trigger, download, transfer, encode on P4)
python3 debug_gifs/pimslo_capture.py

# Or manually via serial:
# 1. Transfer 4 photos to P4 SD card at /sdcard/pimslo/pos{1-4}.jpg
# 2. Send: pimslo 150 0.05
#    (150ms frame delay, 0.05 parallax strength)
```

### Parallax Algorithm
- Horizontal-only crop per position, same formula as original PIMSLO
- GIF sequence: 1→2→3→4→3→2→1 (7 oscillating frames)
- Parallax strength 0.05 works best for separate cameras (vs 0.2 for quad-lens)

### S3 API Additions
| Method | Endpoint | Purpose |
|--------|----------|---------|
| POST | `/api/v1/config/position` | Set camera position 1-4 (persisted in NVS) |
| GET | `/api/v1/latest-photo` | Download the most recent JPEG directly |

### SPI Direct Transfer (Current Method)
JPEG photos transfer directly from S3 cameras to P4 via SPI at 16MHz (~1.6MB/s).
No WiFi or host PC needed — fully autonomous pipeline.

| Step | Time |
|------|------|
| GPIO34 trigger + capture | ~700ms |
| SPI transfer × 4 cameras (sequential) | ~1,300ms |
| GIF encoding (7 frames, full resolution) | ~35s |
| **Total** | **~40s** |

Hardware: 330Ω series resistors on each S3 MISO line required for bus integrity.
SPI3_HOST on P4 supports 3 HW CS pins — camera #4 uses dynamic CS slot swapping.

### Future Improvements
- Physical camera positioning for better 3D parallax effect
- Speed optimizations to reduce total pipeline time

## Known Issues

- **ESP-IDF v5.5.3 required** — v5.5.1/5.5.2 fail component resolution
- **PlatformIO unsupported** — ESP32-P4 requires native ESP-IDF CMake
- **Simulator camera** — Viewfinder renders empty (no camera feed, expected)
- **SDL2 + libpng dependencies** — LVGL simulator needs `libsdl2-dev` and `libpng-dev`
- **LVGL/lv_drivers** — Must be manually cloned into `test/simulator/` (gitignored)
- **Chip revision** — ESP32-P4X-EYE dev boards are rev v1.3; ESP-IDF v5.5.3 defaults to rev v3.01+ minimum

## Flash via Docker

```bash
docker run --rm -v $(pwd):/workspace -w /workspace/factory_demo \
  --device=/dev/ttyACM0 espressif/idf:v5.5.3 \
  bash -c ". \$IDF_PATH/export.sh && idf.py -p /dev/ttyACM0 flash"
```

## ABSOLUTE REQUIREMENTS

- **NEVER downscale resolution.** All GIFs must be encoded at the full source resolution (e.g., 2048x1536 from OV3660, 1920x1080 from P4 camera). Image quality is the #1 priority. If memory is tight, find another way (free buffers, process one frame at a time, use smaller intermediate structures) — but NEVER reduce the output resolution.

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
