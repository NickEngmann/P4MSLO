# GIF Encoder Optimization Log

Session log of perf work on the PIMSLO encoder. Numbers are measured
on the actual P4MSLO device — ESP32-P4X v3.1, OV5640 cameras over
SPI, 1824×1920 stereoscopic 6-frame GIFs (4 forward + 2 replayed).

The original version of this doc covered the P4-EYE single-camera
1920×1080 4-frame GIF encoder. That codebase is gone; this one is
PIMSLO-specific. The "what worked" wins below carry over conceptually
but the numbers are different.

## Hardware

- **CPU**: ESP32-P4X v3.1, dual-core RISC-V @ 400 MHz
- **ISA**: `rv32imafc_zicsr_zifencei_xesploop_xespv` (no Zb)
- **RAM**: 32 MB PSRAM @ 200 MHz, ~287 KB internal HP L2MEM, **8 KB TCM at 0x30100000**
- **Target**: 1824×1920 stereoscopic GIF, 6 frames (4 forward + 2 replayed), Floyd-Steinberg dithering, 256-color palette

## Headline numbers (current build, 2026-04-25)

| Stage | Time |
|---|---|
| User-visible "saving" overlay | **~2-3 s** (SPI capture only) |
| Background SD save (4× ~500 KB) | ~6-12 s, invisible |
| `.p4ms` preview generation (4× tjpgd → 240×240) | ~7 s |
| GIF Pass 1 (palette build) | ~10-15 s |
| GIF Pass 2 (6 frames × ~24 s/frame) | ~140 s |
| **Total photo → playable GIF** | **~150-160 s (~2.5 min)** |
| **Photo cadence (back-to-back)** | **every ~3 s** (SPI-bound) |

10 back-to-back `spi_pimslo` captures verified **10/10 × 4/4 cameras**
at 100% on this build — see `tests/e2e/_spi_20shot.py`.

## What's working in the current encoder

### 1. TCM-resident LUT (commit `a206f6e`)

The Pass 2 hot loop's per-pixel palette lookup is the encoder's tightest
inner loop. We've tried three places for the lookup table:

| LUT placement | Per-lookup latency | Status |
|---|---|---|
| 64 KB RGB565 LUT in PSRAM | ~80 ns | Original — Pass 2 took ~270 s for a 6-frame GIF |
| 64 KB RGB565 LUT in HP L2MEM BSS | ~3 ns | **REJECTED** — starved DMA-internal pool, broke SPI master |
| **4 KB R4G4B4 LUT in TCM** | **~3 ns** | **CURRENT** — separate physical SRAM, doesn't compete with DMA |

R4G4B4 = 12-bit address (4 bits each of R/G/B). The bottom 4 LSBs of
each channel are dropped. Floyd-Steinberg's error propagation already
adds noise larger than 4 LSBs, so the visual difference is below the
encoder's noise floor.

TCM lookup is `((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)` — single
indirection, ~3 ns. **~25× faster than the PSRAM LUT.**

The 4 KB sizing is forced by `pmu_init.c` and friends already
consuming ~2.8 KB of the 8 KB TCM region. An 8 KB R4G5B4 attempt
overflowed `tcm_idram_seg` by 2816 bytes.

### 2. BSS-resident static stacks for encoder tasks (commits `d170f0e`, `1ac42a1`)

`pimslo_encode_queue_task` ("pimslo_gif", 16 KB) and `gif_bg` (16 KB)
both use `xTaskCreateStaticPinnedToCore` with stack arrays in
internal-RAM BSS. The shorter-call-chain tasks (`pimslo_save` 6 KB,
`pimslo_cap` 8 KB) keep PSRAM/internal stacks.

Why: per-pixel hot loop hits the stack constantly. PSRAM stack means
every push/local-read costs ~100-200 ns instead of an internal-RAM
cycle. Net effect on Pass 2 alone: ~12 s/frame (internal stack) →
~55 s/frame (PSRAM stack). Plus the heap-corruption risk —
the FreeRTOS canary doesn't reliably cover PSRAM stacks; a stack
overflow into adjacent PSRAM heap blocks corrupts free-list metadata
and panics later in `tlsf::remove_free_block`.

The internal-RAM headroom for these stacks comes from the TCM LUT
change above — moving the 64 KB LUT off internal RAM freed enough
space.

### 3. Shared `s_tjpgd_work[32768]` workspace (commit `f9fad72`)

`gif_encoder.c` and `app_gifs.c` used to each have their own 32 KB
tjpgd workspace BSS. They never run concurrently within an encode
pipeline (encoder uses it for Pass 1/Pass 2 decodes; app_gifs uses
it for `decode_jpeg_crop_to_canvas` in the .p4ms path).
Consolidated under a mutex (`s_tjpgd_mutex` in `app_gifs.c`):

- `app_gifs_acquire_tjpgd_work(timeout, &size)` — takes mutex, returns workspace pointer
- `app_gifs_release_tjpgd_work()` — releases mutex

Saves 32 KB internal BSS. That headroom is what lets pimslo_gif have
a static BSS stack.

### 4. `EXT_RAM_BSS_ATTR` for the 32 KB stdio fwrite buffer

`gif_encoder.c::file_buf[32768]` lives in PSRAM (via
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` + `EXT_RAM_BSS_ATTR`).
SD throughput is the bottleneck (~250 KB/s); PSRAM access on the
fwrite path is wildly faster than the bottleneck so the move is free.

Saves 32 KB internal BSS.

### 5. File-level `-O2` for pure-math files (`factory_demo/CMakeLists.txt`)

Global `COMPILER_OPTIMIZATION_DEBUG=y` (`-Og`) for stability, with
`-O2` opt-in via `set_source_files_properties` for:

- `main/app/Gif/gif_tjpgd.c` (JPEG decode)
- `main/app/Gif/gif_encoder.c` (LZW + palette)
- `main/app/Gif/gif_decoder.c` (LZW decode)
- `main/app/Gif/gif_lzw.c`
- `main/app/Gif/gif_quantize.c`
- `main/app/Spi/spi_camera.c`

These files are pure number-crunching with no LVGL / display
interaction. Per-frame tjpgd decode goes ~2.2 s → ~1.7 s (~25%
faster).

Whole-component `-O2` was tried (LVGL, esp_lvgl_port, esp_lcd,
esp_driver_spi/jpeg/isp/cam, the whole `main` component) — all of
them WDT-hung. Per-file targeting is the safe path.

### 6. Output-cell-driven JPEG downscale (commit `c9a4631`)

In `app_gifs.c::jpeg_crop_out_cb` (the .p4ms path), the original
implementation iterated every source pixel in each MCU and computed
which output cell it maps to — O(MCU_bw × MCU_bh) per block, ~9.5 K
ops per MCU at 16×16. Replaced with output-cell iteration: for each
canvas cell whose nearest source falls inside this MCU and inside
the crop rect, pluck that one source pixel — O(canvas_cells_hit)
≈ ~160 ops per MCU on a 1824 → 240 downscale. **~58× fewer
iterations per MCU.**

Watch out: the inverse mapping has an off-by-one trap. The forward
map is `cropped_x = floor(out_x * cw / ow)`, so the inverse upper
bound must be `out_x_hi = ((rel_right + 1) * ow - 1) / cw`, NOT
`floor(rel_right * ow / cw)`. The wrong formula loses one canvas
column at every MCU boundary, rendering as **vertical dark bars on
.p4ms previews**.

### 7. SPI camera reliability (commits `8bb11b7`, `c835c28`, `eaadcbd`)

The SPI master had two reliability problems:

- **`setup_dma_priv_buffer(1206): Failed to allocate priv RX buffer`**
  panic mid-capture, usually on camera 4 after 3 successful
  captures. Cause: ESP-IDF's SPI master triggers a per-transaction
  priv-buffer alloc whenever the source/dest buffer is not aligned
  to `cache_align_int` (64 bytes on ESP32-P4). `heap_caps_malloc(
  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)` returns 4-byte aligned
  blocks, which fails the 64-byte check; priv buf allocated per xfer
  → fragmentation → NULL deref → panic.

  Fixes:
  1. Permanent 64-byte aligned `s_scratch_tx[16]` / `s_scratch_rx[16]`
     for status/control transactions. `spi_xfer_cam` memcpys through
     the scratch for any ≤16-byte transaction.
  2. Permanent 64-byte aligned `s_chunk_rx[4096]` for chunked-RX,
     claimed eagerly at `spi_camera_init` while DMA pool is fresh.
  3. ISR-driven trigger on GPIO34 with a CS re-queue gap.

- **DMA pool starvation under concurrent capture+encode** (commit
  `1ac42a1`). The default `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`
  gave a 32 KB reserved pool for DMA-only allocations. Under
  concurrent capture + encode + SD writes, fragmentation took the
  largest free DMA block down to ~6-14 KB, which broke priv-buffer
  alloc. Bumped to **64 KB**; now stays at ~30 KB largest under load.

Result on `_spi_20shot.py 10`: **10/10 × 4/4 cameras**, zero priv_buf
failures, zero panics.

## What didn't work / things to avoid

### 64 KB LUT in HP L2MEM BSS

`s_pixel_lut[65536]` as static BSS got the per-pixel speedup (~25×
faster than PSRAM) but the 64 KB consumed enough of HP L2MEM that
`dma_int largest` dropped to ~1.6 KB. The SPI master's per-tx
priv-buffer alloc then started failing → mid-capture panic. Reverted.

The mock simulator (`p4_budget.c`) now mirrors INTERNAL BSS deductions
≥32 KB into the dma_int pool to catch this kind of architecture
regression at simulation time. See the
`proposed_bss_lut_starves_dma_int` scenario in `test_e2e.c`.

### `CONFIG_HEAP_POISONING_COMPREHENSIVE=y`

Boot hung at LVGL init. The canary overhead was too high for the
L2MEM headroom on this board. `CONFIG_HEAP_POISONING_LIGHT=y` boots
fine and stays as defense-in-depth (~12 B per allocation).

### Whole-component `-O2`

Tried on LVGL, esp_lvgl_port, esp_lcd, esp_driver_*, `main` — all
WDT-hung. Per-file `-O2` for pure-math files works, whole-component
doesn't. (Specific symptom: under global `-O2`, LVGL's `lv_refr`
busy-waits forever on `draw_buf->flushing` at `lv_refr.c:709`.)

### Encoder error buffers in INTERNAL with PSRAM fallback

`err_cur` / `err_nxt` (11.5 KB each, allocated per Pass 2 frame for
Floyd-Steinberg) used to try `MALLOC_CAP_INTERNAL` first with PSRAM
fallback. The 23 KB pair × 12 alloc cycles per encode (4 + 2 frames
× 2 buffers) was the binding source of dma_int fragmentation under
concurrent capture+encode.

Manifested as either `setup_dma_priv_buffer(1206)` SPI master panics
OR `sdmmc_write_sectors: not enough mem (0x101)` failures during the
save task's SD writes. Forced to PSRAM unconditionally as of commit
`1ac42a1`. Costs ~75 ns/access on the dither hot loop ≈ 1.5 s/frame
≈ 10 s per 6-frame encode (so encode total went from ~95 s to
~140 s). Acceptable trade vs OOM panics.

### PSRAM-resident task stacks for the encoder hot path

`pimslo_encode_queue_task` and `gif_bg` falling back to PSRAM (via
`xTaskCreatePinnedToCore`'s silent fallback when internal can't
satisfy the request) made the encoder ~5-7× slower AND introduced
silent stack overflows that the FreeRTOS canary doesn't catch (see
research notes from Espressif: forum t=22793). Static BSS internal
stacks are the only stable option.

## Memory layout pinned by these wins

The current memory map is the result of trading off all of the above:

| Region | Reserved for | Notes |
|---|---|---|
| TCM (8 KB) | 4 KB R4G4B4 LUT + ~2.8 KB pmu_init.c statics | Single-cycle access, separate from DMA |
| HP L2MEM internal BSS | 32 KB shared tjpgd_work + 16 KB pimslo_gif stack + 16 KB gif_bg stack + 700 B static TCBs | All other BSS as needed |
| HP L2MEM regular heap | task TCBs, scratch, drivers | ~73 KB free, ~32 KB largest post-boot |
| HP L2MEM DMA-reserved | SPI master priv-buffers, SDMMC DMA descriptors | 64 KB reserved at boot via `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`, ~36 KB largest post-boot |
| PSRAM | encoder scaled_buf (7 MB), album PPA (6 MB), file_buf (32 KB EXT_RAM_BSS), err_cur/err_nxt (23 KB per encode), pimslo_save stack (6 KB), frame caches | ~17 MB free post-boot, 8.4 MB largest contig |

## Things to consider for the next round

1. **Streaming GIF playback decoder** — already done in commit
   `e0dea27`. Peak playback memory dropped 6.7 MB → 3.5 MB.

2. **Parallel decode/encode pipeline** — decode frame N+1 on a
   worker task while encoding frame N. Saves ~1.7 s/frame. Requires
   double-buffered scaled_buf (2× 7 MB = 14 MB PSRAM). Estimated
   improvement: ~10 s per encode (~7%). Probably not worth the
   complexity.

3. **DMA2D for cache row prefetch** — `enc->scaled_buf` is in PSRAM;
   `row_cache[1920]` is internal. Currently `memcpy` per row. The
   ESP32-P4 has a DMA2D engine that could do this async, overlapping
   with the previous row's dither work. Estimated saving: ~50 ms /
   frame (1920 × 2 bytes ÷ PSRAM bandwidth). Negligible.

4. **Move the encoder error buffers back to internal somehow** —
   ~10 s/encode regression from the PSRAM placement. Would need to
   either (a) reduce internal-BSS footprint somewhere else first, or
   (b) keep the error buffers internal but allocate them ONCE per
   encoder session (not per frame) so the per-frame churn is
   eliminated.

5. **Ordered (Bayer) dithering** — fully parallelizable (no pixel
   dependencies), SIMD-friendly, but lower visual quality. The
   encoder's current Floyd-Steinberg quality is part of the product;
   probably don't go here.

6. **Frame delta encoding** — GIF supports encoding only changed
   pixels between frames. For a stereoscopic PIMSLO, frames 1→4 are
   parallax-shifted versions of similar source content; the LZW work
   could probably be reduced for frames 5/6 (the replays). Worth
   investigating if encoder timing becomes user-painful again.

## Cross-references

- [CLAUDE.md § Pipeline Timing](../CLAUDE.md#pipeline-timing-ov5640-4-cameras-working) — the canonical timing table
- [CLAUDE.md § Known Issues](../CLAUDE.md#known-issues) — full diagnosis log for memory / SPI bugs
- [CLAUDE-MOCK.md](../CLAUDE-MOCK.md) — host mock harness (validates architecture decisions in <1 s before flashing)
- [tests/README.md](../tests/README.md) — on-device e2e tests
