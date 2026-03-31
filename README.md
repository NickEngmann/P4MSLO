# ESP32-P4X-EYE Development Board

## User Guide

*Note: This section contains detailed usage instructions for the ESP32-P4X-EYE development board.*

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

# P4MSLO - ESP32-P4X-EYE Factory Demo with AI Detection

## Project Overview

P4MSLO (ESP32-P4X-EYE Factory Demo) is a comprehensive embedded software project for the ESP32-P4X-EYE development board. It features:

- **AI Detection**: Real-time object detection using ESP32-P4's AI capabilities
- **LVGL UI**: Full graphical interface with 4 screens, 7 fonts, and 21 images
- **Hardware Simulation**: Complete mock infrastructure for testing without physical hardware
- **NVS Storage**: Non-volatile storage for settings persistence
- **GPIO/BSP Control**: Full board support package integration
- **Power Management**: Sleep/wakeup functionality with multiple triggers

The project uses ESP-IDF v5.5.3 for ESP32-P4 development and includes a comprehensive test suite with 61 tests covering all major functionality.

## Build & Run

### Language & Framework

- **Language**: C (ESP-IDF framework)
- **Framework**: ESP-IDF v5.5.3 (required - v5.5.1/5.5.2 fail)
- **Docker image**: `gcc:14` for host tests, `espressif/idf:v5.5.3` for ESP-IDF builds
- **Build System**: CMake (ESP-IDF native)

### Prerequisites

#### For ESP-IDF Builds

1. **ESP-IDF v5.5.3** - Install from [Espressif official documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/index.html)
2. **ESP32-P4 target** - Set target with `idf.py set-target esp32p4`
3. **Docker** - For CI builds using `espressif/idf:v5.5.3`

#### For Host Tests (No ESP-IDF Required)

1. **CMake** - For building test suite
2. **GCC** - For compiling C code
3. **Make** - For building tests
4. **nproc** - For parallel compilation

#### For LVGL Simulator

1. **SDL2 development headers** - `libsdl2-dev` on Ubuntu/Debian
2. **LVGL v8.3.11** - Clone from [LVGL GitHub](https://github.com/lvgl/lvgl)
3. **LVGL drivers v8.3.0** - Clone from [lv_drivers GitHub](https://github.com/lvgl/lv_drivers)

### Installation

#### Clone Repository

```bash
git clone https://github.com/espressif/ESP32-P4X-EYE.git
cd ESP32-P4X-EYE
```

#### Install Dependencies (Host Tests)

```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install -y cmake build-essential libsdl2-dev

# For ESP-IDF builds
# Follow ESP-IDF installation guide at:
# https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/index.html
```

#### Install Dependencies (LVGL Simulator)

```bash
cd test/simulator
git clone --depth 1 --branch v8.3.11 https://github.com/lvgl/lvgl.git
git clone --depth 1 --branch v8.3.0 https://github.com/lvgl/lv_drivers.git
cd ..
cd ..
```

### Build Commands

#### ESP-IDF Build (Factory Demo)

```bash
cd factory_demo
idf.py set-target esp32p4
idf.py build
```

This produces firmware binaries for flashing to the ESP32-P4X-EYE board.

#### Host Tests Build

```bash
cd test
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

#### LVGL Simulator Build

```bash
cd test/simulator
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

This produces the `p4eye_sim` executable (per `test/simulator/CMakeLists.txt:72`) for interactive and headless UI testing.

#### Docker Build (Test Suite)

```bash
docker build -f Dockerfile.test -t p4mslo-test .
docker run --rm p4mslo-test
```

### Run Commands

#### Run All Host Tests

```bash
cd test/build
for t in test_*; do [ -x "$t" ] && ./$t; done
```

This runs all 61 tests across 7 test suites:
- NVS Storage tests (10 tests)
- GPIO/BSP tests (12 tests)
- UI State tests (12 tests)
- AI Buffers tests (8 tests)
- Sleep/Wakeup tests (5 tests)
- Photo Quality tests (19 tests)
- UI Simulator tests (12 tests)

#### Run LVGL Simulator (Interactive Mode)

```bash
cd test/simulator/build
./p4eye_sim
```

This launches an interactive SDL2 window (720x720) with keyboard-to-button mapping for testing the full UI workflow.

#### Run LVGL Simulator (Headless Mode)

```bash
cd test/simulator/build
./p4eye_sim --screenshot
```

This runs in headless mode and dumps PPM framebuffer screenshots at each navigation step for automated testing.

**Note**: The executable is named `p4eye_sim` per `test/simulator/CMakeLists.txt:72`, not `p4mslo_sim` as sometimes referenced in older documentation.

#### Flash to Hardware

```bash
cd factory_demo
idf.py -p /dev/ttyUSB0 flash monitor
```

Replace `/dev/ttyUSB0` with your actual serial port.

## Testing

### Test Framework

The project uses a custom C-based test framework with the following structure:

- **Unit Tests**: Direct function testing with mock dependencies
- **Integration Tests**: Full workflow testing with simulated hardware
- **LVGL Simulator Tests**: Real UI code tested against mocked hardware calls

### Test Suites Overview

| Suite | File | Tests | Coverage |
|-------|------|-------|----------|
| NVS Storage | `test/test_nvs_storage.c` | 10 | Settings save/load, photo count, interval state, type checking |
| GPIO/BSP | `test/test_gpio_bsp.c` | 12 | Pin state, flashlight, display, I2C, SD detect, knob init |
| UI State | `test/test_ui_state.c` | 12 | Page navigation, magnification, AI mode, SD/USB transitions |
| AI Buffers | `test/test_ai_buffers.c` | 8 | Buffer init, alignment, circular index, deinit safety |
| Sleep/Wakeup | `test/test_sleep_wakeup.c` | 5 | Wakeup cause, timer+interval, GPIO wakeup |
| Photo Quality | `test/test_photo_quality.c` | 19 | JPEG quality, brightness, contrast, saturation, range validation |
| UI Simulator | `test/simulator/test_ui_simulator.c` | 12 | Full UI workflow, knob debounce, menu wrap, USB interrupt |

**Total**: 61 tests

### Mock Infrastructure

The project includes 17 mock headers in `test/mocks/` for hardware-independent testing:

#### Core Mocks

- **`nvs.h`**: In-memory NVS with 64-entry store, namespace isolation, type checking
- **`nvs_flash.h`**: NVS flash interface stubs
- **`driver/gpio.h`**: 64-pin state tracking with read/write capabilities
- **`bsp/esp32_p4_eye.h`**: Full BSP mock including flashlight, I2C, SD card, knob, display, and buttons

#### System Mocks

- **`esp_timer.h`**: Controllable mock timer for time-based testing
- **`esp_sleep.h`**: Configurable wakeup cause simulation
- **`esp_memory_utils.h`**: Aligned allocation via stdlib for memory testing
- **`esp_err.h`**: ESP error code definitions
- **`esp_log.h`**: Log output wrapper
- **`esp_system.h`**: System function stubs
- **`esp_check.h`**: Check macro implementations
- **`esp_lvgl_port.h`**: LVGL port stubs

#### I/O Mocks

- **`iot_button.h`**: Button interface mock
- **`iot_knob.h`**: Knob interface mock
- **`ui_extra.h`**: UI helper functions

#### Support Mocks

- **`freertos/`**: Task and queue simulation
- **`sdkconfig.h`**: Configuration parameter mocks

### Test Execution

#### Individual Test Files

```bash
cd test/build
./test_nvs_storage
./test_gpio_bsp
./test_ui_state
./test_ai_buffers
./test_sleep_wakeup
./test_photo_quality
```

#### Full Test Suite

```bash
cd test/build
for t in test_*; do [ -x "$t" ] && ./$t; done
```

#### CI Test Execution

Tests run automatically in CI with the following phases:

1. **Docker Test Image** - Builds Dockerfile.test, runs tests in container (~30s)
2. **ESP-IDF Build** - Cross-compiles with espressif/idf:v5.5.3, uploads firmware artifacts (~4.5min)
3. **LVGL Simulator** - Builds and runs simulator tests with screenshot capture (~20s)

### Hardware Mocks Required

- **Host Tests**: No hardware required - all mocks are software-based
- **LVGL Simulator**: No hardware required - sim_hal.c provides hardware stubs
- **ESP-IDF Build**: Requires ESP32-P4X-EYE board for flashing
- **CI Pipeline**: Uses Docker containers for reproducible builds

## Known Issues

### ESP-IDF Version Compatibility

- **ESP-IDF v5.5.3 required** - Projects using v5.5.1 or v5.5.2 will fail to build
- **PlatformIO limitation** - PlatformIO does NOT support ESP32-P4 - must use ESP-IDF CMake directly
- **idf_component.yml constraint** - Project enforces ESP-IDF v5.5.3 via dependency configuration

### LVGL Simulator Requirements

- **SDL2 dev headers** - Required for simulator build, not available in all CI environments
- **Camera viewfinder** - Renders as empty in simulator (expected behavior - no camera feed available)
- **Memory constraints** - Simulator requires sufficient RAM for LVGL framebuffer (~512KB)

### Build System Limitations

- **No PlatformIO support** - ESP32-P4 requires native ESP-IDF CMake build system
- **Cross-compilation** - Requires ESP-IDF toolchain installation
- **Docker dependencies** - ESP-IDF Docker image is large (~5GB) and slow to pull

### Test Infrastructure

- **Mock coverage** - 17 mock headers cover all hardware interfaces but may not capture all edge cases
- **Timing tests** - Sleep/wakeup tests require precise timing control from mock timers
- **UI state transitions** - Complex state machines may have untested edge cases

## Architecture

### Project Structure

```
ESP32-P4X-EYE/
├── factory_demo/           # Main ESP-IDF application
│   ├── main/              # Application source code
│   ├── components/        # AI detection components
│   ├── sim_hal.c         # Hardware simulation layer
│   ├── CMakeLists.txt    # Build configuration
│   └── idf_component.yml # ESP-IDF component dependencies
├── test/                  # Test suite
│   ├── test_nvs_storage.c
│   ├── test_gpio_bsp.c
│   ├── test_ui_state.c
│   ├── test_ai_buffers.c
│   ├── test_sleep_wakeup.c
│   ├── test_photo_quality.c
│   ├── simulator/         # LVGL simulator tests
│   │   ├── test_ui_simulator.c
│   │   ├── sim_hal.c
│   │   └── CMakeLists.txt
│   └── mocks/            # Hardware mock implementations (17 headers)
│       ├── nvs.h
│       ├── nvs_flash.h
│       ├── driver/gpio.h
│       ├── bsp/esp32_p4_eye.h
│       ├── esp_timer.h
│       ├── esp_sleep.h
│       ├── esp_memory_utils.h
│       ├── esp_err.h
│       ├── esp_log.h
│       ├── esp_system.h
│       ├── esp_check.h
│       ├── esp_lvgl_port.h
│       ├── iot_button.h
│       ├── iot_knob.h
│       ├── ui_extra.h
│       └── sdkconfig.h
├── common_components/     # Shared ESP32-P4X-EYE components
│   └── esp32_p4_eye/
├── .github/workflows/     # CI/CD pipeline definitions
├── Dockerfile.test        # Test container definition
└── platformio.ini         # PlatformIO configuration (for reference)
```

### Key Components

#### LVGL UI System

- **Screens**: 4 main screens (Home, Camera, Settings, AI Status)
- **Fonts**: 7 custom fonts for different UI elements
- **Images**: 21 UI assets including icons and backgrounds
- **Navigation**: Knob-based menu system with debounce handling
- **State Machine**: Complex UI state transitions with USB/SD event handling

#### AI Detection Pipeline

- **Buffer Management**: Circular buffer with alignment guarantees
- **Image Processing**: ISP (Image Signal Processor) integration
- **Detection Algorithms**: Real-time object detection with confidence scoring
- **Memory Safety**: Deinit safety checks and buffer overflow protection

#### Power Management

- **Sleep Modes**: Deep sleep, light sleep, and standby modes
- **Wakeup Sources**: GPIO interrupts, timer triggers, and interval-based wakeup
- **State Preservation**: NVS storage for power state recovery
- **Power Monitoring**: Current consumption tracking and optimization

#### Hardware Abstraction Layer (HAL)

- **GPIO Control**: Pin configuration and state management
- **I2C Communication**: Sensor and display interface
- **SD Card Interface**: Storage access and file management
- **Display Driver**: LVGL integration with hardware display
- **Button Input**: Physical button event handling
- **Knob Interface**: Rotary encoder for menu navigation

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

## Notes

### Development Guidelines

1. **Always test with mocks first** - Run host tests before hardware deployment
2. **Verify LVGL simulator** - Test UI changes in simulator before flashing
3. **Check ESP-IDF version** - Ensure v5.5.3 is installed before building
4. **Monitor memory usage** - LVGL and AI features are memory-intensive
5. **Test power states** - Verify sleep/wakeup behavior in all scenarios

### CI/CD Pipeline

The project uses a multi-phase CI pipeline:

1. **Phase 1: Host Tests** - No ESP-IDF required, runs in Docker container (~30s)
2. **Phase 2: LVGL Simulator** - Builds and tests UI with screenshot capture (~20s)
3. **Phase 3: ESP-IDF Build** - Cross-compiles firmware for ESP32-P4 (~4.5min)
4. **Phase 4: Upload Artifacts** - Stores firmware binaries for deployment

### Hardware Requirements

- **ESP32-P4X-EYE Board**: Required for production deployment
- **USB-C Cable**: For flashing and serial monitoring
- **SD Card**: Optional for storage expansion
- **Camera Module**: Required for AI detection features
- **Power Supply**: 5V/2A USB power adapter

### Debugging Tips

1. **Enable verbose logging** - Use `idf.py set-log-level DEBUG`
2. **Check NVS dumps** - Use `nvs_dump` command for storage issues
3. **Monitor GPIO states** - Use `gpio_monitor` for pin debugging
4. **Test with simulator** - Use LVGL simulator for UI debugging
5. **Review mock coverage** - Check which hardware interfaces are mocked

### Security Considerations

1. **NVS encryption** - Sensitive settings should be encrypted
2. **GPIO protection** - Prevent unauthorized pin access
3. **SD card validation** - Verify file integrity before mounting
4. **AI model signing** - Ensure firmware authenticity
5. **Secure boot** - Enable ESP32-P4 secure boot features

### Performance Optimization

1. **Buffer alignment** - Use aligned allocation for AI processing
2. **Memory pooling** - Pre-allocate common data structures
3. **DMA usage** - Leverage DMA for camera and display transfers
4. **Power gating** - Disable unused peripherals during sleep
5. **Cache optimization** - Configure instruction/data cache settings

### Future Enhancements

1. **Multi-camera support** - Add support for additional camera modules
2. **Cloud integration** - Enable remote firmware updates
3. **Advanced AI models** - Support for custom neural network models
4. **Wireless connectivity** - Add Wi-Fi/Bluetooth for remote control
5. **Extended sensors** - Support for additional I2C/SPI sensors

### Contributing

1. **Fork the repository** - Create your own fork for development
2. **Branch from main** - Use feature branches for new work
3. **Run all tests** - Ensure 59 tests pass before submitting
4. **Update documentation** - Keep README and MARISOL.md current
5. **Follow coding style** - Match existing C code conventions

### License

This project is part of the ESP32-P4X-EYE development board firmware. See the LICENSE file in the repository for full licensing terms.

### Support

For technical support:
- [ESP32-P4X-EYE User Guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html)
- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/index.html)
- [LVGL Documentation](https://docs.lvgl.io/)
- [GitHub Issues](https://github.com/espressif/ESP32-P4X-EYE/issues)
