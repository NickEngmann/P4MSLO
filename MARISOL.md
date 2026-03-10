# MARISOL.md — Pipeline Context

## Project Overview
ESP32-P4X-EYE factory demo firmware featuring AI face/pedestrian detection, LVGL-based UI, and model packing utilities. The project includes host-based unit tests (59 tests across 6 suites), an LVGL simulator for desktop testing, and ESP-IDF cross-compilation for the ESP32-P4 platform. Key technologies: C, ESP-IDF v5.5.3, LVGL 8.3.11, SDL2 (simulator), pytest-style C tests.

## Build & Run
- **Language**: C (C11 standard)
- **Framework**: ESP-IDF (v5.5.3), LVGL 8.3.11, CMake
- **Docker image**: gcc:14 (for host tests), espressif/idf:v5.5.3 (for ESP-IDF builds)
- **Install deps**: 
  - Host tests: cmake, make, gcc (already in gcc:14 image)
  - Simulator: libsdl2-dev, lvgl v8.3.11, lv_drivers v8.3.0
  - ESP-IDF: espressif/idf:v5.5.3 Docker image
- **Run**:
  - Host tests: `cd test && mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) && for t in test_*; do [ -x "$t" ] && ./$t; done`
  - Simulator: `cd test/simulator && git clone --depth 1 --branch v8.3.11 https://github.com/lvgl/lvgl.git && git clone --depth 1 --branch v8.3.0 https://github.com/lvgl/lv_drivers.git && mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) && ./p4mslo_sim` (requires SDL2)
  - ESP-IDF: `cd factory_demo && idf.py set-target esp32p4 && idf.py build` (requires espressif/idf:v5.5.3)
  - Docker: `docker build -f Dockerfile.test -t p4mslo-test . && docker run --rm p4mslo-test`

## Testing
- **Test framework**: CMake + custom test harness (pytest-style output)
- **Test command**: `cd test/build && for t in test_*; do [ -x "$t" ] && ./$t; done`
- **Hardware mocks needed**: Yes — 14 mock headers in `test/mocks/`:
  - `nvs.h`: In-memory NVS with 64-entry store, namespace isolation, type checking
  - `driver/gpio.h`: 64-pin state tracking
  - `bsp/esp32_p4_eye.h`: Full BSP (flashlight, I2C, SD, knob, display, buttons)
  - `esp_timer.h`: Controllable mock timer
  - `esp_sleep.h`: Configurable wakeup cause
  - `esp_memory_utils.h`: Aligned allocation via stdlib
  - FreeRTOS stubs, esp_log printf wrappers, sdkconfig defines
- **Known test issues**: None discovered — all 59 tests pass in verified runs

## Pipeline History
- **2025-03-10**: Host tests verified — 59 tests pass (0 failures, 0 ignored)
- **2025-03-10**: LVGL simulator build requires SDL2 dev headers — not available in sandbox
- **2025-03-10**: ESP-IDF v5.5.3 confirmed required — v5.5.1/5.5.2 fail to build
- **2025-03-10**: CI workflow validates: host tests, ESP-IDF build, artifact upload

## Known Issues
- ESP-IDF v5.5.3 required (project's `idf_component.yml` constraint) — v5.5.1/5.5.2 fail
- LVGL simulator needs SDL2 dev headers (`libsdl2-dev`) — not available in all CI environments
- Camera viewfinder canvas renders as empty in simulator (no camera feed, expected)
- PlatformIO does NOT support ESP32-P4 — must use ESP-IDF CMake directly

## Notes
- **Test Suites** (59 tests total):
  - NVS Storage (`test/test_nvs_storage.c`): 10 tests — Settings save/load, photo count, interval state, type checking
  - GPIO/BSP (`test/test_gpio_bsp.c`): 12 tests — Pin state, flashlight, display, I2C, SD detect, knob init
  - UI State (`test/test_ui_state.c`): 12 tests — Page navigation, magnification, AI mode, SD/USB transitions
  - AI Buffers (`test/test_ai_buffers.c`): 8 tests — Buffer init, alignment, circular index, deinit safety
  - Sleep/Wakeup (`test/test_sleep_wakeup.c`): 5 tests — Wakeup cause, timer+interval, GPIO wakeup
  - UI Simulator (`test/simulator/test_ui_simulator.c`): 12 tests — Full UI workflow, knob debounce, menu wrap, USB interrupt
- **LVGL Simulator**: Compiles real SquareLine Studio UI code (4 screens, 7 fonts, 21 images) against LVGL 8.3.11 with SDL2 display backend. Hardware calls (camera, storage, ISP, album) stubbed in `sim_hal.c`.
- **Mock Architecture**: All hardware dependencies mocked for host testing — NVS, GPIO, BSP, timers, sleep, memory, FreeRTOS, logging.
- **CI Workflow**: `.github/workflows/ci.yml` runs host tests on push/PR, builds ESP-IDF artifacts, uploads .bin files.
- **Dockerfile.test**: Self-contained test environment with gcc:14, CMake, SDL2 dev, LVGL source.
