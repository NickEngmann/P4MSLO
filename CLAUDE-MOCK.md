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
| `test_timing` | Predicts end-to-end encode duration for the LUT-placement variants (PSRAM, INTERNAL BSS, RGB444, OCTREE_TCM, OCTREE_HPRAM) using measured per-frame numbers from the device. Confirms which architecture hits the < 2 min target. **Validates: the timing fix actually fixes the timing.** |
| `test_e2e` | **31 user-visible scenarios** mirroring on-device e2e tests: photo from MAIN, photo from CAMERA + nav, multiple photos, too-few-cams drop, bg worker recovery, gallery yield, memory steady state, foreground vs bg encode, app_album release/reacquire JPEG-decoder dance (4 scenarios), gallery delete-modal flows, p4ms persistence across page transitions, long-sequence heap stability (30-cycle stress), encoder timing per LUT variant. **Validates: user-facing flows work end-to-end.** |

## The simulator library

### `p4_mem_model.{h,c}` — constrained allocator

`heap_caps_*` equivalents that fail when an allocation exceeds the
pool's largest contiguous block, mirroring the on-device FreeRTOS /
TLSF behavior. PSRAM is modeled with up to 4 free blocks because the
chip has multiple banks (a 7 MB alloc doesn't preclude a parallel
6 MB alloc); INTERNAL pools just track a single largest_contiguous.

Two preset memory models:

  - `P4_MEM_MODEL_DEFAULT` — post-boot snapshot. **Updated 2026-04-25
    after `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` bumped 32 KB → 64 KB:**
    `dma_int largest=36864` (was 6400), `int largest=31732` (was 7168),
    `psram largest=8388608` plus a 6 MB second PSRAM block. TCM is
    4 KB usable (8 KB total - 4 KB consumed by the encoder's R4G4B4
    LUT). Numbers are direct reads from the `heap_caps` serial cmd
    on the live board. Use this when simulating runtime behavior of
    the current firmware.

  - `P4_MEM_MODEL_RAW` — pre-BSS, pre-ESP-IDF state. Use when
    experimenting with "what if I add or remove N KB of BSS?": start
    RAW, run the catalog (BSS items drain the pools first), then
    runtime allocs see the resulting headroom. Also updated for the
    64 KB DMA reservation.

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

## TCM-LUT path — SHIPPED (commit `a206f6e`, 2026-04-25)

The encoder's per-pixel LUT lookup needs internal-RAM speed but
shouldn't compete with SPI's DMA pool. Solution: put a small LUT in
**TCM** (the Tightly-Coupled Memory at 0x30100000). TCM is:
  - Separate from HP L2MEM (where the DMA pool lives)
  - 8 KB total — but ~2.8 KB consumed by `pmu_init.c` and friends
  - Closer to the core than HP L2MEM (single-cycle access)
  - Pre-cleared at boot, so functionally a normal BSS region

**Final implementation: 4 KB R4G4B4 LUT** (12-bit address, 4 bits each
of R/G/B). The original "8 KB octree" plan in this section was
prototyped against an 8 KB R4G5B4 direct LUT, but that overflowed
`tcm_idram_seg` by 2816 bytes at link time once the pmu_init.c TCM
usage was accounted for. 4 KB R4G4B4 fits with 1280 B headroom.

Mock simulator predicted ~54 s for an 8 KB octree-TCM path; hardware
delivered ~140 s with the 4 KB direct-TCM LUT. The gap is:
- Mock used a single `LUT_PENALTY` scalar; reality has multiple
  cumulative penalties on the dither hot loop (PSRAM err_cur/err_nxt,
  scaled_buf reads, etc.) that the timing model didn't separate.
- The 4 KB R4G4B4 lookup is ~25× faster than the PSRAM 64 KB LUT,
  same as the mock predicted; the encoder Pass 2 work is no longer
  LUT-bound.

The mock budget catalog still has the OCTREE_TCM and OCTREE_HPRAM
entries for what-if comparisons. The actual shipping LUT placement
is `P4_LUT_OCTREE_TCM` in the timing model (the model name is
historical — the implementation is a direct R4G4B4 lookup, not an
octree, but in TCM at the modeled size).

### What the firmware change actually looked like

In `factory_demo/main/app/Gif/gif_encoder.c`:
```c
TCM_DRAM_ATTR static uint8_t s_pixel_lut_tcm[4096] __attribute__((aligned(64)));

esp_err_t gif_encoder_pass1_finalize(gif_encoder_t *enc) {
    // ... build palette ...
    enc->pixel_lut = s_pixel_lut_tcm;          // no heap alloc — TCM static
    gif_quantize_build_lut12(&enc->palette, enc->pixel_lut);
    return ESP_OK;
}
```

And in the Pass 2 hot loop:
```c
uint16_t d12 = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
uint8_t idx = lut[d12];   // single TCM indirection, ~3 ns
```

`gif_quantize.c::gif_quantize_build_lut12()` iterates the 4096
buckets, expands each R4G4B4 to RGB888 with bit replication, finds
the nearest palette entry, and writes the index. ~40 ms to build.

### dma_int budget on hardware (verified)

| state                                | dma_int largest |
|--------------------------------------|-----------------|
| Pre-fix (LUT in PSRAM)               | ~6.4 KB         |
| Tried: 64 KB BSS LUT in HP L2MEM     | ~1.6 KB → SPI panic |
| **Shipped: 4 KB R4G4B4 in TCM + 64 KB DMA reserve** | **~36 KB ✓** |

The mock scenario `proposed_octree_tcm_passes_dma_int_budget` still
runs as a regression check.

### Hardware verification

10× back-to-back `spi_pimslo` captures: **10/10 × 4/4 cameras**, zero
priv_buf failures, zero panics. Encoder Pass 2 ~22 s/frame instead
of ~270 s/frame from the original PSRAM-LUT pre-fix state.

See [CLAUDE.md § Pipeline Timing](./CLAUDE.md#pipeline-timing-ov5640-4-cameras-working)
and [docs/OPTIMIZATIONS.md](./docs/OPTIMIZATIONS.md) for the full
canonical numbers.

## Subsequent stability fixes (commits `d170f0e`, `1ac42a1`)

Beyond the TCM LUT, two more fixes landed to address heap-corruption
panics that surfaced under heavy concurrent load:

### `gif_bg` static BSS stack (commit `d170f0e`)

Was `xTaskCreatePinnedToCoreWithCaps(MALLOC_CAP_SPIRAM)` with a 16 KB
PSRAM stack. The encoder hot path through gif_bg overflowed the stack
into adjacent PSRAM heap blocks, corrupting free-block prev/next
pointers. FreeRTOS canary doesn't reliably cover PSRAM stacks on this
chip. Symptom: `tlsf::remove_free_block` panic with garbage MTVAL.

Fixed by switching to `xTaskCreateStaticPinnedToCore` with a
BSS-resident static 16 KB stack array in internal RAM. Mock budget
catalog had this in the PROPOSED architecture for a while; the firmware
just hadn't caught up.

### Defense-in-depth combo (commit `1ac42a1`)

Three knobs together because none alone covered all failure modes:

1. **`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536`** (was 32 KB) —
   doubles DMA-only pool. Eliminates `setup_dma_priv_buffer(1206)`
   panics in `13_spi_back_to_back.py`.
2. **`CONFIG_HEAP_POISONING_LIGHT=y`** — defense-in-depth canary on
   every allocation. Catches buffer overruns from the second
   still-unidentified PSRAM-stack source (likely `pimslo_save` 6 KB
   or `pimslo_cap` 8 KB).
3. **`err_cur` / `err_nxt` → MALLOC_CAP_SPIRAM unconditionally** —
   eliminates the encoder's per-frame internal-RAM allocation churn
   that fragmented the dma_int pool.

Mock catalog updated to match — DMA pool reservation reflected in the
DEFAULT/RAW models, encoder per-frame allocs in `pimslo_sim.c`'s
`run_encode_pipeline` model the PSRAM placement.
