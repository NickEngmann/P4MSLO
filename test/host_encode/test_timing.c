/**
 * Predict end-to-end PIMSLO encode duration for both stack regimes.
 * Compares against the user's "<2 minute" target.
 */
#include <stdio.h>
#include "p4_timing.h"

int main(void)
{
    printf("\n========== INTERNAL stack (PROPOSED — static BSS) ==========\n");
    p4_pipeline_timing_t t_int = p4_timing_estimate((p4_pipeline_params_t){
        .n_cams = 4, .stack = P4_STACK_INTERNAL, .save_p4ms = true,
    });
    p4_timing_print(t_int, stdout);

    printf("\n========== PSRAM stack (BASELINE — current FreeRTOS fallback) ==========\n");
    p4_pipeline_timing_t t_psr = p4_timing_estimate((p4_pipeline_params_t){
        .n_cams = 4, .stack = P4_STACK_PSRAM, .save_p4ms = true,
    });
    p4_timing_print(t_psr, stdout);

    int target_ms = 120 * 1000;
    printf("\n=== Verdict (target ≤ %d ms / 2 min) ===\n", target_ms);
    printf("  INTERNAL stack: %5.1f s  %s\n", t_int.total_ms / 1000.0,
           (t_int.total_ms <= target_ms) ? "✓ PASS" : "✗ FAIL");
    printf("  PSRAM stack:    %5.1f s  %s\n", t_psr.total_ms / 1000.0,
           (t_psr.total_ms <= target_ms) ? "✓ PASS" : "✗ FAIL");
    printf("  Speedup of INTERNAL vs PSRAM: %.1f×\n",
           (double)t_psr.total_ms / t_int.total_ms);
    return 0;
}
