# P4MSLO Complete Circuit Diagram — v2 (SN74HC125N revision)

> **v2 PCB.** Only change from [`circuit-diagram.md`](circuit-diagram.md) (v1): insertion of a single SN74HC125N quad tri-state buffer on the MISO lines and removal of the 4× 330Ω series resistors that each S3 had on its MISO output. See [`SN74AHCT125N-circuit.md`](SN74AHCT125N-circuit.md) for the rationale (ESP-IDF [#8638](https://github.com/espressif/esp-idf/issues/8638) MISO-driven-while-deselected workaround).
>
> **CS naming in this doc:** for readability I use **CS1–CS4** (matching camera 1–4). The P4 firmware internally labels these 0-indexed (`spi_device_cs_0 … cs_3`) but the physical pins are the same: CS1=GPIO 51, CS2=GPIO 52, CS3=GPIO 53, CS4=GPIO 54.
>
> Everything else — on-board P4X-EYE peripherals, S3 internal wiring, power distribution, 33Ω source-termination on the P4's CLK/MOSI, 10KΩ pulldown on the P4's MISO, 100pF on each S3's CLK (to GND, *not* to 5V), 10KΩ CS pullup at each S3 — is unchanged from v1.

---

## 1. Big-picture: what goes through the '125 and what doesn't

There are 5 SPI signals on this bus. Only MISO goes through the SN74HC125N. Everything else is direct point-to-multipoint, exactly as in v1.

```
                        ┌───────────────────┐
    CLK      ───────────┤                   ├───────→ S3 cam 1  GPIO 7
    (P4 GPIO 37, 33Ω)   │    direct bus,    ├───────→ S3 cam 2  GPIO 7
                        │    no buffer      ├───────→ S3 cam 3  GPIO 7
                        │                   ├───────→ S3 cam 4  GPIO 7
                        └───────────────────┘

                        ┌───────────────────┐
    MOSI     ───────────┤                   ├───────→ S3 cam 1  GPIO 9
    (P4 GPIO 38, 33Ω)   │    direct bus,    ├───────→ S3 cam 2  GPIO 9
                        │    no buffer      ├───────→ S3 cam 3  GPIO 9
                        │                   ├───────→ S3 cam 4  GPIO 9
                        └───────────────────┘

                        ┌───────────────────┐
    TRIGGER  ───────────┤                   ├───────→ S3 cam 1  GPIO 1
    (P4 GPIO 34)        │    direct bus,    ├───────→ S3 cam 2  GPIO 1
                        │    no buffer      ├───────→ S3 cam 3  GPIO 1
                        │                   ├───────→ S3 cam 4  GPIO 1
                        └───────────────────┘

    CS1 (P4 GPIO 51) ───┬──→ S3 cam 1 GPIO 2           (2 destinations
                        └──→ '125 pin 1  (1OE)          on same net,
    CS2 (P4 GPIO 52) ───┬──→ S3 cam 2 GPIO 2           one per camera)
                        └──→ '125 pin 4  (2OE)
    CS3 (P4 GPIO 53) ───┬──→ S3 cam 3 GPIO 2
                        └──→ '125 pin 10 (3OE)
    CS4 (P4 GPIO 54) ───┬──→ S3 cam 4 GPIO 2
                        └──→ '125 pin 13 (4OE)

                        ╔═══════════════════╗
    P4 GPIO 50 (MISO)   ║                   ║
         ↑              ║                   ║
         │              ║    SN74HC125N     ║
         │     1Y ──────╫─┐                 ║           ◄──── S3 cam 1 GPIO 8  (into 1A, pin 2)
         └──── 2Y ──────╫─┤                 ║           ◄──── S3 cam 2 GPIO 8  (into 2A, pin 5)
               3Y ──────╫─┤  ONLY MISO      ║           ◄──── S3 cam 3 GPIO 8  (into 3A, pin 9)
               4Y ──────╫─┘  goes through   ║           ◄──── S3 cam 4 GPIO 8  (into 4A, pin 12)
                        ║  here             ║
                        ║                   ║
                        ╚═══════════════════╝
```

The 4× `nA` input pins each have their own private wire (no sharing). The 4× `nY` output pins are tied together into one rail — safe because at any instant only the selected gate is driving; the other three are Hi-Z.

---

## 2. SN74HC125N pinout + complete pin-by-pin connection table

```
              ┌────────∪────────┐
       1OE ──┤ 1            14 ├── Vcc (3.3V)
        1A ──┤ 2            13 ├── 4OE
        1Y ──┤ 3            12 ├── 4A
       2OE ──┤ 4            11 ├── 4Y
        2A ──┤ 5  74HC125   10 ├── 3OE
        2Y ──┤ 6             9 ├── 3A
       GND ──┤ 7             8 ├── 3Y
              └─────────────────┘
```

| Pin | Name | I/O    | Wired to                                                      | Shared? |
|-----|------|--------|---------------------------------------------------------------|---------|
| 1   | 1OE  | input  | **CS1** (P4 GPIO 51) — same net also goes to S3 cam 1 GPIO 2  | No (just the CS1 net) |
| 2   | 1A   | input  | **S3 cam 1 GPIO 8** (MISO out)                                | **No — private to cam 1** |
| 3   | 1Y   | output | Merged MISO rail → P4 GPIO 50                                 | **Yes — tied to 2Y, 3Y, 4Y** |
| 4   | 2OE  | input  | **CS2** (P4 GPIO 52) — same net also goes to S3 cam 2 GPIO 2  | No (just the CS2 net) |
| 5   | 2A   | input  | **S3 cam 2 GPIO 8** (MISO out)                                | **No — private to cam 2** |
| 6   | 2Y   | output | Merged MISO rail → P4 GPIO 50                                 | **Yes — tied to 1Y, 3Y, 4Y** |
| 7   | GND  | power  | Board GND (common with P4 and all S3s)                        | — |
| 8   | 3Y   | output | Merged MISO rail → P4 GPIO 50                                 | **Yes — tied to 1Y, 2Y, 4Y** |
| 9   | 3A   | input  | **S3 cam 3 GPIO 8** (MISO out)                                | **No — private to cam 3** |
| 10  | 3OE  | input  | **CS3** (P4 GPIO 53) — same net also goes to S3 cam 3 GPIO 2  | No (just the CS3 net) |
| 11  | 4Y   | output | Merged MISO rail → P4 GPIO 50                                 | **Yes — tied to 1Y, 2Y, 3Y** |
| 12  | 4A   | input  | **S3 cam 4 GPIO 8** (MISO out)                                | **No — private to cam 4** |
| 13  | 4OE  | input  | **CS4** (P4 GPIO 54) — same net also goes to S3 cam 4 GPIO 2  | No (just the CS4 net) |
| 14  | Vcc  | power  | 3.3V rail — **100nF X7R across pins 14↔7, leads short**       | — |

Truth table for one gate (all four are identical):

| OE (= CS) | A (= S3 MISO) | Y (= shared MISO rail) |
|-----------|---------------|------------------------|
| LOW       | whatever S3 is driving | follows A |
| HIGH      | whatever S3 is driving | **Hi-Z (truly disconnected)** |

---

## 3. The key insight: why 1A–4A are NOT shared but 1Y–4Y ARE

This is the part I think tripped you up in my last draft — understandably, because "they both go toward MISO" isn't quite right.

**1A–4A are 4 separate wires, one per camera.** There are literally 4 physical traces on the PCB going from the 4 cameras into the 4 A pins. They never touch each other. The '125 is reading 4 independent signals, one per gate.

**1Y–4Y are 4 pins tied together into 1 wire.** All four Y pins are shorted at the board — that's the "merged MISO rail" that goes to P4 GPIO 50. This is allowed ONLY because the '125 guarantees that at any instant, at most one gate is enabled, so at most one Y pin is actively driving. The other three are in genuine Hi-Z (electrically disconnected from the rail).

**Analogy:** think of the A pins as 4 separate microphones in 4 different rooms. Each gate is a DJ. The Y pins are 4 speakers wired into one PA system. If all 4 DJs tried to talk into the PA at once, you'd get noise — that's the v1 contention problem. The trick: only the DJ whose OE is LOW turns their mic on; the other three DJs are muted (Hi-Z). One speaker, one voice at a time, clean PA.

**The v1 problem in these terms:** in v1 there was no '125 at all — the 4 S3 MISO pins were directly wired together into one rail. The S3 SPI slave firmware has a bug ([#8638](https://github.com/espressif/esp-idf/issues/8638)) where it keeps driving MISO even when deselected. So all 4 DJs were always talking into the PA. The 330Ω per-camera resistors tried to turn them into whispers so the selected DJ would win by volume, but it was marginal.

**The v2 fix:** insert the '125 between the S3 outputs and the shared rail. Now the tri-state guarantee is on the '125 side (which actually honors it), not on the S3 side (which doesn't). The 330Ωs become redundant and come out.

---

## 4. Detailed wiring for Camera 1

Cameras 2, 3, 4 are wired identically — just substitute gate 2 / 3 / 4 and CS2 / CS3 / CS4.

```
    ESP32-P4                              S3 Camera #1
   ┌──────────┐                          ┌──────────────┐
   │ GPIO 37  │──┤33Ω├───── CLK ────┬──→│ GPIO 7       │──┤100pF├── GND
   │ GPIO 38  │──┤33Ω├───── MOSI ───┼──→│ GPIO 9       │
   │ GPIO 34  │────────── TRIGGER ──┼──→│ GPIO 1       │
   │          │                     │   │              │
   │          │                     │   │              │──┤10KΩ├── 3.3V
   │ GPIO 51  │────── CS1 ──┬───────┼──→│ GPIO 2       │
   │ (= CS1)  │              │      │   │              │
   │          │              │      │   │              │
   │          │              │      │   │ GPIO 8       │──┐
   │          │              │      │   │              │  │
   │          │              │      │   └──────────────┘  │
   │          │              │      │                     │ dedicated wire,
   │          │              │      │                     │ private to cam 1
   │          │              │      │                     │
   │          │              ▼      │                     ▼
   │          │   ┌──── SN74HC125N ──────────────┐
   │          │   │                               │
   │          │   │ pin 1  (1OE) ◄── CS1         │   ← enables gate 1
   │          │   │ pin 2  (1A)  ◄──────────────────── from S3 cam 1 MISO
   │          │   │ pin 3  (1Y)  ──┐              │
   │          │   │                │              │   ← gate 1 output
   │          │   │ pin 14 (Vcc) ◄── 3.3V        │
   │          │   │ pin 7  (GND) ──► GND         │
   │          │   │                               │   + 100nF X7R across
   │          │   │ (pins 4,5,6 / 9,10,11 / ... = │     pins 14 ↔ 7
   │          │   │  cameras 2, 3, 4 — wire the  │
   │          │   │  same way, into gates 2,3,4) │
   │          │   └──────┬────────────────────────┘
   │          │          │
   │          │          │ 1Y, 2Y, 3Y, 4Y all tie here
   │          │          ▼
   │ GPIO 50  │◄───── merged MISO rail ──────┬──
   │ (MISO in)│                               │
   │          │                             ┌─┴─┐
   │          │                             │10K│Ω  (pulldown,
   │          │                             └─┬─┘   unchanged from v1)
   │          │                               │
   │          │                              GND
   └──────────┘
```

**Wire count for camera 1:**

| Signal            | From             | To                                    | Private or shared? |
|-------------------|------------------|---------------------------------------|---------------------|
| CLK               | P4 GPIO 37       | S3 cam 1 GPIO 7                       | Shared (via 33Ω)    |
| MOSI              | P4 GPIO 38       | S3 cam 1 GPIO 9                       | Shared (via 33Ω)    |
| TRIGGER           | P4 GPIO 34       | S3 cam 1 GPIO 1                       | Shared              |
| CS1               | P4 GPIO 51       | S3 cam 1 GPIO 2 **and** '125 pin 1    | Unique (2 loads)    |
| MISO (to buffer)  | S3 cam 1 GPIO 8  | '125 pin 2 (1A)                       | **Private** to cam 1 |
| MISO (from buffer)| '125 pin 3 (1Y)  | Merged rail → P4 GPIO 50              | Shared (via Hi-Z)   |

The '125 itself and its 100nF cap are fitted **once**, physically near the P4, at the point where the 4 CS lines and 4 incoming MISO wires converge.

---

## 5. A quick sanity-check example

Suppose the P4 wants to read from camera 3:

1. P4 drives CS3 (GPIO 53) **LOW**. CS1, CS2, CS4 stay **HIGH**.
2. The CS3 net has two destinations:
   - S3 cam 3's GPIO 2 sees LOW → cam 3's SPI slave knows it's selected.
   - '125 pin 10 (3OE) sees LOW → gate 3 is enabled.
3. Cameras 1, 2, 4 see their own CS HIGH → their S3s stay deselected, AND '125 gates 1, 2, 4 are Hi-Z.
4. P4 clocks out bytes on CLK + MOSI. Only cam 3 responds on its MISO pin (GPIO 8).
5. Cam 3's MISO wire carries the signal to '125 pin 9 (3A).
6. Gate 3 is enabled, so pin 8 (3Y) mirrors pin 9 (3A). Pin 8 is physically tied to 1Y, 2Y, 4Y — but those three Y pins are Hi-Z, so they contribute nothing.
7. The merged rail carries cam 3's signal cleanly to P4 GPIO 50.

When P4 raises CS3 HIGH at end of transaction, all 4 gates are now Hi-Z, the 10KΩ pulldown at P4 holds the rail at a clean 0V until the next transaction. No contention anywhere.

---

## 6. Per-camera side components (v2 fitted)

```
   (v1 had a 330Ω between S3 GPIO 8 and the shared bus — REMOVED in v2)

   S3 cam N, GPIO 8 (MISO) ─── direct wire ─── '125 nA input



   P4 GPIO 37 ──┤33Ω├─── CLK bus ─┬── to all 4 S3s
                                   │
   at each S3's GPIO 7:            │
                                   │
        ┌──── GPIO 7 (CLK in) ─────┤
        │                          │
        │                        ┌─┴─┐
        │                        │100│pF  (C0G/NP0, TO GND — not to 5V)
        │                        └─┬─┘
        │                          │
        │                         GND



   P4 GPIO 5N ─── CSn ─┬── S3 cam N, GPIO 2
                        └── '125 corresponding OE (pin 1/4/10/13)

   at each S3's GPIO 2:
        ┌──── GPIO 2 (CS in) ──── 10KΩ ─── 3.3V
```

---

## 7. Master-side CLK / MOSI source termination (unchanged from v1)

33Ω in series with P4 GPIO 37 (CLK) and P4 GPIO 38 (MOSI), placed close to the P4 pins. These damp edge ringing on a 1-driver, 4-receiver CMOS bus. They are on P4-driven signals and have nothing to do with the '125. Empirically responsible for lifting capture reliability from ~40% → ~95% first-try in v1 (see [circuit-diagram.md](circuit-diagram.md) for the full story).

---

## 8. Master-side MISO pulldown (unchanged, still required)

```
   P4 GPIO 50 ──┬── merged MISO rail (= 1Y + 2Y + 3Y + 4Y of '125)
                │
              ┌─┴─┐
              │10K│Ω
              └─┬─┘
               GND
```

In v2 this resistor is arguably *more* important than in v1. With all 4 gates in true Hi-Z between transactions (v2 behavior), the rail has zero active drivers. The 10KΩ to GND is what defines the idle state as a clean 0V instead of a floating mystery voltage.

---

## 9. XIAO ESP32-S3 Sense internal wiring

Unchanged from v1. GPIO 1 = trigger in, GPIO 2 = CS in, GPIO 7 = CLK in, GPIO 8 = MISO out, GPIO 9 = MOSI in. Only wiring change is on the *outside* of GPIO 8 — no 330Ω in v2; the trace runs directly to the '125's nA input.

---

## 10. Power distribution

Unchanged from v1. Only addition: the '125's Vcc (pin 14) taps the 3.3V rail, pin 7 bonds to board GND, and a 100nF X7R sits across pins 14↔7 with short leads (< 10 mm). Quiescent current is under 1 mA — negligible on the rail budget.

---

## 11. Complete pin reference

### ESP32-P4 GPIO assignments

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
| 37 | SPI CLK | Output | SPI3 | 16MHz to S3s (33Ω source-term at P4) |
| 38 | SPI MOSI | Output | SPI3 | P4 → S3 (33Ω source-term at P4) |
| 45 | SD card detect | Input | GPIO | Card presence |
| 46 | SD card enable | Output | GPIO | Power control |
| 47 | Knob B | Input | GPIO | Rotary encoder |
| 48 | Knob A | Input | GPIO | Rotary encoder |
| 50 | SPI MISO | Input | SPI3 | **v2: fed by SN74HC125N 1Y/2Y/3Y/4Y merged rail** |
| 51 | **CS1** | Output | SPI3 | Camera #1 CS + '125 pin 1 (1OE) |
| 52 | **CS2** | Output | SPI3 | Camera #2 CS + '125 pin 4 (2OE) |
| 53 | **CS3** | Output | SPI3 | Camera #3 CS + '125 pin 10 (3OE) |
| 54 | **CS4** | Output | GPIO | Camera #4 CS + '125 pin 13 (4OE) (HW slot swap) |

### ESP32-S3 (XIAO Sense) GPIO assignments

| GPIO | Function | Direction | Notes |
|------|----------|-----------|-------|
| 1 | Trigger input (D0) | Input | Active LOW from P4 GPIO 34 |
| 2 | SPI CS | Input | From P4 CS1-CS4 (same net also drives '125 OE) |
| 7 | SPI CLK | Input | From P4 GPIO 37 (shared with SD CLK) |
| 8 | SPI MISO | Output | **v2: direct wire to SN74HC125N nA input (no 330Ω)** |
| 9 | SPI MOSI | Input | From P4 GPIO 38 (shared with SD MOSI) |
| 21 | SD card CS | Output | Disabled when SPI slave active |

---

## 12. Bill of Materials (v2)

### Per-camera (×4)

| Component | Value | Package | Qty | Placement | Change from v1 |
|-----------|-------|---------|-----|-----------|----------------|
| ~~Resistor~~ | ~~330Ω~~ | — | ~~1~~ | ~~Series on MISO (S3 GPIO 8 to bus)~~ | **REMOVED in v2** — '125 tri-state makes it redundant |
| Capacitor | 100pF | Ceramic, C0G/NP0, 50V | 1 | S3 GPIO 7 (CLK in) **to GND** at S3 end | unchanged |
| Resistor | 10KΩ | 1/4W axial or 0805 | 1 | S3 GPIO 2 (CS in) **to 3.3V** pullup at S3 end | unchanged |

### Master-side (ESP32-P4)

| Component | Value | Package | Qty | Placement | Status |
|-----------|-------|---------|-----|-----------|--------|
| Resistor | 10KΩ | 1/4W axial or 0805 | 1 | P4 GPIO 50 (MISO) to GND — pulldown on merged rail | **unchanged** |
| Resistor | 33Ω | 1/4W axial or 0805 | 1 | P4 GPIO 37 (CLK) series source termination | **unchanged** |
| Resistor | 33Ω | 1/4W axial or 0805 | 1 | P4 GPIO 38 (MOSI) series source termination | **unchanged** |
| **IC** | **SN74HC125N** | **DIP-14 or SOIC-14** | **1** | See §2 pin table — gates 1–4 assigned to cameras 1–4; `nA` = cam MISO in, `nY` = merged to P4 GPIO 50, `nOE` = driven by CSn | **NEW in v2** |
| **Capacitor** | **100nF X7R** | **0603** | **1** | Across SN74HC125N pins 14 (Vcc, 3.3V) and 7 (GND); leads as short as possible | **NEW in v2** |

### Totals

- Per-camera: 2 × 4 = **8**
- Master-side: **5**
- **Total: 13** components (v1 was 15 — four 330Ω out, one IC + one cap in)

---

## 13. What changed from v1 (one-page summary)

| Change | Reason |
|--------|--------|
| **Added**: 1× SN74HC125N | Hardware fix for [#8638](https://github.com/espressif/esp-idf/issues/8638). Each gate's tri-state OE is tied to that camera's CS, so deselected slaves are truly disconnected from the MISO rail. |
| **Added**: 1× 100nF X7R across '125 Vcc↔GND | Standard CMOS IC decoupling. This is **100 nano**farads (power rail), not to be confused with the **100 pico**farads on each S3's CLK line — different cap, different job, different value. |
| **Removed**: 4× 330Ω on each S3 MISO | Redundant once contention is resolved at the source by the '125. |
| **Unchanged**: 33Ω on P4 CLK/MOSI | P4-driven signals, nothing to do with the '125. Keep. |
| **Unchanged**: 10KΩ pulldown on P4 MISO | Still required — defines idle state when all '125 gates are Hi-Z. Arguably more important in v2 than in v1. |
| **Unchanged**: 10KΩ pullup on each S3 CS | Same net now also holds the '125's OE HIGH during P4 boot — gate is Hi-Z at boot, which is the correct idle state. |
| **Unchanged**: 100pF C0G on each S3 CLK to GND | Receiver-side edge-ringing filter, independent of MISO. |

---

## References

- [`circuit-diagram.md`](circuit-diagram.md) — v1 PCB (still accurate for the prototype hardware)
- [`SN74AHCT125N-circuit.md`](SN74AHCT125N-circuit.md) — rationale + test plan for the '125 workaround
- [ESP-IDF #8638](https://github.com/espressif/esp-idf/issues/8638) — "SPI Slave Driver holds down MISO when DMA is disabled"
- [TI SN74HC125 datasheet](https://www.ti.com/product/SN74HC125) — quad tri-state buffer, 2V–6V Vcc
