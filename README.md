# P4MSLO — 4-Camera Stereoscopic 3D GIF Camera

Firmware for a 4-camera stereoscopic capture rig built on the
ESP32-P4X-EYE. The user presses one button; the device fires four
ESP32-S3 cameras simultaneously over SPI, captures synchronized JPEGs,
and encodes them into a 1920×1920 oscillating "PIMSLO" GIF
(`1→2→3→4→3→2→1`) that gives a 3D-looking parallax effect when played
back. A 240×240 `.p4ms` preview is generated alongside so the gallery
can flash a still instantly while the full GIF finalizes in the
background.

| | |
|---|---|
| **Master board** | ESP32-P4X-EYE v3.1 (RISC-V dual-core @ 400 MHz, 32 MB PSRAM, 16 MB flash, 8 KB TCM) |
| **Capture cameras** | 4× ESP32-S3 with OV5640 (5 MP, 2560×1920) — see `esp32s3/` |
| **Trigger bus** | SPI3 @ 16 MHz, GPIO34 active-LOW shared trigger |
| **Framework** | ESP-IDF v5.5.3 (C) |
| **UI** | LVGL 8.3.11 / SquareLine Studio — 240×240 ST7789 panel |
| **Output** | Per shot: 4 × ~500 KB JPEGs → 1 × ~9 MB GIF (1824×1920, 6 frames) + 1 × ~460 KB `.p4ms` preview |
| **Tests** | 80 unit tests (8 suites) + 14 on-device e2e tests + 31 host-mock scenarios + LVGL simulator |
| **CI** | GitHub Actions — host tests, Docker tests, ESP-IDF cross-compilation |

The original Espressif "P4-EYE factory demo" with face/pedestrian/YOLO
detection has been **stripped out**; this firmware reuses the BSP and
display/camera plumbing but the AI components and most of the original
camera-app screens are gone. The detailed engineering reference is
[CLAUDE.md](./CLAUDE.md). Everything below is a quick orientation.

## What the device does (user-facing)

1. **Photo button (encoder press)** → P4 fires GPIO34, all 4 S3 cams
   capture simultaneously. **The "saving..." overlay clears in
   ~2-3 s** — that's the SPI transfer window. The user can immediately
   take the next photo.
2. **Background save** (~6-12 s, invisible) — `pimslo_save_task` writes
   the 4 JPEGs to SD on Core 1 while the user is already framing the
   next shot.
3. **Background encode** (~150-160 s, invisible) —
   `pimslo_encode_queue_task` decodes the 4 JPEGs, builds a 240×240
   `.p4ms` preview, runs Floyd-Steinberg dithering + LZW into a 6-frame
   1824×1920 GIF.
4. **Gallery (ALBUM page)** — every capture appears immediately as a
   "PROCESSING" entry showing the JPEG preview. Once the encoder
   finishes, it promotes to a playable `.gif`. Up/down knob nav, encoder
   press for delete modal.

The bottom line: the user can fire shots every ~3 s without waiting for
SD writes or GIF encoding to finish.

## Quick start

### Build the firmware (Docker, recommended)

```bash
cd factory_demo
docker run --rm -v "$(pwd)/..:/workspace" -w /workspace/factory_demo \
  --device=/dev/ttyACM0 espressif/idf:v5.5.3 \
  bash -c '. $IDF_PATH/export.sh && idf.py -p /dev/ttyACM0 flash'
```

### Run the host tests (no hardware needed)

```bash
# Original 80-test unit suite
cd test && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc)
for t in test_*; do [ -x "$t" ] && ./"$t"; done

# 31-scenario PIMSLO end-to-end mock harness
cd ../host_encode && ./run.sh
```

### Run the on-device e2e tests

```bash
# Fast heartbeat — 3 tests, ~80 s, gates the full suite
tests/e2e/run_fast.sh

# Full regression — 12 tests, ~10-15 min
tests/e2e/run_all.sh
```

Set `P4MSLO_TEST_PORT=/dev/ttyACMn` if the P4 isn't at `/dev/ttyACM0`.

## Key concepts

### PIMSLO pipeline

Three FreeRTOS tasks across two cores form the photo→GIF pipeline. The
"saving" overlay tracks **only the SPI capture phase**; everything
after is invisible to the user.

```
Core 0                            Core 1
──────                            ──────
pimslo_cap (8 KB internal)        pimslo_save (6 KB PSRAM stack)
  GPIO34 trigger                    fwrite 4× pos*.jpg to SD (~6-12 s)
  SPI receive 4× JPEG (~2 s)        copy P4 photo → preview dir
  hand off to save_queue            enqueue GIF job
  ↓ overlay clears (~3 s post-press)
                                  pimslo_encode_queue_task (16 KB BSS-internal)
                                    .p4ms generation (~7 s)
                                    GIF Pass 1 + Pass 2 (~140 s)

                                  gif_bg (16 KB BSS-internal)
                                    pre-render missing .p4ms
                                    re-encode stale captures
```

See [CLAUDE.md § Pipeline Timing](./CLAUDE.md#pipeline-timing-ov5640-4-cameras-working)
for the full step-by-step timing table.

### Memory layout (why this architecture)

The ESP32-P4 has three memory regions and we use each for a specific
purpose:

| Region | Size | Use |
|---|---|---|
| TCM (`0x30100000`) | 8 KB | 4 KB R4G4B4 palette LUT for the encoder hot loop. Single-cycle access, separate from DMA pool. |
| HP L2MEM (internal) | ~287 KB | Encoder task stacks (BSS-resident), tjpgd workspace, SPI/SD scratch buffers. Reserved 64 KB for DMA-capable allocations. |
| PSRAM | 32 MB | Encoder scaled_buf (7 MB), album PPA buffer (6 MB), frame caches, error-diffusion buffers. |

The encoder's ~150 s GIF run is heap-stable across long sessions — no
`tlsf::remove_free_block` panics, no `setup_dma_priv_buffer` alloc
failures, no SDMMC OOMs. See [CLAUDE.md § Known Issues](./CLAUDE.md#known-issues)
for the diagnosis trail (most of the gotchas were PSRAM-stack overflows
that the FreeRTOS canary doesn't reliably detect on this chip).

## Project layout

```
P4MSLO/
├── factory_demo/             # ESP-IDF master firmware (P4)
│   ├── main/
│   │   ├── app/
│   │   │   ├── Pimslo/       # 4-cam pipeline orchestration
│   │   │   ├── Spi/          # SPI master, 4-cam capture
│   │   │   ├── Gif/          # encoder, decoder, quantizer, tjpgd
│   │   │   ├── Video/        # P4 viewfinder + photo capture
│   │   │   ├── Net/          # P4 WiFi via onboard ESP32-C6 (esp_hosted)
│   │   │   ├── Album/        # legacy P4-photo album (kept compiled, not in menu)
│   │   │   └── ...
│   │   ├── ui/               # SquareLine Studio screens / fonts / images
│   │   └── main.c
│   └── sdkconfig.defaults    # P4X v3.1, 64 KB DMA reserve, HEAP_POISONING_LIGHT
├── esp32s3/                  # Slave camera firmware (one per S3, 4 total)
│   └── src/                  # SPI slave, OV5640 driver, OTA endpoint
├── common_components/
│   └── esp32_p4_eye/         # BSP fork (LVGL trans_size, knob, display)
├── test/                     # Host-side unit + mock infrastructure
│   ├── test_*.c              # 80 unit tests across 8 suites
│   ├── host_encode/          # PIMSLO mock harness (5 runners, 31 e2e scenarios)
│   ├── mocks/                # ESP-IDF mock headers + memory model + budget catalog
│   └── simulator/            # LVGL+SDL2 UI simulator (44 screenshots)
├── tests/e2e/                # On-device e2e tests (run_fast.sh + run_all.sh)
├── docs/                     # Hardware schematics, optimization log, phase plan
├── CLAUDE.md                 # ★ Engineering reference for the master firmware
├── CLAUDE-MOCK.md            # ★ Mock harness reference (architecture validation)
└── README.md                 # this file
```

## Test infrastructure

### Unit suite (host, ~30 s)

| Suite | File | Tests | Covers |
|-------|------|-------|--------|
| NVS Storage | `test/test_nvs_storage.c` | 10 | Settings save/load, photo count, interval state |
| GPIO/BSP | `test/test_gpio_bsp.c` | 12 | Pin state, flashlight, display, I2C, SD detect, knob |
| UI State | `test/test_ui_state.c` | 12 | Page navigation, magnification, AI-mode, transitions |
| AI Buffers | `test/test_ai_buffers.c` | 8 | Buffer init, alignment, circular index, deinit safety (legacy paths kept compiled) |
| Sleep/Wakeup | `test/test_sleep_wakeup.c` | 5 | Wakeup cause, timer+interval, GPIO wakeup |
| UI Simulator | `test/simulator/test_ui_simulator.c` | 13 | Full UI workflow under SDL2, knob debounce, USB interrupt |
| Phase 2 Preview | `test/test_phase2_preview.c` | 9 | Preview-scan, path format, copy-buffer size invariant, fw-version macro |
| Phase 4 Exposure | `test/test_phase4_exposure.c` | 11 | Status-flag disjointness, SPI command uniqueness, AE encode/decode, SET_EXPOSURE wire protocol |

### Mock harness (host, ~2 s) — `test/host_encode/`

A constrained allocator + budget catalog + phase simulator + timing
model lets you validate architecture decisions in <1 second per cycle
instead of 5+ minutes flashing. **31 end-to-end scenarios** cover
PIMSLO photo flows, gallery state, encoder timing under different
LUT/stack placements, the album JPEG-decoder release/reacquire dance,
heap fragmentation under long sequences, and more.

See [CLAUDE-MOCK.md](./CLAUDE-MOCK.md) for the full reference.

### On-device e2e (~10-15 min for full regression)

14 Python scripts that drive the P4 over `/dev/ttyACM0` and assert
against parsed log output. Each test writes its own log file alongside
itself. Verdict reported as PASS/FAIL with a structured summary.

See [tests/README.md](./tests/README.md) for the full test list.

## CI Pipeline

Three jobs run in parallel via GitHub Actions (`.github/workflows/ci.yml`):

| Job | What | Time |
|-----|------|------|
| **host-tests** | Build + run all unit + mock-harness tests on `ubuntu-latest` | ~30 s |
| **docker-test** | Build `Dockerfile.test`, run tests in container | ~1 min |
| **idf-build** | Cross-compile in `espressif/idf:v5.5.3`, upload firmware artifacts | ~4.5 min |

## Hardware

- **ESP32-P4X-EYE v3.1** master + USB-C
- **4× ESP32-S3 + OV5640** capture cameras (XIAO ESP32-S3 sense or equivalent — see `esp32s3/`)
- **SD card** (1+ GB) for photo / GIF storage
- **5V/2A USB power**
- **SPI bus**: 330Ω series resistors on each S3's MISO line (required); recommended 100 pF cap on CLK at each S3 + 10 KΩ pull-ups on each CS

Schematics, S3 cabling, and signal-integrity notes live in
[`docs/`](./docs/).

## Absolute requirements (from product owner)

These are non-negotiable and apply to any future change:

- **Never downscale resolution.** GIFs are encoded at full source
  resolution (square crop from 2560×1920 → 1824×1920). Image quality
  is the #1 priority. If memory is tight, find another way (free
  buffers, process one frame at a time, use smaller intermediate
  structures) — never reduce output resolution.
- **Camera JPEG quality minimum 4.** Quality 2 produces non-standard
  Huffman tables that software decoders (tjpgd, esp_new_jpeg) cannot
  handle.
- **`.p4ms` preview stays at 240×240.** That's the ST7789 panel size;
  reducing it would make the gallery preview look pixelated.
- **SPI chunk size 4096 bytes**, master and slave must match. Mismatch
  causes data corruption.

## License

See [LICENSE](./LICENSE).

## Links

- [ESP32-P4X-EYE User Guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html)
- [ESP-IDF v5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32p4/get-started/index.html)
- [LVGL 8.3](https://docs.lvgl.io/8.3/)
- [CLAUDE.md](./CLAUDE.md) — full engineering reference
- [CLAUDE-MOCK.md](./CLAUDE-MOCK.md) — mock harness reference
