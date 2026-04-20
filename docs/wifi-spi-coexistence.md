# WiFi + SPI Coexistence — Analysis & Options

## Background

As of 2026-04-20, the 4-camera PIMSLO pipeline is fully stable on the SPI side **only when WiFi is disabled** on the ESP32-S3 cameras (`DISABLE_WIFI=1` in `esp32s3/src/config/Config.h`). With WiFi off, 5/5 consecutive captures succeed cleanly across all connected cameras. With WiFi on, one specific camera (CS51, IP 192.168.1.119 in current deployment) returned pure-zero responses on every SPI poll — not corruption, not intermittent, deterministic `[00 00 00 00 00]` every time.

The other two cameras (CS52, CS53) worked with WiFi on. Only one failed. This reproducibility — 100% failure on one unit, 100% success on others running identical firmware — is the key clue.

## Root cause hypotheses (ranked by likelihood)

### 1. WiFi ISR latency starves the CS-edge ISR (most likely)

The slave uses a GPIO interrupt on its CS pin to re-enable MISO output (tri-state release) the moment CS falls. At 10 MHz SPI the master starts clocking ~5 µs after CS↓, so the ISR must complete within that window. WiFi ISRs (Rx, phy coexistence) run at level 1–2 and can hog the CPU for 100–300 µs bursts. Even a 5 µs extra latency means the first 8-byte poll response is clocked out while MISO is still in INPUT mode → master reads all zeros.

Why only camera 1: ISR scheduling is non-deterministic. If camera 1 happened to align its WiFi Rx beacon interval with the master's poll cadence, it would fail every poll — deterministically, but by coincidence.

### 2. TX-power brownout on a marginal USB cable

An S3 WiFi TX burst pulls ~240 mA peaks. If camera 1's USB cable or connector has slightly higher resistance (older cable, marginal solder, dirty contact), local Vcc dips during bursts. A brief dip while the SPI output driver is trying to drive MISO HIGH would produce logic-LOW → zeros on the master side. Since each cable has unique impedance this would be deterministic per-unit.

### 3. Interrupt allocation competition

`gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1)` failed with `No free interrupt inputs` when WiFi was later disabled — meaning WiFi grabbed interrupt slots aggressively. Even when my ISR eventually got allocated, it may have landed on a slower priority/core than intended, compounding (1).

The ~5 µs latency window vs observed all-zeros response is consistent with (1); the per-unit determinism is consistent with (2). Both may contribute.

## Knobs available, ranked by cost/benefit

| # | Strategy | Effort | Expected impact | Risk |
|---|---|---|---|---|
| 1 | `esp_wifi_set_max_tx_power(8)` — drop 20 dBm → 2 dBm | 1 line | 🟢 big — eliminates brownout, still fine for same-LAN | very low |
| 2 | `esp_wifi_set_ps(WIFI_PS_NONE)` — no power save | 1 line | 🟢 medium — removes periodic WiFi wake ISRs | very low |
| 3 | Pin SPI slave task to Core 1 (already done); confirm WiFi tasks run on Core 0 | trivial | 🟢 medium — isolates scheduling | very low |
| 4 | Bump SPI slave task priority 10 → 23 | 1 line | 🟡 medium — WiFi ISRs still preempt tasks, but Core-0/Core-1 split helps | low |
| 5 | Drop SPI clock 10 MHz → 5 MHz | 1 line | 🟡 doubles timing margin; 5 µs ISR latency = 25 bits instead of 50 | medium (slower) |
| 6 | Skip HTTP server startup; keep only minimal OTA listener | ~30 lines | 🟢 large — removes lwIP task churn | low |
| 7 | Add SPI-protocol CRC + retry on corrupt transfers | ~50 lines/side | 🟢 tolerates residual failures rather than preventing them | low |
| 8 | Runtime `esp_wifi_stop()` before GPIO34 trigger, restart after | ~100 lines | 🟢 **guaranteed** — but 200–500 ms per capture for WiFi reconnect | medium |
| 9 | WiFi only when user explicitly enters OTA mode (default off) | bigger refactor | 🟢 best long-term — no overhead in normal use, OTA still works | medium |

## Proposed staged rollout

### Stage 1 — cheap, likely sufficient

Lower TX power to ~2 dBm, disable WiFi power save, skip HTTP server (only expose OTA endpoint). If (2) was dominant, this alone restores 5/5.

### Stage 2 — if Stage 1 is still flaky

Drop SPI clock to 5 MHz. Costs ~1 s per capture across 3 cameras. Acceptable.

### Stage 3 — nuclear fallback

Runtime `esp_wifi_stop()` before GPIO34 trigger, `esp_wifi_start()` + reconnect after. Guarantees zero WiFi interference during the 4-second capture window. Adds ~500 ms reconnect overhead per burst, which is negligible against the existing GIF-encode pipeline timing.

## Strategic question

Before doing any of the above, clarify **what WiFi is actually for** in this product:

- **OTA updates**: yes, needed.
- **Status queries from a phone/laptop**: maybe.
- **Config changes (camera position)**: could equally well go over the existing SPI command path.
- **Live streaming**: not supported regardless.
- **HTTP capture**: nice-to-have but GPIO34 trigger is the canonical path.

If the answer is "mostly OTA with occasional config," option 9 (WiFi on-demand) gives 100% of that value with zero SPI risk. A permanently-on WiFi is paying a constant complexity tax for convenience. The most defensible long-term architecture is probably:

> **WiFi off by default. The P4 sends a SPI command "enter OTA mode" which reboots the slaves into a WiFi-enabled firmware variant; they OTA-reboot back into WiFi-off after the update.**

That's a bigger change but yields a permanently stable product that still updates remotely.

## Recommendation

1. Keep `DISABLE_WIFI=1` on `main` as the known-good configuration.
2. Create a feature branch and try **Stage 1** (TX power + no PS + no HTTP). If that hits 5/5, ship it.
3. If Stage 1 is flaky, skip Stage 2 and go straight to **Stage 3** (runtime stop/start). Chasing WiFi-coexistence tuning past Stage 1 is a time sink for marginal gains.
4. Separately, consider the Option 9 architectural change for the next major revision.

## Appendix — test evidence this analysis is built on

With WiFi on, consistent failure pattern (April 2026):
```
Camera 1 (CS51): FAILED (0x107)  — all polls return [00 00 00 00 00]
Camera 2 (CS52): ✓ 900 KB JPEG transferred
Camera 3 (CS53): ✓ 700 KB JPEG transferred
Camera 4 (CS54): not connected in this test
```

With `DISABLE_WIFI=1`, 5 consecutive runs:
```
Run 1: Camera 1 ✓ 766 KB | Camera 2 ✓ 219 KB | Camera 3 ✓ 745 KB | total 4.56 s
Run 2: Camera 1 ✓ 764 KB | Camera 2 ✓ 219 KB | Camera 3 ✓ 746 KB | total 4.56 s
Run 3: Camera 1 ✓ 754 KB | Camera 2 ✓ 218 KB | Camera 3 ✓ 747 KB | total 4.55 s
Run 4: Camera 1 ✓ 760 KB | Camera 2 ✓ 217 KB | Camera 3 ✓ 747 KB | total 4.55 s
Run 5: Camera 1 ✓ 744 KB | Camera 2 ✓ 219 KB | Camera 3 ✓ 700 KB | total 4.50 s
```

Throughput ~1.2 MB/s per camera; aggregate capture ~4.5 s for 3 cameras.
