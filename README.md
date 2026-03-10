# ESP32-P4X-EYE Development Board

## User Guide

* ESP32-P4X-EYE - [English](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html) / [中文](https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32p4/esp32-p4x-eye/user_guide.html)

## Examples

The examples are developed under the ESP-IDF **release/v5.5** branch. The [Factory Demo](./examples/factory_demo/) is the factory firmware of the development board.

## Factory Bin

* [Factory Bin](https://dl.espressif.com/AE/esp-dev-kits/p4x_eye_factory_demo_110.bin) for ESP32-P4X-EYE, programmed with the [Factory Demo](./examples/factory_demo/) example.

<a href="https://espressif.github.io/esp-launchpad/?flashConfigURL=https://espressif2022.github.io/ESP32-P4-Function-EV-Board/launchpad.toml">
    <img alt="Try it with ESP Launchpad" src="https://dl.espressif.com/AE/esp-dev-kits/new_launchpad.png" width="316" height="100">
</a>

Experience more examples instantly with the [ESP-LaunchPad](https://espressif.github.io/esp-launchpad/?flashConfigURL=https://espressif2022.github.io/ESP32-P4-Function-EV-Board/launchpad.toml).

**Note**:
* Firmware files with the `p4x_` prefix are for ESP32-P4X boards. Firmware files with the `p4_` prefix are for the original ESP32-P4 boards.

---

## Project Overview

This repository contains the factory demo firmware for the ESP32-P4X-EYE development board. The firmware implements:

- **AI Face/Pedestrian Detection**: Real-time object detection using ESP32-P4's AI accelerators
- **LVGL UI**: Interactive touchscreen interface built with SquareLine Studio
- **NVS Storage**: Persistent settings management for user preferences
- **Hardware Integration**: GPIO control, I2C peripherals, SD card, USB disk, and rotary knob
- **Power Management**: Sleep/wakeup mechanisms with multiple trigger sources

The project uses ESP-IDF v5.5.3 and requires the ESP32-P4X-EYE hardware for full functionality.

---

## Development Environment

### Prerequisites

- **ESP-IDF v5.5.3**: Required for ESP32-P4X-EYE compilation (v5.5.1/5.5.2 fail)
- **CMake 3.16+**: For test suite and simulator builds
- **GCC 14+**: For cross-compilation and testing
- **Python 3.x**: For ESP-IDF toolchain
- **SDL2 development headers**: Required for LVGL simulator (not available in all CI environments)

### Hardware Requirements

- **ESP32-P4X-EYE Development Board**: Required for actual firmware deployment
- **Linux/Ubuntu host**: For building and testing

**Note**: PlatformIO does NOT support ESP32-P4 — must use ESP-IDF CMake directly.

---

## Building Firmware

### ESP-IDF Cross-Compilation

```bash
cd factory_demo
idf.py set-target esp32p4
idf.py build
```

This compiles the factory demo firmware for the ESP32-P4X-EYE board. The output binary can be flashed using ESP-LaunchPad or esptool.

### Docker Build

```bash
docker build -f Dockerfile.test -t p4mslo-test .
docker run --rm p4mslo-test
```

The Docker image `gcc:14` is used for building and testing in CI environments.

---

## Testing

### Host Tests (No Hardware Required)

The test suite runs on Linux hosts without ESP-IDF or hardware dependencies. All 59 tests pass successfully.

#### Running Tests

```bash
cd test
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

# Run all tests (unit + state-machine simulator)
for t in test_*; do [ -x "$t" ] && ."/$t"; done
```

#### Test Suites

| Suite | File | Tests | Coverage |
|-------|------|-------|----------|
| NVS Storage | `test/test_nvs_storage.c` | 10 | Settings save/load, photo count, interval state, type checking |
| GPIO/BSP | `test/test_gpio_bsp.c` | 12 | Pin state, flashlight, display, I2C, SD detect, knob init |
| UI State | `test/test_ui_state.c` | 12 | Page navigation, magnification, AI mode, SD/USB transitions |
| AI Buffers | `test/test_ai_buffers.c` | 8 | Buffer init, alignment, circular index, deinit safety |
| Sleep/Wakeup | `test/test_sleep_wakeup.c` | 5 | Wakeup cause, timer+interval, GPIO wakeup |
| UI Simulator | `test/simulator/test_ui_simulator.c` | 12 | Full UI workflow, knob debounce, menu wrap, USB interrupt |

**Total**: 59 tests, 0 failures

### Mock Architecture

The test suite uses 14 mock headers in `test/mocks/` to simulate hardware:

- **`nvs.h`**: In-memory NVS with 64-entry store, namespace isolation, type checking
- **`driver/gpio.h`**: 64-pin state tracking
- **`bsp/esp32_p4_eye.h`**: Full BSP (flashlight, I2C, SD, knob, display, buttons)
- **`esp_timer.h`**: Controllable mock timer
- **`esp_sleep.h`**: Configurable wakeup cause
- **`esp_memory_utils.h`**: Aligned allocation via stdlib
- FreeRTOS stubs, esp_log printf wrappers, sdkconfig defines

---

## LVGL Simulator

The LVGL simulator runs the **real** SquareLine Studio UI code (4 screens, 7 fonts, 21 images) against LVGL 8.3.11 with SDL2 display backend on desktop Linux. No fake renderers — all widgets, fonts, images, and the UI state machine work exactly as on hardware.

### Setup

```bash
cd test/simulator
git clone --depth 1 --branch v8.3.11 https://github.com/lvgl/lvgl.git
git clone --depth 1 --branch v8.3.0 https://github.com/lvgl/lv_drivers.git
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Running the Simulator

#### Interactive Mode (SDL2 Window)

```bash
./p4mslo_sim
```

Opens a 720x720 SDL2 window with keyboard → button mapping. Navigate the UI in real-time at ~200fps.

#### Headless Mode (CI Screenshots)

```bash
./p4mslo_sim --screenshot
```

Dumps PPM framebuffer images at each navigation step for automated UI testing.

### Hardware Stubs

All `app_*` hardware calls (storage, camera, ISP, album) are implemented as stubs in `sim_hal.c` that track state in a `sim_hw_state_t` struct and print actions to stdout for debugging.

**Known Limitation**: Camera viewfinder canvas renders as empty in simulator (no camera feed, expected).

---

## Project Structure

```
.
├── examples/
│   └── factory_demo/      # ESP-IDF factory firmware
├── test/                  # Host test suite (no hardware needed)
│   ├── test_nvs_storage.c
│   ├── test_gpio_bsp.c
│   ├── test_ui_state.c
│   ├── test_ai_buffers.c
│   ├── test_sleep_wakeup.c
│   ├── simulator/         # LVGL UI simulator
│   │   ├── CMakeLists.txt
│   │   ├── sim_hal.c
│   │   ├── sim_main.c
│   │   └── test_ui_simulator.c
│   └── mocks/             # 14 mock headers for hardware simulation
├── Dockerfile.test        # CI Docker image (gcc:14)
├── MARISOL.md             # Pipeline context (build/test/history)
└── README.md              # This file
```

---

## Known Issues

1. **ESP-IDF Version Constraint**: ESP-IDF v5.5.3 required — v5.5.1/5.5.2 fail during build
2. **SDL2 Dependencies**: LVGL simulator needs SDL2 dev headers (`libsdl2-dev`) — not available in all CI environments
3. **Camera Viewfinder**: Renders as empty in simulator (no camera feed, expected behavior)
4. **PlatformIO**: Does NOT support ESP32-P4 — must use ESP-IDF CMake directly

---

## Contributing

### Adding Tests

1. Create new test file in `test/` directory
2. Add mock implementations in `test/mocks/` if needed
3. Update `test/CMakeLists.txt` to include new test
4. Verify all 59 tests pass: `cd test/build && for t in test_*; do [ -x "$t" ] && ."/$t"; done`

### Updating LVGL UI

1. Modify SquareLine Studio project (external tool)
2. Regenerate `ui_*.c` files
3. Update `test/simulator/` with new assets
4. Test in interactive mode: `./p4mslo_sim`
5. Verify screenshots in headless mode: `./p4mslo_sim --screenshot`

### Firmware Development

1. Use ESP-IDF v5.5.3 exclusively
2. Test with host tests before hardware deployment
3. Validate LVGL simulator behavior matches hardware expectations
4. Document any hardware-specific changes in `MARISOL.md`

---

## License

Copyright (c) 2024 Espressif Systems (Shanghai) CO., LTD. All rights reserved.

This project is part of the ESP32-P4X-EYE development board firmware distribution.

---

## References

- [ESP32-P4X-EYE User Guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html)
- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/)
- [LVGL Documentation](https://docs.lvgl.io/)
- [SquareLine Studio](https://squareline.io/)
