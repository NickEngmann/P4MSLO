/**
 * @file p4_timing.c
 * @brief PIMSLO pipeline timing estimator.
 */
#include "p4_timing.h"
#include <stdio.h>

static int stack_penalty(p4_stack_location_t stack)
{
    return (stack == P4_STACK_PSRAM)
           ? P4_TIMING_STACK_PENALTY_PSRAM_PCT
           : P4_TIMING_STACK_PENALTY_INTERNAL_PCT;
}

static int lut_penalty(p4_lut_location_t lut)
{
    switch (lut) {
        case P4_LUT_PSRAM:    return P4_TIMING_LUT_PENALTY_PSRAM_PCT;
        case P4_LUT_OCTREE:   return P4_TIMING_LUT_PENALTY_OCTREE_PCT;
        case P4_LUT_RGB444:   return P4_TIMING_LUT_PENALTY_RGB444_PCT;
        case P4_LUT_INTERNAL:
        default:              return P4_TIMING_LUT_PENALTY_INTERNAL_PCT;
    }
}

static int apply_penalty(int nominal_ms, p4_stack_location_t stack)
{
    return (nominal_ms * stack_penalty(stack)) / 100;
}

/* Pass 2 LZW+dither: dominated by per-pixel LUT reads, NOT by stack.
 * Hardware confirmed: with stack=INTERNAL but LUT=PSRAM, the encode
 * step stayed at ~55 s/frame. Stack only matters for decode/setup;
 * LUT is the binding constraint for the per-pixel hot loop. So we
 * pick the LARGER penalty rather than multiplying. */
static int apply_pass2_penalty(int nominal_ms, p4_stack_location_t stack,
                                p4_lut_location_t lut)
{
    int s = stack_penalty(stack);
    int l = lut_penalty(lut);
    int worst = (s > l) ? s : l;
    return (nominal_ms * worst) / 100;
}

p4_pipeline_timing_t p4_timing_estimate(p4_pipeline_params_t p)
{
    p4_pipeline_timing_t t = {0};
    int n = p.n_cams;
    if (n < 2) n = 2;
    if (n > 4) n = 4;

    /* .p4ms save: tjpgd decode 4 cams (one per camera). Stack-bound,
     * LUT-independent (tjpgd doesn't use the pixel_lut). */
    if (p.save_p4ms) {
        t.p4ms_save_ms = apply_penalty(P4_TIMING_P4MS_PER_FRAME_MS * n, p.stack);
    }

    /* Pass 1: palette accumulation, n decodes. Stack-bound. Doesn't
     * touch the per-encode pixel_lut. */
    t.pass1_ms = apply_penalty(P4_TIMING_PALETTE_PER_FRAME_MS * n, p.stack);
    t.pass1_ms += P4_TIMING_PALETTE_LUT_BUILD_MS;

    /* Pass 2: forward — n frames. Decode is stack-only; encode (dither
     * + LZW) is stack × LUT. */
    int decode_part = apply_penalty(P4_TIMING_DECODE_PER_FRAME_MS * n, p.stack);
    int encode_part = apply_pass2_penalty(P4_TIMING_ENCODE_PER_FRAME_MS * n,
                                           p.stack, p.lut);
    t.pass2_forward_ms = decode_part + encode_part;

    /* Pass 2: reverse — (n-2) middle frames replayed from cache, no
     * LZW re-encode but the file write still costs SD throughput. */
    int reverse_frames = (n >= 2) ? n - 2 : 0;
    t.pass2_replay_ms = P4_TIMING_REPLAY_PER_FRAME_MS * reverse_frames;
    /* SD writes are not stack-bound; keep at nominal. */

    /* Sum + per-encode setup. */
    t.decode_ms = (P4_TIMING_DECODE_PER_FRAME_MS * n);
    t.encode_ms = (P4_TIMING_ENCODE_PER_FRAME_MS * n);
    t.total_ms = t.p4ms_save_ms + t.pass1_ms + t.pass2_forward_ms + t.pass2_replay_ms
                 + P4_TIMING_VIEWFINDER_FREE_MS
                 + P4_TIMING_GIF_HEADER_MS
                 + P4_TIMING_VIEWFINDER_REALLOC_MS;
    return t;
}

void p4_timing_print(p4_pipeline_timing_t t, FILE *out)
{
    if (!out) return;
    fprintf(out, "  .p4ms save        : %5d ms  (%5.1f s)\n", t.p4ms_save_ms, t.p4ms_save_ms / 1000.0);
    fprintf(out, "  Pass 1 palette    : %5d ms  (%5.1f s)\n", t.pass1_ms, t.pass1_ms / 1000.0);
    fprintf(out, "  Pass 2 forward    : %5d ms  (%5.1f s)\n", t.pass2_forward_ms, t.pass2_forward_ms / 1000.0);
    fprintf(out, "  Pass 2 replay     : %5d ms  (%5.1f s)\n", t.pass2_replay_ms, t.pass2_replay_ms / 1000.0);
    fprintf(out, "  setup + finish    : %5d ms\n",
            P4_TIMING_VIEWFINDER_FREE_MS + P4_TIMING_GIF_HEADER_MS + P4_TIMING_VIEWFINDER_REALLOC_MS);
    fprintf(out, "  TOTAL             : %5d ms  (%5.1f s)\n", t.total_ms, t.total_ms / 1000.0);
}
