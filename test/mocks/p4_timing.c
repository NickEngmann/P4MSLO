/**
 * @file p4_timing.c
 * @brief PIMSLO pipeline timing estimator.
 */
#include "p4_timing.h"
#include <stdio.h>

static int apply_penalty(int nominal_ms, p4_stack_location_t stack)
{
    int pct = (stack == P4_STACK_PSRAM)
              ? P4_TIMING_STACK_PENALTY_PSRAM_PCT
              : P4_TIMING_STACK_PENALTY_INTERNAL_PCT;
    return (nominal_ms * pct) / 100;
}

p4_pipeline_timing_t p4_timing_estimate(p4_pipeline_params_t p)
{
    p4_pipeline_timing_t t = {0};
    int n = p.n_cams;
    if (n < 2) n = 2;
    if (n > 4) n = 4;

    /* .p4ms save: tjpgd decode 4 cams (one per camera). Stack-bound. */
    if (p.save_p4ms) {
        t.p4ms_save_ms = apply_penalty(P4_TIMING_P4MS_PER_FRAME_MS * n, p.stack);
    }

    /* Pass 1: palette accumulation, n decodes. Stack-bound. */
    t.pass1_ms = apply_penalty(P4_TIMING_PALETTE_PER_FRAME_MS * n, p.stack);
    t.pass1_ms += P4_TIMING_PALETTE_LUT_BUILD_MS;

    /* Pass 2: forward — n frames decoded + LZW-encoded. Stack-bound. */
    int per_frame_total = P4_TIMING_DECODE_PER_FRAME_MS + P4_TIMING_ENCODE_PER_FRAME_MS;
    t.pass2_forward_ms = apply_penalty(per_frame_total * n, p.stack);

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
