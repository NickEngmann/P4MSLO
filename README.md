# P4MSLO — ESP32-P4X-EYE Factory Demo

Factory firmware for the [ESP32-P4X-EYE](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html) development board featuring real-time AI object detection, a full LVGL graphical interface, and a comprehensive hardware-independent test suite.

| | |
|---|---|
| **Board** | ESP32-P4X-EYE ([User Guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html)) |
| **Framework** | ESP-IDF v5.5.3 (C) |
| **UI** | LVGL 8.3.11 / SquareLine Studio — 4 screens, 7 fonts, 21 images |
| **AI** | COCO object, human face, and pedestrian detection via esp-dl |
| **Tests** | 60 tests across 6 suites, 17 mock headers, LVGL simulator with screenshots |
| **CI** | GitHub Actions — host tests, Docker tests, ESP-IDF cross-compilation |

## Quick Start

### Build the Factory Demo (ESP-IDF)

```bash
# Requires ESP-IDF v5.5.3 — install from https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/
cd factory_demo
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # flash + serial monitor
```

### Run Host Tests (no hardware needed)

```bash
sudo apt-get install -y cmake build-essential   # Ubuntu/Debian
cd test && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
for t in test_*; do [ -x "$t" ] && ./"$t"; done
```

### Run Tests in Docker

```bash
docker build -f Dockerfile.test -t p4mslo-test .
docker run --rm p4mslo-test
```

### Run the LVGL Simulator

```bash
sudo apt-get install -y libsdl2-dev
cd test/simulator
git clone --depth 1 --branch v8.3.11 https://github.com/lvgl/lvgl.git
git clone --depth 1 --branch v8.3.0 https://github.com/lvgl/lv_drivers.git
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc)
./p4eye_sim              # interactive SDL2 window (720x720)
./p4eye_sim --screenshot # headless — captures PPM screenshots at each step
```

## Project Structure

```
P4MSLO/
├── factory_demo/              # ESP-IDF application
│   ├── main/
│   │   ├── app/              # Camera, storage, ISP, video logic
│   │   ├── ui/               # SquareLine Studio UI (screens/, fonts/, images/)
│   │   └── main.c
│   ├── components/            # AI models (coco_detect, human_face, pedestrian)
│   ├── sdkconfig.defaults
│   └── CMakeLists.txt
├── common_components/
│   └── esp32_p4_eye/          # Board support package
├── test/
│   ├── test_nvs_storage.c     # 10 tests — NVS save/load, type checking
│   ├── test_gpio_bsp.c        # 12 tests — GPIO, flashlight, I2C, knob
│   ├── test_ui_state.c        # 12 tests — page nav, magnification, AI mode
│   ├── test_ai_buffers.c      #  8 tests — buffer init, alignment, circular index
│   ├── test_sleep_wakeup.c    #  5 tests — wakeup cause, timer, GPIO wakeup
│   ├── simulator/
│   │   ├── test_ui_simulator.c # 12 tests — full UI workflow, knob, USB interrupt
│   │   ├── sim_main.c         # SDL2 main loop
│   │   ├── sim_hal.c          # Hardware stub layer
│   │   └── screenshots/       # 11 captured UI navigation screenshots
│   ├── mocks/                 # 17 ESP-IDF mock headers
│   ├── unity/                 # Unity test framework header
│   └── CMakeLists.txt
├── .github/workflows/ci.yml
├── Dockerfile                 # Full CI image (tests + ESP-IDF cross-compile)
├── Dockerfile.test            # Lightweight test-only image
└── .dockerignore
```

## Testing

### Test Suites (59 total)

| Suite | File | # | What it covers |
|-------|------|---|----------------|
| NVS Storage | `test/test_nvs_storage.c` | 10 | Settings save/load, photo count, interval state, type checking |
| GPIO/BSP | `test/test_gpio_bsp.c` | 12 | Pin state, flashlight, display, I2C, SD detect, knob init |
| UI State | `test/test_ui_state.c` | 12 | Page navigation, magnification, AI mode, SD/USB transitions |
| AI Buffers | `test/test_ai_buffers.c` | 8 | Buffer init, alignment, circular index, deinit safety |
| Sleep/Wakeup | `test/test_sleep_wakeup.c` | 5 | Wakeup cause, timer+interval, GPIO wakeup |
| UI Simulator | `test/simulator/test_ui_simulator.c` | 13 | Full UI workflow, knob debounce, menu wrap, USB interrupt |

### Mock Infrastructure

17 mock headers in `test/mocks/` enable hardware-independent testing:

- **Hardware**: `nvs.h` (64-entry in-memory store), `nvs_flash.h`, `driver/gpio.h` (64-pin tracking), `driver/i2c_master.h`, `iot_button.h`, `iot_knob.h`
- **BSP**: `bsp/esp32_p4_eye.h` (flashlight, I2C, SD, knob, display, buttons), `bsp/esp-bsp.h`, `bsp/display.h`, `bsp/bsp_err_check.h`
- **System**: `esp_timer.h` (controllable), `esp_sleep.h` (configurable wakeup), `esp_system.h`, `esp_memory_utils.h`, `esp_log.h`, `esp_err.h`, `esp_check.h`
- **UI/Config**: `esp_lvgl_port.h`, `ui_extra.h`, `sdkconfig.h`, `freertos/FreeRTOS.h`, `freertos/task.h`

### LVGL Simulator

The simulator compiles **real** SquareLine Studio UI code against LVGL 8.3.11 with an SDL2 display backend. All hardware calls (camera, storage, ISP) are stubbed in `sim_hal.c`.

- **Interactive mode**: 720x720 SDL2 window with keyboard-to-button mapping
- **Headless mode**: `--screenshot` captures framebuffer PNGs at each navigation step
- 11 screenshots stored in `test/simulator/screenshots/`

## CI Pipeline

Three jobs run in parallel via GitHub Actions (`.github/workflows/ci.yml`):

| Job | What | Time |
|-----|------|------|
| **host-tests** | Build + run 59 unit tests on `ubuntu-latest` | ~30s |
| **docker-test** | Build `Dockerfile.test`, run tests in container | ~1min |
| **idf-build** | Cross-compile in `espressif/idf:v5.5.3`, upload firmware artifacts | ~4.5min |

## Known Issues

- **ESP-IDF v5.5.3 required** — v5.5.1/5.5.2 fail to resolve components
- **PlatformIO not supported** — ESP32-P4 requires native ESP-IDF CMake
- **Camera viewfinder** renders empty in simulator (expected — no camera feed)
- **SDL2 headers** required for simulator (`libsdl2-dev`), not available in all CI envs
- **LVGL/lv_drivers** must be cloned manually into `test/simulator/` (gitignored)
- **Dockerfile** ESP-IDF stage still references `v5.5.1` — CI uses `v5.5.3`

## Hardware

- **ESP32-P4X-EYE Board** + USB-C cable for flashing/monitoring
- **Camera module** required for AI detection
- **SD card** optional for storage
- **5V/2A USB** power supply

## License

See [LICENSE](./LICENSE) for terms.

## Links

- [ESP32-P4X-EYE User Guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html)
- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/index.html)
- [LVGL Documentation](https://docs.lvgl.io/)
