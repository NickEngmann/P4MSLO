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
    P4_LUT_INTERNAL,    /* LUT fits in internal RAM — Pass 2 fast */
    P4_LUT_PSRAM,       /* LUT in PSRAM — Pass 2 slow */
    P4_LUT_OCTREE,      /* small (~8 KB) hierarchical LUT in internal,
                         * 3-4 indirections per lookup but cache-warm */
    P4_LUT_RGB444,      /* 4 KB RGB444 LUT in internal — quality loss */
} p4_lut_location_t;

/* Per-frame timing baseline (with INTERNAL stack + INTERNAL LUT),
 * in milliseconds. Numbers from CLAUDE.md "Pipeline Timing" + measured
 * Frame timing logs. */
#define P4_TIMING_DECODE_PER_FRAME_MS    1700   /* tjpgd decode 2560×1920 → 1824×1920 */
#define P4_TIMING_ENCODE_PER_FRAME_MS    10000  /* dither + LZW for 1824×1920, INTERNAL LUT */
#define P4_TIMING_PALETTE_PER_FRAME_MS   1750   /* Pass 1 palette accumulation */
#define P4_TIMING_REPLAY_PER_FRAME_MS    9000   /* file write of cached frame bytes */

/* LUT-location penalty for the Pass 2 LZW+dither encode step.
 * Measured: with INTERNAL stack but LUT-in-PSRAM the encode is still
 * ~55 s/frame (vs ~10 s with LUT internal). So the LUT-location
 * penalty is bigger than the stack-location penalty.
 *   100 = LUT internal — nominal
 *   550 = LUT in PSRAM — measured, ~5.5×
 *   180 = LUT octree (4 indirections, all in internal) — estimated
 *         from typical octree-vs-table-lookup cost ratios
 *   140 = RGB444 LUT (1 lookup, smaller table) — slightly slower
 *         than RGB565 due to extra channel-shrink ops, but still
 *         cache-warm */
#define P4_TIMING_LUT_PENALTY_INTERNAL_PCT 100
#define P4_TIMING_LUT_PENALTY_PSRAM_PCT    550
#define P4_TIMING_LUT_PENALTY_OCTREE_PCT   140
#define P4_TIMING_LUT_PENALTY_RGB444_PCT   115

/* Per-encode setup costs (one-time per run). */
#define P4_TIMING_VIEWFINDER_FREE_MS      50
#define P4_TIMING_PALETTE_LUT_BUILD_MS    650
#define P4_TIMING_GIF_HEADER_MS           50
#define P4_TIMING_VIEWFINDER_REALLOC_MS   1300

/* .p4ms direct-JPEG save: 4 × tjpgd decode + canvas writes. */
#define P4_TIMING_P4MS_PER_FRAME_MS      1500

/* SD save costs (per pos*.jpg = ~250 KB at 250 KB/s). */
#define P4_TIMING_SD_SAVE_PER_FRAME_MS    1000

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
} p4_pipeline_params_t;

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
