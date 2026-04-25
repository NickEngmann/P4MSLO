/**
 * @file p4_budget.h
 * @brief Per-component memory footprint catalog for P4-EYE.
 *
 * The architecture question we keep getting stuck on is: with N components
 * each demanding chunks of internal RAM (bounded by ~7 KB largest free
 * contiguous block) or PSRAM (bounded by ~8 MB largest), does the
 * proposed layout actually fit?
 *
 * Rather than guess + flash + measure each iteration, this catalog
 * captures every known allocation by component, with measured numbers
 * from CLAUDE.md / the linker map / serial heap_caps logs. Pass a list
 * of `p4_component_t` entries to `p4_budget_simulate()` and it returns
 * an annotated report on whether the proposed configuration fits.
 *
 * "Fitting" means:
 *   1. Every static/BSS reservation lands without overrunning the
 *      DRAM segment (linker would fail on real device — we model it).
 *   2. Every runtime alloc that asks for INTERNAL has a contiguous
 *      block to land in. If not, we mark it as "fell back to PSRAM"
 *      (matches the FreeRTOS allocator silent-fallback behavior).
 *   3. No DMA-internal allocation runs the pool below the LCD priv-TX
 *      reserve threshold — that's the line where the screen freezes.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include "p4_mem_model.h"

typedef enum {
    P4_LIFETIME_BSS,           /* link-time fixed allocation in DRAM */
    P4_LIFETIME_PERMANENT,     /* heap_caps_malloc, never freed */
    P4_LIFETIME_PER_ENCODE,    /* alloc'd on each encode, freed at end */
    P4_LIFETIME_PER_FRAME,     /* alloc'd per frame within encode */
    P4_LIFETIME_PER_TASK_LIFE, /* xTaskCreate stack — held until task dies */
} p4_lifetime_t;

typedef struct {
    const char    *name;
    p4_pool_t      pool;        /* preferred pool */
    bool           psram_fallback_ok; /* if `pool` doesn't have room, OK to land in PSRAM? */
    p4_lifetime_t  lifetime;
    size_t         size_bytes;
    /* Optional notes / source citation for the number. */
    const char    *note;
} p4_component_t;

/* Budget catalog: every known sized allocation on the device.
 * Numbers come from CLAUDE.md "Known Issues" + linker map + on-device
 * heap_caps reads. Update this when the firmware changes. */
extern const p4_component_t P4_BUDGET_BASELINE[];
extern const size_t          P4_BUDGET_BASELINE_COUNT;

/* Proposed architecture catalog. Differences from BASELINE:
 *   - gif_encoder.c::tjwork DROPPED (shared with s_tjpgd_work via mutex)
 *   - gif_encoder.c::file_buf moved to PSRAM (EXT_RAM_BSS_ATTR)
 *   - pimslo_gif task: 16 KB BSS-resident static stack (was heap-alloc
 *     16 KB that always landed in PSRAM)
 *
 * Net BSS delta: -32 KB (drop tjwork) -32 KB (file_buf to PSRAM) +16 KB (static stack)
 *              = -48 KB internal BSS freed. */
extern const p4_component_t P4_BUDGET_PROPOSED[];
extern const size_t          P4_BUDGET_PROPOSED_COUNT;

typedef enum {
    /* DEFAULT model: BSS already baked into starting state. BSS items
     * in the catalog are just labels — no deduction. Use to simulate
     * runtime behavior of the AS-IS firmware. */
    P4_BUDGET_MODE_AS_IS = 0,
    /* RAW model: BSS items deduct from pool size. Use to compare
     * "before vs after we change BSS layout". */
    P4_BUDGET_MODE_FROM_RAW = 1,
} p4_budget_mode_t;

/* Apply every entry in the budget to the current p4_mem model.
 * Returns 0 if everything fits as preferred, >0 if some allocs had
 * to fall back to PSRAM (each fallback prints a WARN line), <0 on
 * a hard failure (alloc neither fits the preferred pool nor PSRAM,
 * or BSS would overflow DRAM). */
int p4_budget_simulate(const p4_component_t *items, size_t n_items,
                        p4_budget_mode_t mode, FILE *report);

/* Convenience: simulate the BASELINE budget on a default-shape
 * memory model and return the result. */
int p4_budget_simulate_baseline(FILE *report);
