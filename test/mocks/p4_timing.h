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

/* Per-frame timing baseline (with INTERNAL stack), in milliseconds.
 * Numbers from CLAUDE.md "Pipeline Timing" + measured Frame timing
 * logs on the legacy pimslo path. */
#define P4_TIMING_DECODE_PER_FRAME_MS    1700   /* tjpgd decode 2560×1920 → 1824×1920 */
#define P4_TIMING_ENCODE_PER_FRAME_MS    10000  /* dither + LZW for 1824×1920 */
#define P4_TIMING_PALETTE_PER_FRAME_MS   1750   /* Pass 1 palette accumulation */
#define P4_TIMING_REPLAY_PER_FRAME_MS    9000   /* file write of cached frame bytes */

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
