# SN74HC125N (Tri-State Buffer) — MISO Isolation Circuit

## Why we need this

The ESP32 SPI slave driver **actively drives MISO even when CS is HIGH** (see [esp-idf #8638](https://github.com/espressif/esp-idf/issues/8638)). On a shared-bus setup with 4 slaves, that means 3 deselected slaves fight the 1 selected slave on the MISO line — the 330Ω series resistors on each S3's MISO limit the contention *current*, but don't tri-state anything. The selected slave wins only by winning a voltage divider against 3 other drivers. The marginal slave (cam 4) loses.

A 74HC125 tri-state buffer, one gate per camera's MISO, gated by that camera's CS, solves this at the hardware level: when a camera's CS is HIGH, its gate's output is genuinely high-impedance — truly disconnected from the bus. No contention, no voltage divider, no guesswork.

---

## Chip overview — SN74HC125N

The SN74HC125N is a **quad tri-state bus buffer** — four independent buffers in one 14-pin package. That's exactly what we need: one buffer per camera, all in a single IC.

- **Vcc range:** 2.0V – 6.0V → runs perfectly at 3.3V
- **Outputs:** swing rail-to-rail at Vcc (so at 3.3V in, you get 3.3V out — safe for the ESP32-P4)
- **Propagation delay:** ~15 ns at 3.3V — negligible at 10 MHz SPI (100 ns bit period)
- **4 gates per chip:** exactly enough for 4 cameras
- **Each gate has its own OE (output-enable) pin:** active-LOW, which conveniently matches how SPI CS works

---

## Do I really need one chip per camera? (No — use one for all 4)

**One SN74HC125N can serve all four cameras.** That's the whole point of a quad buffer. Each of its 4 gates has:
- Its own input (nA)
- Its own output (nY)
- Its own independent enable (nOE)

So you wire each camera's MISO to a different gate's input, each camera's CS to that same gate's enable, and all four gate outputs merge onto the shared MISO bus going to the P4. When only one CS is LOW at a time (which is how SPI always works), only that one gate drives the shared bus; the other three are Hi-Z.

Bottom line: **1 chip + 1 decoupling cap, and you're done for all four cameras.**

The "one per camera" layout from the earlier doc was about optimizing signal integrity by putting the buffer right next to each S3. That matters for ≥50 MHz SPI, but at 10 MHz the difference is not measurable. Skip the complexity — use one chip.

---

## Pinout (DIP-14 / SOIC-14)

```
           ┌────────∪────────┐
    1OE ──┤ 1            14 ├── Vcc  (3.3V with 100nF decoupling cap)
     1A ──┤ 2            13 ├── 4OE
     1Y ──┤ 3            12 ├── 4A
    2OE ──┤ 4  74HC125   11 ├── 4Y
     2A ──┤ 5            10 ├── 3OE
     2Y ──┤ 6             9 ├── 3A
    GND ──┤ 7             8 ├── 3Y
           └─────────────────┘

Per-gate behavior:
   nOE = LOW   →  nY follows nA  (buffer enabled, signal passes)
   nOE = HIGH  →  nY = Hi-Z       (output disconnected from bus)
```

SPI's CS is active-LOW (goes LOW when a slave is selected). The buffer's OE is active-LOW (enabled when pulled LOW). They line up perfectly — **we tie each camera's CS directly to that gate's OE pin**, no inverter needed.

---

## Single-IC wiring — the whole circuit

```
                       ESP32-P4 side                                SN74HC125N
                                                               ┌─────────────────┐
                                                               │                 │
   Cam 1 CS (P4 GPIO 51) ─────────────────────────────────────→│ 1  (1OE)        │
   Cam 1 MISO (from S3 #1 GPIO 8) ───────────────────────────→ │ 2  (1A)         │
                                                               │                 │
                                                          ┌────│ 3  (1Y)         │
                                                          │    │                 │
                                                          │    │                 │
   Cam 2 CS (P4 GPIO 52) ─────────────────────────────────│───→│ 4  (2OE)        │
   Cam 2 MISO (from S3 #2 GPIO 8) ────────────────────────│──→ │ 5  (2A)         │
                                                          │    │                 │
                                                          ├────│ 6  (2Y)         │
                                                          │    │                 │
                                                      ────┤    │ 7  (GND) ────── GND
                                                      │   │    │                 │
                                                      │   ├────│ 8  (3Y)         │
                                                      │   │    │                 │
   Cam 3 MISO (from S3 #3 GPIO 8) ────────────────────│───│───→│ 9  (3A)         │
   Cam 3 CS (P4 GPIO 53) ─────────────────────────────│───│───→│ 10 (3OE)        │
                                                      │   │    │                 │
                                                      │   ├────│ 11 (4Y)         │
                                                      │   │    │                 │
   Cam 4 MISO (from S3 #4 GPIO 8) ────────────────────│───│───→│ 12 (4A)         │
   Cam 4 CS (P4 GPIO 54) ─────────────────────────────│───│───→│ 13 (4OE)        │
                                                      │   │    │                 │
                                                      │   │    │ 14 (Vcc) ─┐     │
                                                      │   │    └───────────│─────┘
                                                      │   │                │
                                                      │   │            ┌───┴───┐
                                                      │   │          3.3V    100nF
                                                      │   │                    │
                                                      │   │                   GND
                                                      │   │                     
                                                      │   │
                                                      ↓   ↓
                                     merged shared MISO bus (all 4 Y outputs)
                                                      │
                                                      │ (the existing 10kΩ
                                                      │  pull-down at P4
                                                      │  stays right here)
                                                      ↓
                                             P4 GPIO 50 (MISO input)
```

That's it. Every gate's input is the S3's raw MISO. Every gate's enable is the same CS line the P4 already drives. Every gate's output merges onto the bus. Only the selected camera's gate is active; the other three are Hi-Z and don't contribute anything to the bus.

### Why this works cleanly

- When P4 pulls **CS51 LOW** to talk to cam 1: gate 1 is enabled, gate 2/3/4 are Hi-Z. Only cam 1's MISO reaches the P4. No contention.
- When P4 pulls **CS54 LOW** to talk to cam 4: gate 4 is enabled, gate 1/2/3 are Hi-Z. Only cam 4's MISO reaches the P4. No contention.
- When P4 has **all CS HIGH** (between transactions): all 4 gates are Hi-Z. The 10kΩ pull-down at the P4 holds the line at a clean 0V.

---

## Where to physically place the chip

The single chip should sit **at or near the P4** — right at the point where all 4 camera MISO wires and all 4 CS wires already converge. That way:
- The Y-output rail going to the P4's GPIO 50 is a short stub
- The 10kΩ pull-down you already have can sit right at that same point
- You're essentially building a mini "MISO hub" near the master

The inputs (each camera's MISO) still travel the same wire length they do today — from each camera to this central point. So no new long-wire issues; we're just inserting the buffer at the endpoint.

---

## The 330Ω series resistors

You currently have a 330Ω between each S3's MISO pin and the shared bus. With the buffer in place, those resistors become **electrically redundant** — the buffer's tri-state does what the resistor was trying to do (limit contention), and does it properly.

**Recommendation: leave them in for now.** They're harmless — they just add ~330Ω to the impedance the buffer input sees, which matters not at all. Leaving them means you can compare "before/after" cleanly, and if you ever have to revert, nothing's lost.

---

## Unused inputs? No — all 4 gates are used

Unlike the "one chip per camera" approach (which only uses 1 of 4 gates), the single-chip approach uses all 4 gates. No unused pins to tie off.

---

## BOM

| Component | Value | Package | Qty |
|---|---|---|---|
| SN74HC125N | — | DIP-14 | **1** |
| Ceramic decoupling cap | 100nF X7R, 50V | 0603 / 0805 / radial | **1** |

That's it. Two components to install.

---

## Physical install notes

- **100nF cap placement matters.** Put it directly across pin 14 (Vcc) and pin 7 (GND), with leads as short as physically possible. At 10 MHz, even 10 mm of lead adds inductance that cuts decoupling effectiveness.
- **Ground connection must be solid.** The chip's GND (pin 7) needs a low-impedance path to the same ground the P4 and all S3s share. Star-ground at the P4 is fine; a ground plane is better.
- **Socket it if you're uncertain.** A 14-pin DIP socket lets you pull the chip for A/B testing without desoldering.
- **Keep the Y-output rail short.** The node where 1Y, 2Y, 3Y, 4Y all merge and feed the P4's GPIO 50 should be the shortest stub on the whole board. That's the "bus" — keep it tight.

---

## Testing plan after install

1. **Power up just one camera**, keeping the others physically unplugged (or at least the CS/MISO lines disconnected). Run `spi_capture_all`. That camera should work 100% first-try — sanity check that wiring is correct.
2. **Add cameras back one at a time**, running captures after each. With the buffer, every addition should be clean. If cam 4 still flakes after adding, the issue is on cam 4's side (the wire from cam 4 to the chip input, or the S3 itself) — not bus contention.
3. **Once all 4 are in, run 20 back-to-back `spi_capture_all`.** Target: 4/4 first-try on every run. The bus-contention root cause is gone; remaining flakiness would be isolated per-camera wiring.

---

## What this fixes (and what it doesn't)

**Fixes:** MISO bus contention (the documented [#8638](https://github.com/espressif/esp-idf/issues/8638) issue). All 4 cameras coexist cleanly on the shared bus regardless of how many are present.

**Doesn't fix:**
- Individual broken solder joints or loose wires per-camera (still inspect cam 4's wiring)
- The SPI control-byte corruption issue (that's a master-side SPI timing problem, separate from bus contention)
- The CS51 / CS54 ends-of-bus flakiness we saw earlier (but it *should* disappear once contention is gone — the only reason CS51/54 were more flaky was that they were the most marginal voltage-divider losers)

---

## References

- [ESP-IDF #8638 — "SPI Slave Driver holds down MISO when DMA is disabled"](https://github.com/espressif/esp-idf/issues/8638) — Espressif-acknowledged issue; 74x125 is the recommended hardware workaround
- [TI SN74HC125 datasheet](https://www.ti.com/product/SN74HC125) — quad tri-state buffer, 2V–6V Vcc, pinout matches all other 74x125 variants
