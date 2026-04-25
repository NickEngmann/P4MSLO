/**
 * Predict end-to-end PIMSLO encode duration for both stack regimes.
 * Compares against the user's "<2 minute" target.
 */
#include <stdio.h>
#include "p4_timing.h"

static void run(const char *label, p4_pipeline_params_t params, int target_ms)
{
    p4_pipeline_timing_t t = p4_timing_estimate(params);
    printf("\n========== %s ==========\n", label);
    p4_timing_print(t, stdout);
    printf("  vs target %d ms (%d s): %s\n",
           target_ms, target_ms / 1000,
           (t.total_ms <= target_ms) ? "✓ PASS" : "✗ FAIL");
}

int main(void)
{
    int target_ms = 120 * 1000;  /* 2 min */

    run("BASELINE — PSRAM stack, PSRAM LUT (original firmware)",
        (p4_pipeline_params_t){ .n_cams = 4, .stack = P4_STACK_PSRAM,
                                 .lut = P4_LUT_PSRAM, .save_p4ms = true },
        target_ms);

    run("PROPOSED (commit f9fad72) — INTERNAL stack, PSRAM LUT",
        (p4_pipeline_params_t){ .n_cams = 4, .stack = P4_STACK_INTERNAL,
                                 .lut = P4_LUT_PSRAM, .save_p4ms = true },
        target_ms);

    run("PROPOSED + RGB444 LUT (4 KB internal, slight quality loss)",
        (p4_pipeline_params_t){ .n_cams = 4, .stack = P4_STACK_INTERNAL,
                                 .lut = P4_LUT_RGB444, .save_p4ms = true },
        target_ms);

    run("PROPOSED + OCTREE LUT in TCM (8 KB, no quality loss, SPI safe)",
        (p4_pipeline_params_t){ .n_cams = 4, .stack = P4_STACK_INTERNAL,
                                 .lut = P4_LUT_OCTREE_TCM, .save_p4ms = true },
        target_ms);

    run("PROPOSED + OCTREE LUT in HP L2MEM (8 KB, REJECTED — SPI risk)",
        (p4_pipeline_params_t){ .n_cams = 4, .stack = P4_STACK_INTERNAL,
                                 .lut = P4_LUT_OCTREE_HPRAM, .save_p4ms = true },
        target_ms);

    run("PROPOSED + 64 KB BSS LUT (drops gif_bg static stack)",
        (p4_pipeline_params_t){ .n_cams = 4, .stack = P4_STACK_INTERNAL,
                                 .lut = P4_LUT_INTERNAL, .save_p4ms = true },
        target_ms);

    printf("\n=== Best smaller-LUT path that hits the 2-min budget ===\n");
    printf("  Look for the first ✓ PASS above. That's the recommendation.\n");
    return 0;
}
