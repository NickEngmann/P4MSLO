/**
 * Smoke test for the constrained-allocator + budget catalog.
 *
 * Runs the BASELINE catalog through the simulator and prints which
 * allocs landed where. Useful to eyeball whether the model matches
 * what we see on the device.
 */
#include <stdio.h>
#include "p4_mem_model.h"
#include "p4_budget.h"

/* Budget simulation intentionally leaves all allocations live — we're
 * modeling "the device's steady state after init." LSan would yell. */
const char *__asan_default_options(void) { return "detect_leaks=0"; }

int main(void)
{
    printf("\n########## A: BASELINE catalog vs DEFAULT model (post-boot snapshot) ##########\n");
    p4_mem_init(P4_MEM_MODEL_DEFAULT);
    int rc_a = p4_budget_simulate(P4_BUDGET_BASELINE, P4_BUDGET_BASELINE_COUNT,
                                   P4_BUDGET_MODE_AS_IS, stdout);

    printf("\n########## B: BASELINE catalog vs RAW model (BSS drains pool) ##########\n");
    p4_mem_init(P4_MEM_MODEL_RAW);
    int rc_b = p4_budget_simulate(P4_BUDGET_BASELINE, P4_BUDGET_BASELINE_COUNT,
                                   P4_BUDGET_MODE_FROM_RAW, stdout);

    printf("\n########## C: PROPOSED catalog vs RAW model (the architecture we want) ##########\n");
    p4_mem_init(P4_MEM_MODEL_RAW);
    int rc_c = p4_budget_simulate(P4_BUDGET_PROPOSED, P4_BUDGET_PROPOSED_COUNT,
                                   P4_BUDGET_MODE_FROM_RAW, stdout);

    printf("\n=== Comparison ===\n");
    printf("  A (baseline / as-is):  fallbacks/fails rc=%d\n", rc_a);
    printf("  B (baseline / raw):    fallbacks/fails rc=%d\n", rc_b);
    printf("  C (proposed / raw):    fallbacks/fails rc=%d\n", rc_c);
    printf("  ↑ Goal: C should have FEWER fallbacks than A and B.\n");
    printf("    Specifically, the pimslo_gif task should NOT show up in\n");
    printf("    'WARN  [psram→fallback]' under PROPOSED (it's BSS now).\n");

    return 0;
}
