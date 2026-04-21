# ESP32-C6 as a GIF-Pipeline Coprocessor — Research

**Status**: research only — no code changes proposed yet.
**Question**: can we offload GIF-pipeline work to the onboard C6 to make capture-to-GIF faster?
**Short answer**: JPEG decode on the C6 is a **net loss**. Palette generation, LZW compression, and background I/O are **the viable offload targets**.

---

## 1. The hardware picture

| | ESP32-C6 (onboard coprocessor) | ESP32-P4 (main) |
|---|---|---|
| CPU | RISC-V single-core @ 160 MHz | RISC-V dual-core @ 400 MHz |
| Internal SRAM | 512 KB HP + 16 KB LP | 768 KB HP |
| PSRAM | **None** (chip has no external PSRAM bus) | 32 MB (fragmented, ~6.5–8 MB largest contig) |
| HW JPEG codec | **None** | Yes, but 4:2:0 only — unusable for OV5640 4:2:2 |
| ROM JPEG | tjpgd (software, same algorithm we run on P4) | tjpgd |
| Radios | Wi-Fi 6, BLE 5, Thread, Zigbee | None (delegated to C6) |

**Key takeaway**: the C6 has no hardware JPEG peripheral and no PSRAM. The only JPEG decoder available is the same `tjpgd` software path we already use on the P4 — running on a ~2.5× slower core.

## 2. The transport situation — verified from schematic

Confirmed against `SCH_ESP32-P4-EYE-MB_V2.3_20250416.pdf`: the P4↔C6 link is **UART only**. There are **no SDIO lines between the two chips**. The P4's SDIO/SDMMC pins (SD2_D0–D3) route exclusively to the microSD card slot, not the C6.

| Signal | Schematic net | P4 GPIO | C6 pin | Test point |
|---|---|---|---|---|
| C6 chip enable | `C6_EN` | GPIO9 | EN | TP44 |
| Boot strap | `C6_BOOT` | GPIO33 | IO9 | TP45 |
| C6 UART0 RX (P4 → C6) | `C6_U0RXD` | GPIO35 (via R300, 0 Ω) | U0RXD | TP46 |
| C6 UART0 TX (C6 → P4) | `C6_U0TXD` | GPIO36 (via R250, 0 Ω) | U0TXD | TP47 |

(Matches the pin list in esp-dev-kits issue [#134](https://github.com/espressif/esp-dev-kits/issues/134) exactly.)

Additional C6 IOs that appear on the schematic but aren't traced to the P4 in the text extraction: `C6_IO0` and `C6_IO1` go through 0 Ω jumpers (R298 / R299), `C6_IO8` has a 10 KΩ pull-up (R135) — the standard C6 bootstrap config. If esp_hosted UART needs a hardware-handshake line (RTS/CTS) or a "slave ready" IRQ, it would have to go over one of these IO0/IO1 jumpers, which may or may not be populated out of the factory.

**Implication for this codebase**: `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y` in `factory_demo/sdkconfig` is **wrong for the P4-EYE hardware** — there is no physical SDIO path between the chips. For esp_hosted to actually talk to the C6, the config must switch to `CONFIG_ESP_HOSTED_UART_HOST_INTERFACE=y` with TX=GPIO35, RX=GPIO36. This almost certainly explains the Phase-5 on-device WiFi association failure documented in `docs/phase-plan.md:39`.

**Throughput bounds** (UART is the hard ceiling):

| Baud rate | Effective bytes/s | Notes |
|---|---|---|
| 115,200 | ~11 KB/s | esp_hosted UART default |
| 921,600 | ~90 KB/s | typical "fast" UART |
| 2,000,000 | ~195 KB/s | conservative high-speed target |
| 5,000,000 | ~500 KB/s | silicon max (`CONFIG_SOC_UART_BITRATE_MAX=5000000`), needs short traces + flow control |

**~500 KB/s is the absolute best case** — 20× less than the SDIO ceiling I originally assumed. This dominates every architecture decision below.

## 3. Why JPEG decode offload fails

A single OV5640 square-crop output is 1920×1920 RGB565 = **7.37 MB**. Three separate walls:

1. **Memory**: won't fit in the C6's 512 KB SRAM at all. Would require row-by-row streaming, framed per MCU block.
2. **Transport**: even streaming, 7.37 MB over UART @ 5 Mbaud ≈ **15 seconds per frame** — vs P4's current ~2.2 s of tjpgd.
3. **CPU**: C6 running the same tjpgd algorithm on a 160 MHz single-core vs P4's 400 MHz — realistically 5–7 s per frame of pure decode work before any transfer.

Best case (C6 decodes one of four frames in parallel): C6 is the bottleneck at ~20 s when P4 would have finished the same frame in ~2.2 s. **Offloading decode makes the pipeline slower, not faster.**

## 4. What CAN pay off — offload candidates

The shape of a good C6 job: **small input, small output, CPU-bound integer math**. Three candidates, in order of expected win:

### 4a. Palette generation (median-cut quantization) — BEST FIT

- Today: `gif_encoder.c` pass 1 decodes all 4 frames to build the global 256-color palette (~11 s total, mostly decode time).
- Offload shape: P4 downscales each frame *once* to a thumbnail as a by-product of its own decode. Ships the thumbnail to C6.
- C6 runs median-cut on the 4 thumbnails, returns a 256×3-byte palette (**768 bytes**).
- **Thumbnail-size tradeoff** (UART @ 2 Mbaud ≈ 200 KB/s, 4 frames):

  | Thumbnail | Bytes/frame (RGB565) | 4-frame UART cost | Palette quality vs full-res |
  |---|---|---|---|
  | 160×160 | 51 KB | ~1.0 s | acceptable — same clusters, coarser centroids |
  | 240×240 | 115 KB | ~2.3 s | good |
  | 480×480 | 460 KB | ~9.2 s | near-identical to full-res |

  **Recommended**: 160×160 or 240×240. The larger 480×480 option loses any win to UART transfer time at ≤2 Mbaud. At 5 Mbaud the economics shift — 480×480 costs ~3.7 s.
- Parallelism: runs *while* P4 is doing pass-2 encoding, so most of the UART cost is hidden behind existing encode work. The net win comes from C6 running median-cut (currently 2–3 s of P4's pass-1 time) in parallel rather than serially.
- **Estimated speedup**: ~2–3 s off the 50 s total pipeline. Modest but real, and it validates the coprocessor transport end-to-end.

### 4b. LZW sub-block compression — NOT VIABLE on this hardware

- Today: `gif_encoder.c` pass 2 LZW-encodes 1920×1920 indexed pixels per frame. Work is bit-twiddly integer code that's highly cache-sensitive.
- Offload shape would require streaming 3.7 MB of indexed pixels per frame to C6. At UART's 500 KB/s ceiling that's ~7.4 s of transfer per frame — larger than the LZW work itself on the P4.
- Board-level rework (adding an SDIO bus between P4 and C6) would change this calculus, but that's out of scope.
- **Shelved.** Revisit only if a future board revision adds a high-speed C6 transport.

### 4c. Background network / upload — FREE WIN

- The C6 is already the network stack. When a GIF encode finishes, spin off a C6 task to POST / WebSocket / push the file to a backend, reading it from the SD card over the existing esp_hosted data plane.
- No new offload logic — just use the C6 for what it's already there for, without letting it sit idle after WiFi association.
- This *doesn't* speed GIF encoding, but it removes "sync upload" as a future bottleneck.

## 5. Proposed architecture (if we pursue 4a)

```
┌────────────────────────────────────────────────────────────┐
│ ESP32-P4 (existing pipeline)                               │
│                                                            │
│  tjpgd decode ──► downsampled thumbnail (160–240 px)       │
│      │                    │                                │
│      │                    └──► ESP-Hosted Custom RPC ──┐   │
│      ▼                                                 │   │
│  pass-2 indexing + LZW encode                          │   │
│      ▲                                                 │   │
│      │                    ┌── palette (768 bytes) ◄────┤   │
└──────┼────────────────────┼────────────────────────────┼───┘
       │                    │                            │
       │   UART @ ≤5 Mbaud  │ GPIO35 (TX) / GPIO36 (RX)  │
       │   via esp_hosted   │ enable GPIO9, boot GPIO33  │
┌──────┴────────────────────┴────────────────────────────┴───┐
│ ESP32-C6 (slave)                                           │
│                                                            │
│  esp_hosted-slave + custom RPC handler:                    │
│    - receive 4 thumbnails                                  │
│    - median-cut quantize to 256 colors                     │
│    - return palette                                        │
└────────────────────────────────────────────────────────────┘
```

### RPC design

esp-hosted-mcu explicitly supports custom RPCs: *"The Remote Procedure Call (RPC) used by ESP-Hosted can be extended to provide any function required by the Host, as long as the co-processor can support it."* The transport reserves `ESP_PRIV_IF` and `ESP_TEST_IF` interface types for non-Wi-Fi payloads, so we don't fight the existing Wi-Fi RPC surface.

Minimum additions:
- New protobuf message pair in `common/proto/` (e.g. `Rpc_Req_PaletteQuantize` / `Rpc_Resp_PaletteQuantize`).
- Slave-side handler on the C6 that runs median-cut.
- Host-side helper on the P4 that the `gif_encoder` calls instead of its current `build_global_palette()`.

## 6. Pitfalls and prerequisites

Before any of this ships, four known traps documented by Espressif or in our own codebase:

1. **Flashing the C6 via the P4 bridge is unreliable.** Issue [#134](https://github.com/espressif/esp-dev-kits/issues/134) reports the C6 never responds to esptool SYNC when flashed via the P4 USB↔UART bridge, even with forced BOOT/EN. Plan a direct-wire flash path (TP44–TP47) with an external USB-TTL before committing to custom C6 firmware.
2. **`bsp_p4_eye_init()` resets the C6.** Issue [#135](https://github.com/espressif/esp-dev-kits/issues/135) identifies `rtc_gpio_init(BSP_C6_EN_PIN)` at `esp32_p4_eye.c:368` as force-resetting the C6, clobbering its state. Our `app_p4_net.c:166` already works around this with `rtc_gpio_hold_dis()` and careful ordering, but if we keep the C6 running continuously for coprocessor RPC (not just on-demand for WiFi) we need to either (a) skip the BSP's RTC GPIO init or (b) bring the C6 up *after* BSP init.
3. **Transport is wired but not yet configured.** The schematic confirms UART on GPIO35/36 (§2), but the current `sdkconfig` selects SDIO and the BSP doesn't declare the UART pins. Before any C6-side code can run, `sdkconfig` must switch to `CONFIG_ESP_HOSTED_UART_HOST_INTERFACE=y` and the BSP should add `BSP_C6_UART_TX_PIN` / `BSP_C6_UART_RX_PIN` defines matching the schematic.
4. **WiFi-SPI coexistence risk.** `docs/wifi-spi-coexistence.md` and the [WiFi breaks SPI](../memory) memory both document that Wi-Fi activity on the C6 destabilizes the S3-camera SPI slaves. For GIF offload we'd want the C6 running *without* Wi-Fi active during captures. Since the C6 is doing pure compute (no radio) for palette/LZW, this is mitigable — just don't call `esp_wifi_start()` in the same session.

## 7. Unknowns worth resolving before building

- [x] ~~Confirm physical C6↔P4 transport wiring on the P4-EYE (UART vs SDIO).~~ **Done**: UART only (§2). `sdkconfig` SDIO selection must be changed.
- [ ] Flip `sdkconfig` from `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y` to `CONFIG_ESP_HOSTED_UART_HOST_INTERFACE=y` with TX=GPIO35, RX=GPIO36 — this may also unblock the Phase-5 WiFi integration separately from any coprocessor work.
- [ ] Benchmark actual achievable UART baud rate on the P4-EYE's routed traces (reliable at 5 Mbaud? 2 Mbaud?). Test at incremental baud rates up from 921600.
- [ ] Check whether C6_IO0 / C6_IO1 (via R298 / R299, 0 Ω jumpers) are populated, and if so what P4 GPIO they land on — needed for esp_hosted UART flow control or "slave ready" IRQ.
- [ ] Identify the C6-side flash path that works without the broken P4 USB-bridge (TP45 + TP46/47 direct-wire to external USB-TTL is probably the only reliable option — issue #134).
- [ ] Verify whether the C6 currently ships with stock `esp_hosted-slave` firmware, and whether that firmware can be safely replaced with an extended build (or if it needs to coexist via the custom RPC interface).
- [ ] Measure current `gif_encoder.c` pass-1 palette stage in isolation — confirm the 11 s number and how much of it is median-cut vs decode.

## 8. Recommendation

Don't pursue JPEG-decode offload. The math is fundamentally against it regardless of transport.

**Do** pursue **palette quantization offload (§4a)** as the first coprocessor use case, *after* resolving the transport verification (§7.1) and the flashing path (§7.2). It's the smallest plausible change that actually saves wall-clock time during GIF creation, it uses the C6 for what it's well-suited to (integer compute on small data), and it sets up the RPC infrastructure for future small-data offloads without over-committing.

## References

- [ESP32-P4-EYE-MB schematic v2.3 (2025-04-16)](https://dl.espressif.com/AE/esp-dev-kits/SCH_ESP32-P4-EYE-MB_V2.3_20250416.pdf) — definitive P4↔C6 wiring
- [ESP32-C6 Datasheet (Espressif)](https://www.espressif.com/sites/default/files/documentation/esp32-c6_datasheet_en.pdf)
- [ESP32-P4-EYE User Guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-eye/user_guide.html)
- [esp-hosted-mcu README](https://github.com/espressif/esp-hosted-mcu/blob/main/README.md) — custom RPC extensibility
- [esp-hosted-mcu SDIO docs](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/sdio.md) — default pins, throughput numbers
- [ESP32-C6 lacks external PSRAM support](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/storage/psram.html)
- [P4↔C6 SDIO throughput benchmark](https://github.com/r4d10n/esp32p4-c6-wifi-test)
- [esp-dev-kits issue #134](https://github.com/espressif/esp-dev-kits/issues/134) — P4 bridge can't flash C6
- [esp-dev-kits issue #135](https://github.com/espressif/esp-dev-kits/issues/135) — `bsp_p4_eye_init()` resets the C6
