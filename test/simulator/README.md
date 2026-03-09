# ESP32-P4X-EYE LVGL Simulator

Runs the **real** SquareLine Studio UI code (LVGL 8.3.11) with an SDL2 display
backend on desktop Linux. No fake renderers -- all widgets, fonts, images, and the
`ui_extra.c` state machine compile and run identically to the target hardware.

## Prerequisites

```bash
sudo apt install libsdl2-dev cmake build-essential
```

## Build

```bash
cd test/simulator
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

## Run (Interactive)

```bash
./p4eye_sim
```

A 720x720 window (240x240 at 3x zoom) opens showing the camera main screen.

### Keyboard Controls

| Key          | Action                              |
|--------------|-------------------------------------|
| Up / Down    | Navigate / scroll menu              |
| Left / Right | Knob left / right                   |
| Enter        | Encoder press (select)              |
| Escape / M   | Menu button (back)                  |
| Q            | Quit                                |

## Run (Headless / CI)

```bash
SDL_VIDEODRIVER=dummy ./p4eye_sim --screenshot
```

Outputs numbered PPM screenshots to `screenshots/` after an automated button
sequence that exercises the main pages (camera, album, settings).

No SDL2 window is created -- suitable for headless CI environments.

## Architecture

```
test/simulator/
├── lvgl/               # LVGL 8.3.11 source (git clone --depth 1 --branch v8.3.11)
├── lv_drivers/         # SDL2 display driver (git clone --depth 1 --branch v8.3.0)
├── lv_conf.h           # LVGL config: 240x240, RGB565, 16-swap, stdlib malloc
├── lv_drv_conf.h       # SDL2 driver config: 3x zoom
├── sim_main.c          # SDL2 main loop, keyboard mapping, screenshot mode
├── sim_hal.c/h         # Mock HAL: app_storage, app_album, app_video_stream, app_isp
├── sim_config.h        # (existing) State-machine test parameters
├── ui_simulator.h      # (existing) State-machine regression tests
├── test_ui_simulator.c # (existing) Unit tests
├── CMakeLists.txt      # Build system
└── README.md           # This file
```

The simulator compiles:
- **LVGL 8.3.11 core** (all `lvgl/src/**/*.c`)
- **lv_drivers SDL2 backend** (`lv_drivers/sdl/sdl.c`, `sdl_common.c`)
- **Real UI code**: `ui.c`, `ui_extra.c`, `ui_helpers.c`, all screens, fonts, images
- **Mock HAL**: `sim_hal.c` stubs all ESP32 hardware calls with state tracking
- **ESP-IDF mocks**: Reuses existing `test/mocks/` (FreeRTOS, esp_log, esp_err, etc.)

## How It Works

1. `sim_main.c` initializes LVGL, sets up the SDL2 display driver (240x240, 3x zoom)
2. Calls `ui_init()` (SquareLine generated) which creates all screens/widgets
3. Calls `ui_extra_init()` which loads settings and sets up the state machine
4. SDL event loop maps keyboard presses to `ui_extra_btn_*()` handlers
5. `lv_timer_handler()` drives LVGL rendering at ~200fps

All `app_*` hardware calls (storage, camera, ISP, album) are implemented as
stubs in `sim_hal.c` that track state in a `sim_hw_state_t` struct and print
actions to stdout for debugging.
