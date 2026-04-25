# CLAUDE-MOCK.md — Host-side architecture validation

The on-device PIMSLO pipeline lives across several files, has tight
internal RAM constraints, and runs a 5-7 minute encode cycle that
makes hardware iteration painful. This doc covers the **host mock
harness** that lets us validate architecture decisions in <1 second
without flashing.

The harness lives in `test/mocks/` (the simulator library) and
`test/host_encode/` (the test runners + main scripts).

## TL;DR — running it

```bash
test/host_encode/run.sh           # build + run all 5 host tests
test/host_encode/run.sh --rebuild # force a clean build first
```

Expected output ends with `HOST SUITE: ALL PASS` (~2 seconds).

## The five test runners

| Runner | What it validates |
|--------|---|
| `host_encode` | The actual `gif_encoder.c` + `gif_lzw.c` + `gif_quantize.c` + `gif_tjpgd.c` source files compiled against host stubs, driven against the 4 fixture JPEGs in `debug_gifs/`. Compiled with `-fsanitize=address,undefined` so heap-corruption / use-after-free / OOB write bugs in the encoder fire here too. 50× back-to-back stress run. **Validates: encoder code is memory-clean.** |
| `test_budget` | Every known sized allocation on the device (LCD, LVGL, SPI camera, all task stacks, encoder buffers, viewfinder, album PPA, etc.) has an entry in the `P4_BUDGET_BASELINE` catalog. The simulator runs them through the constrained allocator and reports which fall back to PSRAM and which hard-fail. Compares BASELINE vs PROPOSED architecture catalogs side-by-side. **Validates: the proposed memory layout is feasible.** |
| `test_phases` | Walks through the full PIMSLO photo→encode lifecycle as a sequence of alloc/free events: boot → capture → save → free viewfinder → encoder_create → pass1×4 → pass2×4 → encoder_destroy → realloc viewfinder. Catches order-dependent bugs ("forgot to free viewfinder before allocating scaled_buf"). **Validates: lifecycle ordering is correct.** |
| `test_timing` | Predicts end-to-end encode duration for both stack regimes (INTERNAL vs PSRAM) using measured per-frame numbers from the device. Confirms PROPOSED arch hits the < 2 min target while BASELINE doesn't. **Validates: the timing fix actually fixes the timing.** |
| `test_e2e` | Ten user-visible scenarios mirroring the on-device e2e tests: photo from MAIN, photo from CAMERA + nav, multiple photos, too-few-cams drop, bg worker recovery, gallery yield, memory steady state, etc. **Validates: user-facing flows work end-to-end.** |

## The simulator library

### `p4_mem_model.{h,c}` — constrained allocator

`heap_caps_*` equivalents that fail when an allocation exceeds the
pool's largest contiguous block, mirroring the on-device FreeRTOS /
TLSF behavior. PSRAM is modeled with up to 4 free blocks because the
chip has multiple banks (a 7 MB alloc doesn't preclude a parallel
6 MB alloc); INTERNAL pools just track a single largest_contiguous.

Two preset memory models:

  - `P4_MEM_MODEL_DEFAULT` — post-boot snapshot. `dma_int largest=6400`,
    `int largest=7168`, `psram largest=8650752` plus a 6 MB second PSRAM
    block. Numbers are direct reads from the `heap_caps` serial cmd on
    the live board. Use this when simulating runtime behavior of the
    AS-IS firmware.

  - `P4_MEM_MODEL_RAW` — pre-BSS, pre-ESP-IDF state. Use when
    experimenting with "what if I add or remove N KB of BSS?": start
    RAW, run the catalog (BSS items drain the pools first), then
    runtime allocs see the resulting headroom.

The allocator tracks:
  - active alloc count + bytes per pool
  - alloc fail count per pool (binding-constraint failures)
  - per-pool `total_free` and `largest_contiguous` after each event

### `p4_budget.{h,c}` — component catalog + simulator

Two catalogs:

  - **`P4_BUDGET_BASELINE`** — current firmware on `fix/pimslo-encode-stuck`.
    Three 32 KB BSS hogs in internal RAM (`s_tjpgd_work`, `tjwork.1`,
    `file_buf.0`); pimslo_gif task's 16 KB stack falls back to PSRAM
    because the largest free internal block is ~7 KB.

  - **`P4_BUDGET_PROPOSED`** — the surgical fix:
    1. **Drop `gif_encoder.c::tjwork`** (32 KB), share `s_tjpgd_work`
       via mutex (never used concurrently within an encode pipeline).
    2. **Move `gif_encoder.c::file_buf` to PSRAM** via
       `EXT_RAM_BSS_ATTR` (it's a stdio fwrite buffer; SD throughput
       is the bottleneck, not PSRAM access).
    3. **Add 16 KB BSS-resident static stack** for pimslo_gif via
       `xTaskCreateStaticPinnedToCore` (replaces the heap-alloc that
       always lands in PSRAM).
    Net BSS delta: -32 -32 +16 = -48 KB internal BSS freed.

`p4_budget_simulate(catalog, n, mode, report)` runs each entry against
the current memory model and reports which pool each ended up in. The
two modes:
  - `P4_BUDGET_MODE_AS_IS` — BSS items are markers (already baked into
    `DEFAULT` model state).
  - `P4_BUDGET_MODE_FROM_RAW` — BSS items deduct from pool size; lets
    us experiment with adding/dropping BSS.

`test_budget` runs three scenarios (BASELINE/AS_IS, BASELINE/RAW,
PROPOSED/RAW) and prints the comparison. The PROPOSED column shows
**5 fewer PSRAM-fallback events** than BASELINE; specifically,
pimslo_gif's stack stays in INTERNAL.

### `p4_phases.{h,c}` — phased lifecycle simulator

Walks through alloc/free/check events in order. Catches
order-dependent bugs that the static budget can't see — e.g. forgot
to `app_video_stream_free_buffers()` before `gif_encoder_create()`
allocates its 7 MB scaled_buf.

`P4_PHASES_PIMSLO_FLOW` is the pre-baked event sequence for a
photo→encode cycle:

```
PHASE  boot — permanents
ALLOC  LVGL canvas
ALLOC  LVGL DMA staging
ALLOC  SPI chunk_rx, scratch_tx, scratch_rx
ALLOC  viewfinder buf (7 MB)
ALLOC  album PPA buf  (6 MB)

PHASE  photo_btn fired — capture
PHASE  save — pos*.jpg fwrite (4× 500 KB transient)

PHASE  encode pipeline begins
FREE   viewfinder buf
FREE   album PPA buf

PHASE  encoder_create
ALLOC  encoder scaled_buf (7 MB)
ALLOC  encoder pixel_lut  (64 KB)

PHASE  encode_pass1 — palette accumulation, 4 cams
ALLOC/FREE pass1 jpeg buf 1..4

PHASE  encode_pass2 — frame encode
ALLOC  pass2 frame N jpeg
ALLOC  pass2 err_cur, err_nxt, row_cache
FREE   ... in reverse

PHASE  encoder_destroy
FREE   encoder pixel_lut, scaled_buf

PHASE  post-encode — viewfinder + album realloc
ALLOC  viewfinder buf
ALLOC  album PPA buf

PHASE  idle — system steady state
CHECK  dma_int largest ≥ 1024
CHECK  psram   largest ≥ 8 MB  (this fails — known PSRAM fragmentation)
```

The test_phases output shows every alloc with its outcome (which pool
it landed in, how the largest contiguous block changed). This is the
diagnostic you read when an architectural change makes a runtime
allocation fail unexpectedly.

### `p4_timing.{h,c}` — encode-duration model

Estimates pipeline duration based on:
  - number of cameras returning usable JPEGs (2-4)
  - encoder task stack location (INTERNAL vs PSRAM)
  - whether .p4ms direct-JPEG save runs

Numbers from on-device measurement:
  - INTERNAL stack (legacy `pimslo` serial cmd path):
    decode ~1.7 s, encode ~10 s, total ~12 s/frame
  - PSRAM stack (photo_btn flow on the AS-IS firmware):
    decode ~18 s, encode ~37 s, total ~55 s/frame

Model uses a single `P4_TIMING_STACK_PENALTY_PSRAM_PCT = 460%` factor
applied to all stack-bound work. Predicts:
  - INTERNAL stack 4-cam encode: **~80 s** (under 2 min target ✓)
  - PSRAM stack 4-cam encode: **~295 s** (over by 2.5 min ✗)

Matches the device's 82 s observed via legacy `pimslo` cmd vs 5-7 min
observed via photo_btn flow (`pimslo_encode_queue_task` with PSRAM stack).

### `pimslo_sim.{h,c}` — end-to-end pipeline simulator

Glues the constrained allocator + budget catalog + phase simulator +
timing model into a unified API that mirrors the on-device PIMSLO
subsystem structure.

**Mirrored device state:**
  - `ui_extra_get_current_page()` / `ui_extra_set_current_page()` for
    page transitions (UI_PAGE_MAIN / CAMERA / GIFS / SETTINGS / ...)
  - Gallery state: entries (with stems, .gif/.jpeg/.p4ms presence),
    current_index, is_encoding, is_playing, gallery_ever_opened
  - `encode_should_defer()` mirrors `app_pimslo.c::encode_should_defer`
    (CAMERA/INTERVAL/VIDEO/encoding ⇒ defer)
  - Capture counter assigning P4Mxxxx stems
  - Encode queue (capacity 8)
  - Background worker (.p4ms pre-render + JPEG-only re-encode)

**Operations:**
  - `pimslo_sim_photo_btn(n_cams)` — simulates a button press; runs
    capture (1700 ms × n_cams) + save (~1700 ms × n_cams) and queues
    the encode.
  - `pimslo_sim_wait_idle(max_ms)` — drains the encode queue. If
    `encode_should_defer()` returns true (user on a camera page), the
    queue waits.
  - `bg_worker_kick()` — runs one bg_worker pass (pre-renders missing
    .p4ms, re-encodes JPEG-only orphans).

**Architecture switch:**
  - `pimslo_sim_set_architecture(PIMSLO_ARCH_BASELINE)` — encoder
    stack lives in PSRAM (current firmware behavior).
  - `pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED)` — encoder
    stack lives in INTERNAL via static BSS (the fix).

The timing model's stack penalty fires off this switch, so flipping
between BASELINE and PROPOSED demonstrates the speedup directly.

## How to use it for architecture work

The promise: **don't iterate on hardware**. Cycle is:

  1. Have an architecture idea ("what if we move file_buf to PSRAM?").
  2. Add it to `P4_BUDGET_PROPOSED` in `p4_budget.c`.
  3. `cd test/host_encode/build && make && ./test_budget`. Read the
     output: did the allocations fall where you expected? Did
     pimslo_gif still avoid the PSRAM fallback?
  4. If timing matters, run `./test_timing` and check the predicted
     total.
  5. If lifecycle ordering matters, add events to
     `P4_PHASES_PIMSLO_FLOW` and run `./test_phases`.
  6. If you want to validate a user-visible flow, add a scenario to
     `test_e2e.c`.
  7. Only when all four runners pass: apply the change to the
     firmware and flash. Run `tests/e2e/run_fast.sh`.

If the hardware test fails, iterate on the simulator first — repro the
failure as a new test scenario, fix it under the simulator, then
re-flash. Each simulator iteration is < 1 second; each hardware
iteration is 5+ minutes.

## What the simulator does NOT model

  - **Real concurrency / FreeRTOS scheduling.** The simulator runs
    everything on a single virtual thread; encode "runs" by advancing
    a virtual clock by the timing model's prediction. Race-condition
    bugs (e.g. two tasks fighting for the tjpgd mutex with bad timing)
    won't reproduce here. Layer pthreads + the timing model on top if
    you need this.

  - **ESP-IDF JPEG HW decoder internals.** The on-device "Store
    Access Fault inside `tlsf_control_functions.h:374`" panic
    originates inside the HW decoder calloc — the encoder pipeline
    itself is clean (50× ASan stress shows no diagnostics). If you
    chase that panic, the simulator can show you that the encoder
    side is innocent but it can't reproduce the panic itself.

  - **Real LVGL rendering.** `bsp_display_lock`, canvas mutations,
    actual ST7789 SPI flushes — none of that runs. The simulator
    tracks the canvas BUFFER as an allocation but doesn't pump frames
    through it. For UI-rendering bugs use `test/simulator/` (the
    LVGL+SDL2 simulator).

  - **Real SD I/O timing.** `fwrite` on host is fast; the simulator
    uses a 250 KB/s rate model in `pimslo_sim.c::pimslo_sim_photo_btn`
    that reflects the on-device throughput.

  - **The S3 cameras.** SPI capture is a single virtual operation
    that returns N JPEGs (where N is the parameter). `force_capture_fail()`
    lets you simulate the partial-failure case.

## File layout

```
test/
├── mocks/
│   ├── p4_mem_model.{h,c}       constrained allocator
│   ├── p4_budget.{h,c}          BASELINE + PROPOSED catalogs
│   ├── p4_phases.{h,c}          phased lifecycle simulator
│   ├── p4_timing.{h,c}          encode-duration model
│   ├── pimslo_sim.{h,c}         unified sim (page state, gallery,
│   │                            tasks, bg worker)
│   ├── esp_heap_caps.h          libc-backed heap_caps_* shims
│   ├── esp_log.h                printf-backed ESP_LOG* + clock-backed
│   │                            esp_log_timestamp
│   ├── esp_err.h                ESP_OK / ESP_FAIL / ESP_ERR_*
│   ├── esp_private/esp_cache_private.h    stub
│   ├── driver/jpeg_decode.h     stubs (HW decoder is unused)
│   ├── driver/ppa.h             stubs (PPA is unused on encode path)
│   └── ... (existing mocks for the other host tests)
└── host_encode/
    ├── CMakeLists.txt
    ├── run.sh                   build + run-all driver
    ├── host_encode_main.c       runs gif_encoder against fixture JPEGs
    ├── gif_simd_stub.c          host stub for gif_simd.S
    ├── test_budget.c            runs the budget catalog scenarios
    ├── test_phases.c            runs the phased lifecycle
    ├── test_timing.c            prints predicted encode duration
    └── test_e2e.c               10 end-to-end scenarios
```

## Updating the catalog when firmware changes

If you change a buffer size, allocation pool, or task stack size in
the firmware, also update the matching entry in
`test/mocks/p4_budget.c`. The catalogs are the single source of
truth for the simulator — wrong numbers there mean the predictions
are wrong. Verify against `factory_demo/build/factory_demo.map` for
BSS placements and against `heap_caps` serial cmd output for the
runtime model.

## What landed on hardware (commit f9fad72)

The PROPOSED architecture from the simulator was applied to the
firmware in three surgical changes, fully validated on host before
flashing:

  1. `gif_encoder.c::tjwork` dropped — shares `s_tjpgd_work` from
     app_gifs.c via new `app_gifs_acquire_tjpgd_work()` API. Mutex
     gates the three call sites.
  2. `gif_encoder.c::file_buf` moved to PSRAM via `EXT_RAM_BSS_ATTR`.
     Required `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` in
     sdkconfig.defaults.
  3. `pimslo_encode_queue_task` and `gif_bg` use `xTaskCreateStatic`
     with 16 KB BSS stacks each.

### Measured on-device deltas (heap_caps post-boot):

| pool       | before  | after   | delta   |
|------------|---------|---------|---------|
| dma_int free | 13191 | 36535 | +22 KB  |
| dma_int largest | 6400 | 23552 | +17 KB |
| int free   | 25527 | 73455 | +48 KB |
| int largest | 7168  | 31744 | +24 KB |

Matches the simulator's prediction.

### Encoder timing impact:

| step                    | before          | after          |
|-------------------------|-----------------|----------------|
| .p4ms cam decode (×4)   | 12-40 s each   | 1.3-2.4 s each |
| Pass 2 decode (×4)      | 18 s/frame     | 1.5 s/frame    |
| Pass 2 LZW+dither (×4)  | 55 s/frame     | 55 s/frame ⚠   |
| Total estimate          | 5-7 min        | ~4.5 min       |

The Pass 2 LZW hot loop is still PSRAM-bound — the 64 KB `pixel_lut`
doesn't fit the new 31 KB largest-internal block, so per-pixel LUT
reads still cost ~100-200 ns each. That's the next bottleneck and
the next surgical target. Options being considered:

  - **Octree LUT (~8 KB)**: hierarchical color-quantization table.
    Slower per-lookup (3-4 indirections vs 1) but small enough for
    internal RAM. Net win because PSRAM latency dominates.
  - **RGB444 LUT (4 KB)**: drop 1-2 bits per channel. Smallest LUT
    that still works. Quality impact: probably acceptable since the
    encoder dithers, but needs A/B comparison.
  - **64 KB static BSS LUT**: drop one of the static stacks (gif_bg
    back to heap-alloc) to make room. Simplest if quality loss is
    unacceptable.

### Hardware test status

`run_fast.sh` and `run_all.sh` need 4/4 SPI captures to exercise the
photo→encode flow. The S3 cameras on the test rig are currently
stuck at status 0x00 (unresponsive to `cam_reboot`); per CLAUDE.md
"SPI slave stuck in DATA mode" this requires a physical USB
power-cycle of the S3 boards to recover. Architecture validation
ran via the legacy `pimslo` serial cmd (encodes from existing SD
fixtures) which doesn't need cameras.

## What landed in commit 79c09f9 — BSS pixel_lut

The simulator's PROPOSED_BSS_LUT path was applied. Headline result
on hardware via legacy `pimslo` serial cmd:

| step                    | f9fad72 (LUT in PSRAM) | 79c09f9 (LUT in BSS) |
|-------------------------|------------------------|----------------------|
| Pass 2 frame timing     | decode=1571 encode=55328 ms | decode=1605 encode=2030 ms |
| Pass 2 LZW speedup      | —                      | **27×**              |
| Total 4-cam encode      | ~270 s (4.5 min)       | **36.7 s**           |
| vs 2-min target         | ✗                      | ✓                    |

The 64 KB pixel_lut is now a static BSS array (`s_pixel_lut`). The
per-pixel LUT read in the dither+LZW hot loop hits internal SRAM
instead of PSRAM, which on this chip is ~5-7× faster cold + cache-warm.

Trade-offs documented in the commit:

  - dma_int largest dropped 23552 → 6400 (back to baseline-low). Tight
    for LCD priv-TX scratch but functional.
  - int largest dropped 31744 → 15360. The legacy `pimslo` /
    `spi_pimslo` serial-cmd encode task (`pimslo_enc`) and the bg
    worker (`gif_bg`) both used to be heap-alloc'd 16 KB stacks; that
    no longer fits internal so they had to switch to
    `xTaskCreatePinnedToCoreWithCaps(MALLOC_CAP_SPIRAM)`. Both now run
    on PSRAM stack (slower) — acceptable since they're test/background
    paths, not user-facing.
  - Foreground photo_btn flow (`pimslo_encode_queue_task`) keeps its
    static BSS stack AND benefits from the new internal LUT.

Mock simulator delta: PROPOSED_BSS_LUT scenario in `test_e2e` predicted
80 s total; hardware measured 36.7 s. The simulator's penalty model
was conservative on the LUT side (5.5× PSRAM penalty applied to the
nominal 10 s/frame); reality is closer to 27× speedup which the model
underestimated. That's a useful calibration data point — the next
catalog update should bump LUT_PENALTY_PSRAM_PCT closer to ~2700.

## Reverted in commit 72e06bd — BSS LUT broke SPI

Hardware showed the BSS LUT in HP L2MEM collapsed `dma_int largest`
from 6.4 KB to 1.6 KB and the SPI master priv-RX alloc panicked
mid-capture. Reverted. The simulator was updated to mirror INTERNAL
BSS deductions into the dma_int pool when items exceed 32 KB, so
the PROPOSED_BSS_LUT scenario now correctly hard-fails the budget
simulator before any flash. The 32 KB threshold matches empirical
observation: 8bb11b7 (the 100% SPI baseline) has three 32 KB BSS
items in the same region without breaking SPI, but a single 64 KB
item flips it.

## TCM-octree path — RECOMMENDED next step (mock-validated)

The encoder's per-pixel LUT lookup needs internal-RAM speed but
shouldn't compete with SPI's DMA pool. Solution: put an 8 KB
**octree LUT in TCM** (the Tightly-Coupled Memory at 0x30100000).
TCM is:
  - Separate from HP L2MEM (where the DMA pool lives)
  - 8 KB total — exactly fits an octree-quantized 256-color palette LUT
  - Closer to the core than HP L2MEM (1-2 cycle load latency)
  - Pre-cleared at boot, so functionally a normal BSS region

The simulator now models TCM as `P4_POOL_TCM`. Octree LUT lookups
take 3-4 indirections per pixel (vs 1 for the flat 64 KB LUT) but
all in cache-warm TCM, so the per-pixel cost is roughly 1.8× the
flat-internal nominal — still 15× faster than PSRAM-resident.

### Predicted timings (test_timing output)

| arch                                           | total | budget |
|------------------------------------------------|-------|--------|
| BASELINE (PSRAM stack + PSRAM LUT)             | 331 s | ✗      |
| PROPOSED (commit 72e06bd, current shipping)    | 260 s | ✗      |
| PROPOSED + RGB444 LUT (4 KB internal, lossy)   |  50 s | ✓      |
| **PROPOSED + OCTREE LUT in TCM (8 KB)**        | **54 s** | **✓** |
| PROPOSED + OCTREE LUT in HP L2MEM (rejected)   |  56 s | ✓ but SPI risk |
| PROPOSED + 64 KB BSS LUT (rejected)            |  48 s | ✓ but SPI broken |

### dma_int budget under PROPOSED_OCTREE_TCM

| catalog                            | dma_int largest |
|------------------------------------|-----------------|
| BASELINE / RAW                     |  ~32 KB         |
| PROPOSED with BSS_LUT (rejected)   |   1.6 KB ✗      |
| **PROPOSED with octree-in-TCM**    | **27.6 KB ✓**   |

The budget simulator scenario `proposed_octree_tcm_passes_dma_int_budget`
asserts dma_int stays ≥ 5 KB (LCD priv-TX threshold). Currently passes.

### What the firmware change looks like

When ready to ship (after this mock prototyping):

1. In `gif_encoder.c`:
   - Drop the `heap_caps_malloc(65536, MALLOC_CAP_SPIRAM)` for `pixel_lut`.
   - Replace with a static octree built into a
     `__attribute__((section(".tcm.bss")))` array, ~8 KB.
   - Replace per-pixel `idx = lut[d16]` with
     `idx = octree_lookup(octree, r, g, b)` — 3-4 indirections.
2. In `gif_quantize.c`:
   - New `gif_quantize_build_octree(palette, octree)` that fills the
     TCM array from the 256-color palette.
3. No linker script change needed — `tcm_idram_seg` is already
   configured at `0x30100000 / 0x2000` per `factory_demo.map`.

The octree algorithm (iterate RGB888 high bits, descend tree by
branch index, leaves point to palette index) is well-known.
Implementation effort: ~2-3 hours for the forward port + a unit
test against the host fixtures in `debug_gifs/`.

After flash:
- `run_fast.sh` should pass (SPI healthy because dma_int intact)
- Encode timing should land near 54 s on the legacy `pimslo` path
- photo_btn flow's internal-stack `pimslo_encode_queue_task` should
  hit the same number once cameras are 4/4 reliable
