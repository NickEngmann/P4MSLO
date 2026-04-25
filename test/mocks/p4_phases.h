/**
 * @file p4_phases.h
 * @brief Phased lifecycle simulator for the PIMSLO encode pipeline.
 *
 * The static budget simulator (`p4_budget`) catches "does this all fit
 * at once" failures. But the encode pipeline has explicit free/alloc
 * sequences — viewfinder ↔ encoder scaled_buf share the same 7 MB
 * region — and a snapshot view shows them as a hard collision when
 * really they're sequenced.
 *
 * This phase simulator runs through concrete events and tracks alloc
 * lifetimes so we can catch ORDER-DEPENDENT failures: e.g. "if you
 * forget to free viewfinder before allocating scaled_buf, you OOM."
 *
 * Each event is one of:
 *   ALLOC name pool size       — adds a tracked allocation
 *   FREE  name                 — releases by name (must match prior ALLOC)
 *   CHECK_LARGEST pool size    — assertion: pool's largest is ≥ size
 *   PHASE label                — annotation for the report
 *
 * On ALLOC failure (size > pool's current largest_contiguous), the
 * event handler tries PSRAM fallback if `fallback_ok`, otherwise
 * records a hard fail and continues.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include "p4_mem_model.h"

typedef enum {
    P4_EV_ALLOC,
    P4_EV_FREE,
    P4_EV_CHECK_LARGEST,
    P4_EV_PHASE,
} p4_event_type_t;

typedef struct {
    p4_event_type_t type;
    const char     *label;
    p4_pool_t       pool;       /* for ALLOC + CHECK_LARGEST */
    size_t          size;       /* for ALLOC + CHECK_LARGEST */
    bool            fallback_ok; /* for ALLOC */
} p4_event_t;

typedef struct {
    int   alloc_ok;
    int   alloc_fallback;
    int   alloc_fail;
    int   check_pass;
    int   check_fail;
    int   free_ok;
    int   free_unknown;
} p4_phase_stats_t;

/* Run the event sequence against the current p4_mem model.
 * Returns 0 if no hard failures, <0 with the count of hard fails. */
int p4_phases_run(const p4_event_t *events, size_t n_events,
                  p4_phase_stats_t *out_stats, FILE *report);

/* Pre-baked sequence: full PIMSLO photo→encode flow.
 *   PHASE  boot
 *   ALLOC  viewfinder
 *   ALLOC  album_ppa_buf
 *   PHASE  capture (no big allocs, just SPI receive)
 *   PHASE  pre_encode
 *   FREE   viewfinder
 *   FREE   album_ppa_buf
 *   PHASE  encode_create
 *   ALLOC  encoder scaled_buf (7 MB)
 *   ALLOC  encoder pixel_lut (64 KB)
 *   PHASE  encode_pass1
 *   ... per-frame allocs (×4 cams) ...
 *   PHASE  encode_pass2
 *   ... per-frame allocs (×4 cams) ...
 *   PHASE  post_encode
 *   FREE   encoder scaled_buf
 *   FREE   encoder pixel_lut
 *   ALLOC  viewfinder
 *   ALLOC  album_ppa_buf
 */
extern const p4_event_t P4_PHASES_PIMSLO_FLOW[];
extern const size_t      P4_PHASES_PIMSLO_FLOW_COUNT;
