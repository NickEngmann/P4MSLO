# P4MSLO Complete Circuit Diagram

## System Overview

```
                                    ┌─────────────────────┐
                                    │    USB-C (Host PC)   │
                                    └────────┬────────────┘
                                             │ USB Serial JTAG
                                             │ (console + MSC)
                                             │
   ┌─────────────────────────────────────────┴──────────────────────────────────┐
   │                          ESP32-P4X-EYE                                     │
   │                                                                            │
   │  ┌──────────┐  ┌──────────┐  ┌────────┐  ┌───────┐  ┌────────┐           │
   │  │ ST7789   │  │ OV2710   │  │QMA7981 │  │ PDM   │  │ SD Card│           │
   │  │ Display  │  │ Camera   │  │  IMU   │  │  Mic  │  │ (SDMMC)│           │
   │  │ 240x240  │  │ MIPI CSI │  │        │  │       │  │ 4-bit  │           │
   │  └────┬─────┘  └────┬─────┘  └───┬────┘  └──┬────┘  └───┬────┘           │
   │       │SPI2         │MIPI        │I2C       │I2S        │SDMMC           │
   │       │             │            │          │           │                 │
   │  ┌────┴─────────────┴────────────┴──────────┴───────────┴──────────┐      │
   │  │                     ESP32-P4 SoC                                │      │
   │  │                  (dual RISC-V, 360MHz)                          │      │
   │  │                  32MB PSRAM, 350KB SRAM                         │      │
   │  │                                                                 │      │
   │  │  SPI3 Master ──→ 4x ESP32-S3 cameras (external)                │      │
   │  │  GPIO 34     ──→ Trigger (shared to all S3)                     │      │
   │  └────┬──────────────────────────────────────┬─────────────────────┘      │
   │       │SPI3                                  │GPIO                        │
   │       │                              ┌───────┴──────────┐                 │
   │       │                              │ Buttons & Knob   │                 │
   │       │                              │ Flashlight LED   │                 │
   └───────┼──────────────────────────────┴──────────────────┘                 │
           │                                                                    │
           └──────────── SPI Bus + Trigger ────────────────────────────────────┘
                              │
         ┌────────────────────┼────────────────────┐
         │                    │                    │
    ┌────┴─────┐    ┌────────┴────────┐    ┌─────┴────┐    ┌──────────┐
    │ S3 Cam 1 │    │   S3 Cam 2     │    │ S3 Cam 3 │    │ S3 Cam 4 │
    │ OV5640   │    │   OV5640       │    │ OV5640   │    │ OV5640   │
    │ 2560x1920│    │   2560x1920    │    │ 2560x1920│    │ 2560x1920│
    └──────────┘    └────────────────┘    └──────────┘    └──────────┘
```

---

## ESP32-P4X-EYE On-Board Peripherals

```
                         ESP32-P4 SoC
    ┌────────────────────────────────────────────────────┐
    │                                                    │
    │                  ┌──── SPI2 (Display) ────┐        │
    │   GPIO 17 ──CLK──┘                        │        │
    │   GPIO 16 ──MOSI                          │        │
    │   GPIO 18 ──CS────────────────────────┐   │        │
    │   GPIO 19 ──DC────────────────────┐   │   │        │
    │   GPIO 15 ──RST───────────────┐   │   │   │        │
    │   GPIO 20 ──BACKLIGHT─────┐   │   │   │   │        │
    │                           │   │   │   │   │        │
    │               ┌───────────┴───┴───┴───┴───┘        │
    │               │     ST7789 LCD (240x240)           │
    │               │     SPI2_HOST @ 80MHz              │
    │               └────────────────────────────        │
    │                                                    │
    │                  ┌──── MIPI CSI (Camera) ──┐       │
    │   GPIO 11 ──XCLK─┘  (24MHz master clock)  │       │
    │   GPIO 12 ──EN────── (power enable)        │       │
    │   GPIO 26 ──RST───── (sensor reset)        │       │
    │   GPIO 34 ──SCL────┐ (SCCB/I2C, port 0)   │       │
    │   GPIO 31 ──SDA────┤                       │       │
    │               ┌────┴───────────────────────┘       │
    │               │     OV2710 Camera Sensor           │
    │               │     1920x1080, MIPI 2-lane         │
    │               └────────────────────────────        │
    │                                                    │
    │                  ┌──── I2C (Sensors) ──────┐       │
    │   GPIO 13 ──SCL──┘  (port 1, 400kHz)       │       │
    │   GPIO 14 ──SDA                            │       │
    │               ┌────────────────────────────┘       │
    │               │     QMA7981 IMU                    │
    │               │     (display auto-rotation)        │
    │               └────────────────────────────        │
    │                                                    │
    │                  ┌──── I2S (Audio) ────────┐       │
    │   GPIO 22 ──CLK──┘                         │       │
    │   GPIO 21 ──DATA                           │       │
    │               ┌────────────────────────────┘       │
    │               │     PDM Microphone                 │
    │               │     48kHz, 24-bit mono             │
    │               └────────────────────────────        │
    │                                                    │
    │                  ┌──── SDMMC (SD Card) ────┐       │
    │   GPIO 46 ──EN───┘  (power control)        │       │
    │   GPIO 45 ──DET──── (card detect)          │       │
    │   (SDMMC hw) ────── (4-bit data bus)       │       │
    │               ┌────────────────────────────┘       │
    │               │     microSD Card Slot              │
    │               │     4-bit SDMMC, high-speed        │
    │               └────────────────────────────        │
    │                                                    │
    │                  ┌──── User Controls ──────┐       │
    │   GPIO  2 ──BTN──┘  (encoder press)        │       │
    │   GPIO  3 ──BTN──── (button 1)             │       │
    │   GPIO  4 ──BTN──── (button 2)             │       │
    │   GPIO  5 ──BTN──── (button 3)             │       │
    │   GPIO 48 ──ENC_A── (rotary encoder)       │       │
    │   GPIO 47 ──ENC_B── (rotary encoder)       │       │
    │   GPIO 23 ──LED──── (flashlight)           │       │
    │               └────────────────────────────┘       │
    │                  All buttons active LOW             │
    │                                                    │
    └────────────────────────────────────────────────────┘
```

---

## SPI Camera Bus (P4 to 4x S3)

```
    ESP32-P4                              4x XIAO ESP32-S3 Sense
   ┌──────────┐                          ┌──────────────────────┐
   │          │                          │    S3 Camera #1      │
   │          │         CLK              │                      │
   │  GPIO 37 ├─────────────────┬────────┤ GPIO 7 (CLK)        │
   │          │                 │  100pF │                      │
   │          │                 ├──┤├──GND│                      │
   │          │                 │        │                      │
   │          │         MOSI    │        │                      │
   │  GPIO 38 ├─────────────────┼────────┤ GPIO 9 (MOSI)       │
   │          │                 │        │                      │
   │          │         MISO    │ 330Ω   │                      │
   │  GPIO 50 ├──┬──────────────┼─┤/\/├──┤ GPIO 8 (MISO)       │
   │          │  │              │        │                      │
   │          │ ┌┴┐             │        │                      │
   │          │ │ │10KΩ         │        │                      │
   │          │ └┬┘             │        │                      │
   │          │  │              │        │                      │
   │          │ GND  (pulldown at master: anchors MISO          │
   │          │       when all slaves are tri-stated)           │
   │          │                 │        │                      │
   │          │         CS0     │  10KΩ  │                      │
   │  GPIO 51 ├─────────────────┼──┤/\/├─┤─GPIO 2 (CS)         │
   │          │                 │  ↑3.3V │                      │
   │          │         TRIG    │        │                      │
   │  GPIO 34 ├─────────────────┼────────┤ GPIO 1 (TRIGGER)    │
   │          │                 │        │                      │
   │          │                 │        └──────────────────────┘
   │          │                 │
   │          │                 │        ┌──────────────────────┐
   │          │                 │        │    S3 Camera #2      │
   │          │                 │        │                      │
   │          │                 ├────────┤ GPIO 7 (CLK)         │
   │          │                 │  100pF │                      │
   │          │                 ├──┤├──GND│                     │
   │          │                 │        │                      │
   │          │                 ├────────┤ GPIO 9 (MOSI)        │
   │          │                 │        │                      │
   │          │                 │ 330Ω   │                      │
   │          │                 ├─┤/\/├──┤ GPIO 8 (MISO)        │
   │          │         CS1     │        │                      │
   │  GPIO 52 ├─────────────────┼──┤/\/├─┤─GPIO 2 (CS)          │
   │          │                 │  10KΩ  │  ↑3.3V               │
   │          │                 │        │                      │
   │          │                 ├────────┤ GPIO 1 (TRIGGER)     │
   │          │                 │        └──────────────────────┘
   │          │                 │
   │          │                 │        ┌──────────────────────┐
   │          │                 │        │    S3 Camera #3      │
   │          │                 │        │                      │
   │          │                 ├────────┤ GPIO 7 (CLK)         │
   │          │                 │  100pF │                      │
   │          │                 ├──┤├─GND│                      │
   │          │                 │        │                      │
   │          │                 ├────────┤ GPIO 9 (MOSI)        │
   │          │                 │        │                      │
   │          │                 │ 330Ω   │                      │
   │          │                 ├─┤/\/├──┤ GPIO 8 (MISO)        │
   │          │         CS2     │        │                      │
   │  GPIO 53 ├─────────────────┼──┤/\/├─┤─GPIO 2 (CS)          │
   │          │                 │  10KΩ  │  ↑3.3V               │
   │          │                 │        │                      │
   │          │                 ├────────┤ GPIO 1 (TRIGGER)     │
   │          │                 │        └──────────────────────┘
   │          │                 │
   │          │                 │        ┌──────────────────────┐
   │          │                 │        │    S3 Camera #4      │
   │          │                 │        │                      │
   │          │                 ├────────┤ GPIO 7 (CLK)         │
   │          │                 │  100pF │                      │
   │          │                 ├──┤├─GND│                      │
   │          │                 │        │                      │
   │          │                 ├────────┤ GPIO 9 (MOSI)        │
   │          │                 │        │                      │
   │          │                 │ 330Ω   │                      │
   │          │                 └─┤/\/├──┤ GPIO 8 (MISO)        │
   │          │         CS3              │                      │
   │  GPIO 54 ├──────────────────┤/\/├───┤─GPIO 2 (CS)          │
   │          │                  10KΩ    │  ↑3.3V               │
   │          │                          │                      │
   │          │                  ┌───────┤ GPIO 1 (TRIGGER)     │
   │          │                  │       └──────────────────────┘
   └──────────┘                  │
                                 │
   Bus Notes:                    │
   - CLK, MOSI, MISO, TRIGGER   │
     are shared (all on one bus) │
   - Each camera has its own CS  │
   - SPI3_HOST @ 16MHz max      │
   - Camera #4 uses dynamic     │
     CS slot swap (HW limit: 3) │
                                 │
   GND ──────────────────────────┘
   (common ground for all devices)
```

---

## Per-Camera Detail (330Ω + 100pF + 10KΩ)

```
   P4 MISO (GPIO 50) ─────────── shared bus ─────────────┐
                                                          │
                                               330Ω      │
   S3 Camera N, GPIO 8 (MISO out) ────┤/\/\/├────────────┘
                                       ↑
                                   Series resistor prevents
                                   bus contention when this
                                   camera is NOT selected



   P4 CLK (GPIO 37) ──────────── shared bus ──────┐
                                                   │
   S3 Camera N, GPIO 7 (CLK in) ──────────────────┤
                                                   │
                                                 ┌─┴─┐
                                                 │100│pF  (C0G/NP0)
                                                 │   │
                                                 └─┬─┘
                                                   │
                                                  GND
                                                   ↑
                                            Filters ringing
                                            on clock edges



   P4 CS_N (GPIO 51-54) ─────────────────────┬──── S3 Camera N, GPIO 2 (CS in)
                                              │
                                            ┌─┴─┐
                                            │10K│Ω
                                            │   │
                                            └─┬─┘
                                              │
                                             3.3V
                                              ↑
                                       Keeps CS HIGH (deselected)
                                       during P4 boot before
                                       firmware drives it
```

---

## Master-Side CLK / MOSI Source Termination (fitted)

```
   P4 CLK (GPIO 37) ──┤/\/\/├──── shared CLK bus to all 4 S3 slaves ──→
                        33Ω
                        (series resistor AT THE P4, close to the pin)

   P4 MOSI (GPIO 38) ──┤/\/\/├──── shared MOSI bus to all 4 S3 slaves ──→
                         33Ω
```

**Why:** CLK and MOSI each have one driver (P4) and four receivers (S3s) in parallel. The combined input capacitance (≈40–60pF total) forms a low-impedance load at the driver's output, which causes overshoot and ringing on every edge. A small series resistor at the source damps that ringing before it reflects back into the shared bus. This is standard practice for any point-to-multipoint CMOS bus.

**Why 33Ω specifically:** at 10 MHz SPI the bit period is 100 ns. With ~40–50Ω effective source impedance (P4 output + 33Ω) and 60pF load, rise time ≈ 3 ns — comfortably under 5% of the bit period. 22Ω is also a valid pick; 100Ω would push rise time to ~13 ns (13% of the period) and is borderline.

**What this fixed (empirically observed):**

1. **Capture reliability jumped from ~40% first-try success to ~95%.** Before source termination: cameras 1 and 4 (on the "ends" of the physical bus) would fail 60–100% of transfers. After: 5/5 runs saving 4/4 cameras with 19/20 first-try success.

2. **SPI control-command byte corruption was fixed.** Previously, short single-shot transactions from master to slave would sometimes have their first byte mangled (e.g., 0x04 arriving as 0x02 — a one-bit-position shift consistent with the slave sampling on the wrong clock edge because of slow CLK rise). With clean edges, `cam_wifi_on all` now correctly wakes all 4 cameras, `cam_wifi_off all` shuts them down, etc.

Turns out both problems were signal integrity on the CLK/MOSI rails, cured by the same two resistors. USB-powering the cameras is also fine now (previously it caused additional noise that degraded 3 of 4 cameras).

---

## Master-Side MISO Pull-Down

```
   P4 MISO (GPIO 50) ──┬──── shared MISO bus to all 4 S3 slaves ──→
                       │
                     ┌─┴─┐
                     │10K│Ω
                     │   │
                     └─┬─┘
                       │
                      GND
                       ↑
               Anchors MISO at a defined LOW state when
               all slaves are tri-stated (between slave
               selects, or when the slave's CS-edge ISR
               is briefly re-enabling its MISO driver).

               Prevents mid-stream bit ambiguity when the
               line would otherwise float near V_threshold
               during tri-state transitions — which was
               the dominant failure mode for camera 3's
               CORRUPT transfers with 4 slaves on the bus.

               10KΩ is weak enough that a slave driving
               HIGH through its 330Ω series resistor still
               reads ~3.19V (solid logic 1), while strong
               enough to pull the line LOW when genuinely
               floating (I = 330µA sink, τ ≈ 200ns at 20pF
               bus capacitance).
```

---

## XIAO ESP32-S3 Sense Internal Wiring

```
   ┌──────────────────────────────────────────┐
   │         XIAO ESP32-S3 Sense              │
   │                                          │
   │  ┌────────────┐     ┌────────────────┐   │
   │  │  OV5640     │     │  SD Card Slot  │   │
   │  │  Camera     │     │  (disabled in  │   │
   │  │  2560x1920  │     │   SPI mode)    │   │
   │  │  5MP AF     │     │                │   │
   │  └──────┬──────┘     └───────┬────────┘   │
   │         │ DVP/CSI            │ SPI        │
   │         │ (internal)         │ (shared)   │
   │  ┌──────┴────────────────────┴─────┐      │
   │  │          ESP32-S3 SoC           │      │
   │  │        (dual LX7, 240MHz)      │      │
   │  │        8MB PSRAM, 512KB SRAM   │      │
   │  │                                │      │
   │  │  GPIO 7 ── CLK  (SPI slave)    │      │
   │  │  GPIO 9 ── MOSI (SPI slave)    │      │
   │  │  GPIO 8 ── MISO (SPI slave)    │      │
   │  │  GPIO 2 ── CS   (SPI slave)    │      │
   │  │  GPIO 1 ── D0   (trigger in)   │      │
   │  │                                │      │
   │  │  WiFi: "The Garden" network    │      │
   │  │  HTTP API: port 80             │      │
   │  │  OTA: POST /api/v1/ota/upload  │      │
   │  └────────────────────────────────┘      │
   │                                          │
   │  NeoPixel LED (status indicator)         │
   │  USB-C (programming + serial debug)      │
   └──────────────────────────────────────────┘

   Note: GPIO 7/8/9 are shared between SD card SPI and
   the SPI slave interface. When ENABLE_SPI_SLAVE=1 in
   Config.h, SD card is disabled and these pins are used
   for SPI slave communication with the P4 master.
```

---

## Power Distribution

```
   USB-C (5V) ──→ ESP32-P4X-EYE board regulator ──→ 3.3V
                         │
                         ├──→ P4 SoC + PSRAM
                         ├──→ Display backlight
                         ├──→ Camera sensor (OV2710)
                         ├──→ SD card slot
                         └──→ Flashlight LED

   Each XIAO ESP32-S3 has its own USB-C power:
   USB-C (5V) ──→ XIAO onboard regulator ──→ 3.3V
                         │
                         ├──→ S3 SoC + PSRAM
                         ├──→ OV5640 camera
                         └──→ NeoPixel LED

   IMPORTANT: All devices MUST share a common GND.
   The SPI bus will not work without common ground
   between the P4 and all S3 boards.
```

---

## Complete Pin Reference

### ESP32-P4 GPIO Assignments

| GPIO | Function | Direction | Interface | Notes |
|------|----------|-----------|-----------|-------|
| 2 | Encoder button | Input | GPIO | Active LOW |
| 3 | Button 1 | Input | GPIO | Active LOW |
| 4 | Button 2 | Input | GPIO | Active LOW |
| 5 | Button 3 | Input | GPIO | Active LOW |
| 11 | Camera XCLK | Output | Clock | 24MHz to OV2710 |
| 12 | Camera enable | Output | GPIO | Active HIGH |
| 13 | I2C SCL | Bidir | I2C1 | 400kHz, IMU |
| 14 | I2C SDA | Bidir | I2C1 | 400kHz, IMU |
| 15 | Display RST | Output | GPIO | Active LOW |
| 16 | Display MOSI | Output | SPI2 | 80MHz |
| 17 | Display CLK | Output | SPI2 | 80MHz |
| 18 | Display CS | Output | SPI2 | Active LOW |
| 19 | Display DC | Output | GPIO | Data/Command |
| 20 | Display backlight | Output | PWM | LEDC channel |
| 21 | Mic data | Input | I2S | PDM 48kHz |
| 22 | Mic clock | Output | I2S | |
| 23 | Flashlight | Output | GPIO | LED control |
| 26 | Camera RST | Output | GPIO | Active HIGH |
| 31 | Camera I2C SDA | Bidir | I2C0 | SCCB 100kHz |
| 34 | SPI trigger / Cam I2C SCL | Output | GPIO / I2C0 | Shared! |
| 37 | SPI CLK | Output | SPI3 | 16MHz to S3s |
| 38 | SPI MOSI | Output | SPI3 | P4 → S3 |
| 45 | SD card detect | Input | GPIO | Card presence |
| 46 | SD card enable | Output | GPIO | Power control |
| 47 | Knob B | Input | GPIO | Rotary encoder |
| 48 | Knob A | Input | GPIO | Rotary encoder |
| 50 | SPI MISO | Input | SPI3 | S3 → P4 |
| 51 | SPI CS0 | Output | SPI3 | Camera #1 |
| 52 | SPI CS1 | Output | SPI3 | Camera #2 |
| 53 | SPI CS2 | Output | SPI3 | Camera #3 |
| 54 | SPI CS3 | Output | GPIO | Camera #4 (slot swap) |

### ESP32-S3 (XIAO Sense) GPIO Assignments

| GPIO | Function | Direction | Notes |
|------|----------|-----------|-------|
| 1 | Trigger input (D0) | Input | Active LOW from P4 GPIO 34 |
| 2 | SPI CS | Input | From P4 CS0-CS3 |
| 7 | SPI CLK | Input | From P4 GPIO 37 (shared with SD CLK) |
| 8 | SPI MISO | Output | To P4 GPIO 50 via 330Ω (shared with SD MISO) |
| 9 | SPI MOSI | Input | From P4 GPIO 38 (shared with SD MOSI) |
| 21 | SD card CS | Output | Disabled when SPI slave active |

---

## Component Bill of Materials

### Per-Camera Components (×4)

| Component | Value | Package | Qty | Placement |
|-----------|-------|---------|-----|-----------|
| Resistor | 330Ω | 1/4W axial or 0805 | 1 | Series on MISO (S3 GPIO 8 to bus) |
| Capacitor | 100pF | Ceramic, C0G/NP0, 50V | 1 | CLK to GND at S3 end |
| Resistor | 10KΩ | 1/4W axial or 0805 | 1 | CS to 3.3V pull-up at S3 end |

### Master-Side (ESP32-P4) Components

| Component | Value | Package | Qty | Placement | Status |
|-----------|-------|---------|-----|-----------|--------|
| Resistor | 10KΩ | 1/4W axial or 0805 | 1 | MISO (P4 GPIO 50) to GND — pulldown | **fitted** |
| Resistor | 33Ω | 1/4W axial or 0805 | 1 | CLK (P4 GPIO 37) series termination at source | **fitted** |
| Resistor | 33Ω | 1/4W axial or 0805 | 1 | MOSI (P4 GPIO 38) series termination at source | **fitted** |

**All fitted**: 4× 330Ω + 4× 100pF + 4× 10KΩ (slave CS pullups) + 1× 10KΩ (master MISO pulldown) + 2× 33Ω (master CLK/MOSI source termination) = **15 components total**

### Planned for v2 PCB (not yet built)

| Component | Value | Package | Qty | Placement |
|-----------|-------|---------|-----|-----------|
| SN74HC125N | — | DIP-14 or SOIC-14 | 1 | Quad tri-state buffer on MISO lines — one gate per camera, each gate's OE tied to that camera's CS. Permanently eliminates the ESP-IDF SPI-slave MISO contention ([#8638](https://github.com/espressif/esp-idf/issues/8638)). See [SN74AHCT125N-circuit.md](SN74AHCT125N-circuit.md) for detail. |
| Ceramic cap | 100nF X7R | 0603 | 1 | Decoupling for the '125 |

Plus shared wiring: CLK, MOSI, MISO, TRIGGER bus wires + 4 individual CS wires + GND = **9 wires minimum**
