/**
 * @file p4_timing.h
 * @brief Timing model for the PIMSLO encode pipeline.
 *
 * Captures the dominant timing factors observed on hardware so we can
 * predict end-to-end encode duration without flashing. The big knob is
 * **where the encoder task's stack lives**: every push/pop in the inner
 * loop hits internal SRAM at a few ns/access vs PSRAM at 100-200 ns/
 * access. Net effect: Pass 2 per-frame time goes from ~12 s (internal)
 * to ~55 s (PSRAM) on the same code.
 *
 * Numbers come from on-device measurement (Frame timing logs):
 *   - INTERNAL stack (legacy `pimslo` serial cmd path):
 *     decode ~1.7 s, encode ~10 s, total ~12 s/frame
 *   - PSRAM stack (`pimslo_encode_queue_task` from photo_btn flow):
 *     decode ~18 s, encode ~37 s, total ~55 s/frame
 *
 * The slowdown ratio (~4.6×) is consistent across decode and encode
 * sub-steps; we model it as a single STACK_PENALTY factor.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

typedef enum {
    P4_STACK_INTERNAL,  /* fast — internal SRAM, ~1× nominal */
    P4_STACK_PSRAM,     /* slow — PSRAM, ~4.6× nominal */
} p4_stack_location_t;

/* Where the 64 KB pixel_lut lives. Pass 2 LZW does 1 LUT read per
 * pixel × 3.5 Mpx per frame. PSRAM access ~100-200 ns each, so ~700 ms
 * minimum just for LUT reads if not cached. With cache misses (LUT
 * access pattern is not contiguous — RGB565 → 256-palette index is
 * effectively random) we measured 55 s/frame for the dither+LZW path
 * even AFTER the stack moved to internal. So LUT placement is the
 * remaining big knob. */
typedef enum {
    P4_LUT_INTERNAL,     /* LUT fits in HP L2MEM — Pass 2 fast.
                          * REJECTED: starves SPI's dma_int pool. */
    P4_LUT_PSRAM,        /* LUT in PSRAM — Pass 2 slow.
                          * Current state on commit 72e06bd. */
    P4_LUT_OCTREE_HPRAM, /* 8 KB hierarchical LUT in HP L2MEM. Same
                          * SPI conflict as INTERNAL — included only
                          * for comparison; do not ship. */
    P4_LUT_OCTREE_TCM,   /* 8 KB hierarchical LUT in TCM (0x30100000).
                          * NOT DMA-capable, separate from HP L2MEM, so
                          * SPI master priv-RX pool stays healthy. The
                          * recommended path. ~3 s/frame for Pass 2. */
    P4_LUT_RGB444,       /* 4 KB RGB444 LUT in internal — quality loss */
} p4_lut_location_t;

/* Per-frame timing baseline (with INTERNAL stack + INTERNAL LUT),
 * in milliseconds. Numbers from measured Frame timing logs:
 *   commit 79c09f9 (BSS LUT):
 *     decode=1605 ms, encode=2030 ms — both bottlenecks resolved
 *   commit f9fad72 (PSRAM LUT, INTERNAL stack):
 *     decode=1571 ms, encode=55328 ms — LUT bound
 * We model ENCODE nominal as 2000 ms (the BSS-LUT measurement) and
 * apply LUT penalties on top. */
#define P4_TIMING_DECODE_PER_FRAME_MS    1700   /* tjpgd decode 2560×1920 → 1824×1920 */
#define P4_TIMING_ENCODE_PER_FRAME_MS    2000   /* dither + LZW with INTERNAL LUT */
#define P4_TIMING_PALETTE_PER_FRAME_MS   1750   /* Pass 1 palette accumulation */
#define P4_TIMING_REPLAY_PER_FRAME_MS    9000   /* file write of cached frame bytes */

/* LUT-location penalty for the Pass 2 LZW+dither encode step.
 * Calibrated against hardware measurements (commit 79c09f9 vs
 * f9fad72): moving the 64 KB pixel_lut from PSRAM to internal BSS
 * dropped Pass 2 encode from 55 s/frame to 2.0 s/frame — a 27.5×
 * speedup. So the PSRAM penalty is much higher than originally
 * estimated; the per-pixel LUT read pattern is essentially random
 * across the 64 KB table, defeating the cache.
 *   100  = LUT internal — nominal (~2 s/frame)
 *   2750 = LUT in PSRAM — measured 27.5× hardware speedup of move
 *   200  = LUT octree (4 indirections, 8 KB table, cache-warm) —
 *          estimated; could be measured if we ever build it
 *   130  = RGB444 LUT (4 KB table, 1 lookup, cache-warm) — slight
 *          extra channel-shrink ops vs RGB565 */
#define P4_TIMING_LUT_PENALTY_INTERNAL_PCT     100  /* nominal */
#define P4_TIMING_LUT_PENALTY_PSRAM_PCT        2750 /* measured 27.5× */
#define P4_TIMING_LUT_PENALTY_OCTREE_HPRAM_PCT 200  /* HP L2MEM, 4 indir */
#define P4_TIMING_LUT_PENALTY_OCTREE_TCM_PCT   180  /* TCM is closer to
                                                     * the core than HP
                                                     * L2MEM; load latency
                                                     * is 1-2 cycles vs
                                                     * ~3-4 for L2MEM hit.
                                                     * Slightly faster
                                                     * per-indirection. */
#define P4_TIMING_LUT_PENALTY_RGB444_PCT       130  /* 4 KB, 1 lookup */

/* Per-encode setup costs (one-time per run). */
#define P4_TIMING_VIEWFINDER_FREE_MS      50
#define P4_TIMING_PALETTE_LUT_BUILD_MS    650
#define P4_TIMING_GIF_HEADER_MS           50
#define P4_TIMING_VIEWFINDER_REALLOC_MS   1300

/* .p4ms direct-JPEG save: 4 × tjpgd decode + canvas writes. */
#define P4_TIMING_P4MS_PER_FRAME_MS      1500

/* SD save costs (per pos*.jpg = ~250 KB at 250 KB/s). */
#define P4_TIMING_SD_SAVE_PER_FRAME_MS    1000

/* SD read cost for a 600 KB source JPEG at ~250 KB/s. Currently the
 * encoder re-reads each pos*.jpg from SD TWICE — once in Pass 1 and
 * once in Pass 2 — even though the bytes were already loaded into
 * `jpeg_data[]` earlier in the function. Caching across passes saves
 * `n_cams × 2 × P4_TIMING_SD_REREAD_PER_FRAME_MS` per encode. */
#define P4_TIMING_SD_REREAD_PER_FRAME_MS  2400

/* Stack-penalty factor as a percentage of nominal.
 *   100 = no penalty (internal stack)
 *   460 = 4.6× slowdown (PSRAM stack), measured */
#define P4_TIMING_STACK_PENALTY_INTERNAL_PCT  100
#define P4_TIMING_STACK_PENALTY_PSRAM_PCT     460

typedef struct {
    int n_cams;             /* 2-4, number of cameras with usable JPEGs */
    p4_stack_location_t stack;
    p4_lut_location_t   lut;
    bool save_p4ms;         /* whether to include the .p4ms direct-JPEG save */
    /* Cache source JPEGs across Pass 1 and Pass 2. Currently the
     * encoder re-reads each pos*.jpg twice; caching saves
     * 2 × n_cams × P4_TIMING_SD_REREAD_PER_FRAME_MS. Memory cost:
     * ~2.4 MB held during the whole encode (4 × 600 KB), allocated
     * AFTER gif_encoder_create() so the 7 MB scaled_buf alloc isn't
     * fragmented. Pure win, single-core, no quality change. */
    bool cache_source_jpegs;

    /* Dual-core boost mode. When ui_extra_is_display_sleeping() is
     * true on hardware, Core 0 (LVGL + video_stream paused) is fully
     * idle and can be enlisted to help the encoder. Concrete wins
     * the mock models:
     *   - Pass 1 palette accumulation: each frame's tjpgd decode +
     *     histogram contribution is independent. Run them in
     *     parallel across both cores. Two-frame parallelism caps the
     *     speedup at ~50 %; merge overhead + memory contention drops
     *     the realized win to ~35 %.
     *   - Pass 2 forward: SD prefetch of frame N+1 in parallel with
     *     dither + LZW of frame N. JPEG buffers are ~600 KB so they
     *     fit (the 7 MB scaled_buf can NOT be double-buffered).
     *     Realized win ~15 % on the forward pass.
     * Pass 2 replay (cached frame writes) and the .p4ms save are SD
     * I/O–bound and unchanged. */
    bool boost_dual_core;
} p4_pipeline_params_t;

/* Boost mode multipliers applied to Pass 1 and Pass 2 forward when
 * boost_dual_core is true. Calibrated to what's ACTUALLY
 * implementable on hardware given the 7 MB scaled_buf + ~8 MB
 * largest-contig PSRAM constraint:
 *
 *   - The scaled_buf can NOT be double-buffered for true
 *     decode/dither pipelining. Two 7 MB buffers would need 14 MB
 *     contig.
 *   - .p4ms save (4× tjpgd into 240×240 canvas) IS parallelizable —
 *     each cam is independent and the 240×240 canvas is 115 KB. Two
 *     cores can do 2 cams each in parallel. ~40 % savings on the
 *     .p4ms portion.
 *   - Pass 1 histogram accumulation is fast (small), so parallel
 *     decode there yields ~8 % savings.
 *   - Pass 2 forward: SD prefetch of next frame's JPEG (~600 KB) on
 *     Core 0 while Core 1 dithers the current frame on Core 1. Saves
 *     the SD-read latency once per frame ≈ 3 % of pass-2 time.
 *
 * If these prove unrealistic when implemented, adjust the constants
 * here and rerun the test; the speedup-assertion threshold is also
 * driven from these numbers. */
#define P4_BOOST_PASS1_PCT          92   /* 8 % off */
#define P4_BOOST_PASS2_FWD_PCT      97   /* 3 % off */
#define P4_BOOST_P4MS_SAVE_PCT      60   /* 40 % off */

typedef struct {
    int decode_ms;
    int encode_ms;
    int p4ms_save_ms;
    int pass1_ms;
    int pass2_forward_ms;
    int pass2_replay_ms;
    int total_ms;
} p4_pipeline_timing_t;

/* Estimate end-to-end encode duration for the given parameters. */
p4_pipeline_timing_t p4_timing_estimate(p4_pipeline_params_t params);

/* Print a human-readable timing breakdown. */
void p4_timing_print(p4_pipeline_timing_t t, FILE *out);
