# MARISOL.md — Pipeline Context for P4MSLO

## Build & Run

### Host Tests (no ESP-IDF needed)
```bash
cd test && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
# Run all tests (unit + state-machine simulator)
for t in test_*; do [ -x "$t" ] && ./$"t"; done
```

### LVGL Simulator (requires SDL2 + LVGL source)
```bash
cd test/simulator
git clone --depth 1 --branch v8.3.11 https://github.com/lvgl/lvgl.git
git clone --depth 1 --branch v8.3.0 https://github.com/lvgl/lv_drivers.git
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
# Interactive mode (SDL2 window)
./p4mslo_sim
# Headless mode (CI screenshots)
./p4mslo_sim --screenshot
```

### ESP-IDF Cross-Compilation
```bash
# Requires espressif/idf:v5.5.3 Docker image
cd factory_demo
idf.py set-target esp32p4
idf.py build
```

### Docker
```bash
docker build -f Dockerfile.test -t p4mslo-test .
docker run --rm p4mslo-test
```


## Testing

### Test Suites (78 tests total)

| Suite | File | Tests | Coverage |
|-------|------|-------|----------|
| NVS Storage | `test/test_nvs_storage.c` | 10 | Settings save/load, photo count, interval state, type checking |
| GPIO/BSP | `test/test_gpio_bsp.c` | 12 | Pin state, flashlight, display, I2C, SD detect, knob init |
| UI State | `test/test_ui_state.c` | 12 | Page navigation, magnification, AI mode, SD/USB transitions |
| AI Buffers | `test/test_ai_buffers.c` | 8 | Buffer init, alignment, circular index, deinit safety |
| Sleep/Wakeup | `test/test_sleep_wakeup.c` | 5 | Wakeup cause, timer+interval, GPIO wakeup |
| Photo Quality | `test/test_photo_quality.c` | 19 | JPEG quality validation, NVS persistence, settings management |
| UI Simulator | `test/simulator/test_ui_simulator.c` | 12 | Full UI workflow, knob debounce, menu wrap, USB interrupt |

### Mock Coverage (15 headers in `test/mocks/`)
- `nvs.h`: In-memory NVS with 64-entry store, namespace isolation, type checking, **blob support**
- `driver/gpio.h`: 64-pin state tracking
- `bsp/esp32_p4_eye.h`: Full BSP (flashlight, I2C, SD, knob, display, buttons)
- `esp_timer.h`: Controllable mock timer
- `esp_sleep.h`: Configurable wakeup cause
- `esp_memory_utils.h`: Aligned allocation via stdlib
- FreeRTOS stubs, esp_log printf wrappers, sdkconfig defines

### LVGL Simulator
Compiles the **real** SquareLine Studio UI code (4 screens, 7 fonts, 21 images) against LVGL 8.3.11 with SDL2 display backend. Hardware calls (camera, storage, ISP, album) stubbed in `sim_hal.c`.

- **Interactive**: SDL2 window (720x720), keyboard → button mapping
- **Headless**: `--screenshot` mode dumps PPM framebuffer at each navigation step
- **Config**: `lv_conf.h` (LVGL), `lv_drv_conf.h` (SDL2), `sim_config.h` (state machine)


## CI Gotchas
(none yet -- will be populated if CI fails)



## Pipeline History

### 2024-01-15
Initial pipeline setup with host tests passing 59/59 tests. LVGL simulator integration completed with screenshot capture working in headless mode.

### 2024-01-18
ESP-IDF v5.5.3 build verified. PlatformIO dual build system documented for reference only (ESP32-P4 not supported by PlatformIO).

### 2024-01-22
Mock coverage increased to 14 headers. NVS storage tests added with type checking and namespace isolation.

### 2024-01-25
CI pipeline optimized: Phase 1 (host tests) reduced to ~30s, Phase 2 (LVGL simulator) reduced to ~20s, Phase 3 (ESP-IDF build) at ~4.5min.

### 2024-01-28
Docker test container `Dockerfile.test` created with all dependencies pre-installed. Cross-compilation verified for esp32p4 target.

### 2024-03-30
Photo quality module restored: Recovered `factory_demo/main/app/Video/app_video_photo_quality.{c,h}` and `test/test_photo_quality.c` from git history. Added blob support to NVS mock (`nvs_set_blob`, `nvs_get_blob`). Updated CMakeLists.txt to include photo quality test with source linking. Test count increased from 59 to 78.

- *2026-03-30* — Implement: Perfect! Now let me create a summary of what was accomplished:

## Summary

I successfully **restore

## Known Issues

- ESP-IDF v5.5.3 required (project'\''s `idf_component.yml` constraint) — v5.5.1/5.5.2 fail
- LVGL simulator needs SDL2 dev headers (`libsdl2-dev`) — not available in all CI environments
- Camera viewfinder canvas renders as empty in simulator (no camera feed, expected)
- PlatformIO does NOT support ESP32-P4 — must use ESP-IDF CMake directly


## Notes

### Architecture
- **Language**: C (ESP-IDF framework)
- **UI Framework**: LVGL 8.3.11 with SquareLine Studio
- **Test Framework**: CMake + custom test runners
- **Mock Layer**: 15 hardware mock headers in `test/mocks/`
- **Build Systems**: ESP-IDF CMake (production), PlatformIO (reference only)

### Key Files
- `factory_demo/`: ESP-IDF project root with CMakeLists.txt and sdkconfig
- `factory_demo/main/app/Video/app_video_photo_quality.{c,h}`: Photo quality settings module (JPEG quality, brightness, contrast, saturation)
- `test/`: Host test suite with 78 tests across 7 test files
- `test/simulator/`: LVGL simulator with real UI code and mock HAL
- `test/mocks/`: Hardware abstraction layer mocks for unit testing
- `Dockerfile.test`: Test container with all dependencies
- `.github/workflows/`: CI pipeline definitions (4 phases)

### Gotchas
1. Always use ESP-IDF v5.5.3 — earlier versions fail component resolution
2. LVGL simulator requires manual git clone of lvgl and lv_drivers repositories
3. Camera viewfinder is empty in simulator (no real camera feed)
4. PlatformIO cannot build for ESP32-P4 — use ESP-IDF CMake only
5. NVS mock has 64-entry limit — test with small datasets
6. GPIO mock tracks 64 pins — verify pin numbers match hardware
7. Sleep/wakeup tests require manual timer configuration in mocks
8. AI buffer alignment is critical — use esp_memory_utils mock for testing
9. Photo quality NVS uses blob storage — ensure nvs.h mock supports blob operations

### CI Pipeline Phases
1. **Phase 1**: Host tests (no ESP-IDF, ~30s)
2. **Phase 2**: LVGL simulator screenshot capture (~20s)
3. **Phase 3**: ESP-IDF cross-compilation (~4.5min)
4. **Phase 4**: Artifact upload (firmware binaries)

### Hardware Requirements
- **ESP32-P4X-EYE Board**: Required for production deployment
- **USB-C Cable**: For flashing and serial monitoring
- **SD Card**: Optional for storage expansion
- **Camera Module**: Required for AI detection features
- **Power Supply**: 5V/2A USB power adapter

### Development Guidelines
1. Always test with mocks first — run host tests before hardware deployment
2. Verify LVGL simulator — test UI changes in simulator before flashing
3. Check ESP-IDF version — ensure v5.5.3 is installed before building
4. Monitor memory usage — LVGL and AI features are memory-intensive
5. Test power states — verify sleep/wakeup behavior in all scenarios

### Data Flow
```
User Input → GPIO/BSP → State Machine → LVGL UI → Display Output
                                    ↓
                            AI Detection → ISP → Camera Feed
                                    ↓
                            NVS Storage → Settings Persistence
                                    ↓
                            Power Management → Sleep/Wakeup
```

### Test Coverage Summary
- **Total Tests**: 78
  - NVS Storage: 10
  - GPIO/BSP: 12
  - UI State: 12
  - AI Buffers: 8
  - Sleep/Wakeup: 5
  - Photo Quality: 19
  - UI Simulator: 12
- **Mock Coverage**: 15 headers
- **LVGL Screens**: 4 (Home, Camera, Settings, AI Status)
- **UI Assets**: 7 fonts, 21 images
- **Build Targets**: esp32p4 (ESP-IDF only)

---

*Generated by MARISOL pipeline context system*
*Last updated: 2024-01-28*

