/**
 * Phase-based simulator: walks through the full PIMSLO photo→encode
 * lifecycle event by event under the constrained allocator. Catches
 * order-dependent bugs (e.g. "forgot to free viewfinder before
 * allocating encoder scaled_buf") that the static budget can't see.
 */
#include <stdio.h>
#include "p4_mem_model.h"
#include "p4_phases.h"

const char *__asan_default_options(void) { return "detect_leaks=0"; }

int main(void)
{
    printf("\n========== PIMSLO photo→encode lifecycle on DEFAULT model ==========\n");
    p4_mem_init(P4_MEM_MODEL_DEFAULT);
    p4_phase_stats_t st_a = {0};
    int rc_a = p4_phases_run(P4_PHASES_PIMSLO_FLOW, P4_PHASES_PIMSLO_FLOW_COUNT,
                              &st_a, stdout);

    printf("\n========== Same lifecycle, but skip the 'free viewfinder' bug ==========\n");
    /* We can't easily inject a missing FREE without rebuilding the
     * event list, but we CAN run the same flow on a DEFAULT model
     * that already has viewfinder space pre-consumed (i.e. simulate
     * a missing free). For now just print a label so the structure
     * is in place. */
    /* TODO: bug-injection variants for each known regression class */

    printf("\n=== Summary ===\n");
    printf("  alloc ok / fallback / fail  : %d / %d / %d\n",
           st_a.alloc_ok, st_a.alloc_fallback, st_a.alloc_fail);
    printf("  check pass / fail           : %d / %d\n",
           st_a.check_pass, st_a.check_fail);
    printf("  free  ok / unknown          : %d / %d\n",
           st_a.free_ok, st_a.free_unknown);
    printf("  rc                          : %d\n", rc_a);
    return (rc_a < 0) ? 1 : 0;
}
