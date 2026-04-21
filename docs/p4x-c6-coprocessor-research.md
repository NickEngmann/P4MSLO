# Does the ESP32-P4X-EYE Enable a Better C6 Coprocessor Story?

**Status**: research only — no code changes proposed.
**Question**: the sibling doc `c6-coprocessor-research.md` concluded the P4-EYE's UART-only C6 link is the bottleneck. Does the P4X-EYE variant change that?
**Short answer**: **No.** The P4X-EYE is electrically identical to the P4-EYE. Only the P4 silicon revision changed (v3.1+ vs v1.3+). Same C6 pin wiring, same UART transport, same ~500 KB/s ceiling.

---

## 1. What's actually different about the P4X-EYE

From Espressif's own user guide:

> *"The difference between the ESP32-P4X-EYE and the ESP32-P4-EYE is that the main chip on the former has been upgraded to the ESP32-P4 chip revision v3.1 or later version."*

Nothing else. Not PCB layout, not wireless module, not peripheral routing. The ESP32-C6-MINI-1U, the MIPI-CSI camera, the ST7789 LCD, the microSD slot, and every header — all identical.

### Proof: direct schematic diff

Verified against the actual P4X schematic (`SCH_ESP32-P4X-EYE-MB_V2.4_20260202.pdf`, included in the `ESP32-P4X-EYE-EN.zip` reference design from Espressif) compared against the P4-EYE schematic (`SCH_ESP32-P4-EYE-MB_V2.3_20250416.pdf`).

The C6 wiring is **byte-identical** between the two revisions. Every signal matches:

| Signal | P4 GPIO | C6 pin | Test point | P4-EYE V2.3 | P4X-EYE V2.4 |
|---|---|---|---|---|---|
| `C6_EN` | GPIO9 | EN | TP44 | ✓ | ✓ (identical) |
| `C6_BOOT` | GPIO33 | IO9 | TP45 | ✓ | ✓ (identical) |
| `C6_U0RXD` (via R300, 0 Ω) | GPIO35 | U0RXD | TP46 | ✓ | ✓ (identical) |
| `C6_U0TXD` (via R250, 0 Ω) | GPIO36 | U0TXD | TP47 | ✓ | ✓ (identical) |
| `C6_IO0` / `C6_IO1` | 0 Ω jumpers (R298 / R299) | IO0 / IO1 | — | ✓ | ✓ (identical) |
| `C6_IO8` | 10 KΩ pull-up (R135) | IO8 | — | ✓ | ✓ (identical) |

Substantive deltas between the two schematic PDFs (based on layout-preserving text diff):

1. Title block: `ESP32-P4-EYE-MB` → `ESP32-P4X-EYE-MB`, rev 2.3 → 2.4, date 2025-04-16 → 2026-02-03.
2. Internal power-rail net renames: `VDDPST_1` → `VDD_PSRAM_1`, `VDDPST_1` → `VDD_LP` in places — reflecting the rev v3.1+ die's updated power-domain naming, not external wiring.
3. All other diff lines are whitespace/layout only.

The upstream `espressif/esp-dev-kits` documentation repository corroborates this: [`docs/en/esp32-p4x-eye/user_guide.rst`](https://github.com/espressif/esp-dev-kits/blob/master/docs/en/esp32-p4x-eye/user_guide.rst) reuses every schematic figure (including `sch_interface_esp32_c6.png`) from the P4-EYE asset directory, not a P4X-specific one — because the circuits themselves didn't change.

## 2. What the chip-revision bump does and doesn't do

The P4X's rev v3.1+ silicon doesn't unlock new *connectivity* to the onboard C6. Specifically:

| Rev v3.1+ gives us | Relevance to C6 coprocessor |
|---|---|
| Silicon errata fixes (see [ESP32-P4 SoC Errata](https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32p4/index.html)) | None — errata affect camera/USB/cache paths, not the P4↔C6 wire |
| Broader ESP-IDF default support (no need for `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`) | Mild DX win; no transport change |
| Same pin mux, same IOMUX table as v1.3 | Can't route SDIO where no traces exist |
| Same max CPU clock (400 MHz) | No compute shift |

A silicon revision change would only help us if rev v3.1+ added a peripheral that could repurpose the existing UART traces as something faster (e.g., a high-speed serial peripheral that runs on arbitrary GPIOs). It doesn't. Both revisions share the same peripheral set on the same pin mux — it's the ESP32-P4 die, just a metal-layer stepping.

## 3. So the C6 architecture is unchanged

Everything in `docs/c6-coprocessor-research.md` applies to the P4X-EYE identically:

- **Transport**: UART on P4 GPIO35 (TX) / GPIO36 (RX), EN on GPIO9, BOOT on GPIO33 — test points TP44–TP47. No SDIO path.
- **Throughput ceiling**: ~500 KB/s at 5 Mbaud, practical target ~195 KB/s at 2 Mbaud.
- **Viable offload**: palette quantization (§4a of sibling doc). 160–240 px thumbnails, ~50–115 KB/frame.
- **Not viable**: JPEG decode, LZW compression. Both are transport-bound on any ~500 KB/s link.
- **Same prerequisites**: `sdkconfig` still needs `CONFIG_ESP_HOSTED_UART_HOST_INTERFACE=y` instead of the current (impossible) SDIO selection.

## 4. Small P4X-specific wins worth noting

Independent of the C6 story, running rev v3.1+ silicon lets us:

- Drop `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and `CONFIG_ESP32P4_REV_MIN_100=y` from `factory_demo/sdkconfig.defaults` (see CLAUDE.md gotcha #7).
- Use the default ESP-IDF minimum chip revision, which some IDF components check for before enabling optimized paths.
- Pick up any errata-fixed peripherals in rev v3.1+. Worth a skim of the SoC Errata doc if we hit a known rev-v1.x hardware bug.

None of these accelerate GIF creation. They're just cleanup opportunities if the lab has migrated to P4X boards. Note: CLAUDE.md currently states our dev boards are **v1.3** — so **we are on P4-EYE, not P4X-EYE**, unless inventory has refreshed.

## 5. Recommendation

No action needed on the coprocessor architecture for the P4X question specifically. **Keep following the plan in `c6-coprocessor-research.md`**: fix the transport config (SDIO → UART), then prototype palette quantization offload. That work lands identically on either board variant.

If you ever do move to a board revision that routes SDIO between the P4 and C6 (third-party: Waveshare ESP32-P4-Nano, ESP32-P4-Function-EV, etc., all use SDIO), re-open the LZW offload question — that's where the transport ceiling actually bites.

## References

- [ESP32-P4X-EYE User Guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html)
- [P4X-EYE user-guide RST source](https://github.com/espressif/esp-dev-kits/blob/master/docs/en/esp32-p4x-eye/user_guide.rst) — confirms reuse of P4-EYE schematic assets
- [ESP32-P4 Series SoC Errata](https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32p4/index.html) — rev-specific silicon fixes
- `docs/c6-coprocessor-research.md` — full analysis, applies unchanged to the P4X
- `SCH_ESP32-P4-EYE-MB_V2.3_20250416.pdf` — P4-EYE schematic (from Espressif CDN)
- `SCH_ESP32-P4X-EYE-MB_V2.4_20260202.pdf` — P4X-EYE schematic (from `ESP32-P4X-EYE-EN.zip` reference design); C6 subsystem byte-identical to V2.3
