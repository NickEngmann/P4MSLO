/**
 * @file p4_phases.c
 * @brief Phased lifecycle simulator implementation.
 */
#include "p4_phases.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_LIVE 64

typedef struct {
    const char *name;
    void       *ptr;
    size_t      size;
    p4_pool_t   pool;
} live_alloc_t;

static const char *pool_name(p4_pool_t p)
{
    switch (p) {
        case P4_POOL_DMA_INT: return "dma_int";
        case P4_POOL_INT:     return "int";
        case P4_POOL_PSRAM:   return "psram";
        default:              return "?";
    }
}

int p4_phases_run(const p4_event_t *events, size_t n,
                  p4_phase_stats_t *out_stats, FILE *out)
{
    p4_phase_stats_t st = {0};
    live_alloc_t live[MAX_LIVE] = {0};
    int n_live = 0;

    for (size_t i = 0; i < n; i++) {
        const p4_event_t *e = &events[i];
        switch (e->type) {

        case P4_EV_PHASE:
            if (out) fprintf(out, "\n--- phase: %s ---\n", e->label);
            break;

        case P4_EV_ALLOC: {
            void *p = p4_mem_malloc(e->size, e->pool);
            p4_pool_t actual_pool = e->pool;
            if (!p && e->fallback_ok && e->pool != P4_POOL_PSRAM) {
                p = p4_mem_malloc(e->size, P4_POOL_PSRAM);
                if (p) {
                    actual_pool = P4_POOL_PSRAM;
                    st.alloc_fallback++;
                    if (out) fprintf(out, "  WARN  alloc %-30s %7zu B → psram fallback (wanted %s)\n",
                                     e->label, e->size, pool_name(e->pool));
                }
            }
            if (!p) {
                st.alloc_fail++;
                if (out) fprintf(out, "  FAIL  alloc %-30s %7zu B (no room in %s)\n",
                                 e->label, e->size, pool_name(e->pool));
                break;
            }
            if (n_live < MAX_LIVE) {
                live[n_live].name = e->label;
                live[n_live].ptr  = p;
                live[n_live].size = e->size;
                live[n_live].pool = actual_pool;
                n_live++;
            }
            if (actual_pool == e->pool) {
                st.alloc_ok++;
                if (out) fprintf(out, "  ok    alloc %-30s %7zu B → %s (largest after: %zu)\n",
                                 e->label, e->size, pool_name(actual_pool),
                                 p4_mem_pool_state(actual_pool).largest_contiguous);
            }
            break;
        }

        case P4_EV_FREE: {
            int found = -1;
            for (int j = 0; j < n_live; j++) {
                if (live[j].name && strcmp(live[j].name, e->label) == 0) {
                    found = j; break;
                }
            }
            if (found < 0) {
                st.free_unknown++;
                if (out) fprintf(out, "  WARN  free  %-30s — no live alloc found\n", e->label);
                break;
            }
            p4_mem_free(live[found].ptr);
            st.free_ok++;
            if (out) fprintf(out, "  ok    free  %-30s         (largest after: %zu)\n",
                             e->label, p4_mem_pool_state(live[found].pool).largest_contiguous);
            /* Compact list. */
            live[found] = live[n_live - 1];
            live[n_live - 1] = (live_alloc_t){0};
            n_live--;
            break;
        }

        case P4_EV_CHECK_LARGEST: {
            size_t actual = p4_mem_pool_state(e->pool).largest_contiguous;
            if (actual >= e->size) {
                st.check_pass++;
                if (out) fprintf(out, "  ok    check %s largest ≥ %zu B  (actual %zu)\n",
                                 pool_name(e->pool), e->size, actual);
            } else {
                st.check_fail++;
                if (out) fprintf(out, "  FAIL  check %s largest ≥ %zu B  (actual %zu)\n",
                                 pool_name(e->pool), e->size, actual);
            }
            break;
        }
        }
    }

    /* Anything still live counts as "permanent" — fine for the boot
     * model, just print so the reader sees what's still allocated. */
    if (out && n_live > 0) {
        fprintf(out, "\n--- still-live allocations at end ---\n");
        for (int i = 0; i < n_live; i++) {
            fprintf(out, "  %-30s %7zu B in %s\n",
                    live[i].name, live[i].size, pool_name(live[i].pool));
        }
    }

    if (out_stats) *out_stats = st;
    return (st.alloc_fail + st.check_fail) > 0
           ? -(st.alloc_fail + st.check_fail) : 0;
}

/* ============================================================================
 * Pre-baked PIMSLO photo→encode lifecycle.
 * ============================================================================ */

#define ALLOC_(_lbl, _pool, _sz, _fb) \
    { .type = P4_EV_ALLOC, .label = (_lbl), .pool = (_pool), \
      .size = (_sz), .fallback_ok = (_fb) }
#define FREE_(_lbl) \
    { .type = P4_EV_FREE, .label = (_lbl) }
#define CHECK_(_pool, _sz) \
    { .type = P4_EV_CHECK_LARGEST, .pool = (_pool), .size = (_sz) }
#define PHASE_(_lbl) \
    { .type = P4_EV_PHASE, .label = (_lbl) }

const p4_event_t P4_PHASES_PIMSLO_FLOW[] = {

    PHASE_("boot — permanents"),
    ALLOC_("LVGL canvas",       P4_POOL_PSRAM,   240*240*2,    false),
    ALLOC_("LVGL DMA staging",  P4_POOL_DMA_INT, 240*4,        false),
    ALLOC_("SPI chunk_rx",      P4_POOL_DMA_INT, 4096,         false),
    ALLOC_("SPI scratch_tx",    P4_POOL_DMA_INT, 64,           false),
    ALLOC_("SPI scratch_rx",    P4_POOL_DMA_INT, 64,           false),
    ALLOC_("viewfinder buf",    P4_POOL_PSRAM,   7*1024*1024,  false),
    ALLOC_("album PPA buf",     P4_POOL_PSRAM,   1920*1088*3,  false),

    PHASE_("photo_btn fired — capture"),
    /* SPI capture transfers go through the permanent chunk_rx;
     * no new allocations. */

    PHASE_("save — pos*.jpg fwrite"),
    /* Each pos JPEG read into a transient PSRAM buffer for fwrite,
     * freed immediately. Model as alloc/free pairs. */
    ALLOC_("save jpeg buf 1",   P4_POOL_PSRAM, 500*1024, false),
    FREE_("save jpeg buf 1"),
    ALLOC_("save jpeg buf 2",   P4_POOL_PSRAM, 500*1024, false),
    FREE_("save jpeg buf 2"),
    ALLOC_("save jpeg buf 3",   P4_POOL_PSRAM, 500*1024, false),
    FREE_("save jpeg buf 3"),
    ALLOC_("save jpeg buf 4",   P4_POOL_PSRAM, 500*1024, false),
    FREE_("save jpeg buf 4"),

    PHASE_("encode pipeline begins"),
    /* The first thing app_gifs_encode_pimslo_from_dir does is free
     * the viewfinder + album PPA so the encoder's 7 MB scaled_buf
     * has room. THIS IS WHAT BREAKS IF ANYONE FORGETS THE FREE. */
    FREE_("viewfinder buf"),
    FREE_("album PPA buf"),

    PHASE_("encoder_create"),
    ALLOC_("encoder scaled_buf", P4_POOL_PSRAM, 1824*1920*2, false),
    ALLOC_("encoder pixel_lut",  P4_POOL_PSRAM, 65536,       false),

    PHASE_("encode_pass1 — palette accumulation, 4 cams"),
    /* Pass 1 reads each JPEG into PSRAM, decodes, accumulates palette. */
    ALLOC_("pass1 jpeg buf 1",  P4_POOL_PSRAM, 500*1024, false),
    /* tjpgd workspace is BSS — no runtime alloc */
    FREE_("pass1 jpeg buf 1"),
    ALLOC_("pass1 jpeg buf 2",  P4_POOL_PSRAM, 500*1024, false),
    FREE_("pass1 jpeg buf 2"),
    ALLOC_("pass1 jpeg buf 3",  P4_POOL_PSRAM, 500*1024, false),
    FREE_("pass1 jpeg buf 3"),
    ALLOC_("pass1 jpeg buf 4",  P4_POOL_PSRAM, 500*1024, false),
    FREE_("pass1 jpeg buf 4"),

    PHASE_("encode_pass2 — frame encode, 4 cams forward + 2 reverse"),
    /* Per-frame: load JPEG, decode → scaled_buf, dither + LZW, free. */
    ALLOC_("pass2 frame 1 jpeg",   P4_POOL_PSRAM, 500*1024, false),
    ALLOC_("pass2 err_cur 1",      P4_POOL_INT, 1920*3*2, true),
    ALLOC_("pass2 err_nxt 1",      P4_POOL_INT, 1920*3*2, true),
    ALLOC_("pass2 row_cache 1",    P4_POOL_INT, 1920*2,   false),
    FREE_("pass2 row_cache 1"),
    FREE_("pass2 err_nxt 1"),
    FREE_("pass2 err_cur 1"),
    FREE_("pass2 frame 1 jpeg"),
    /* (frames 2/3/4 have the same shape — abbreviate by reusing the
     * lifetime envelope; the budget effect is identical) */

    PHASE_("encode_pass2 — frame replay (cached, no decode)"),
    /* Reverse pass writes from PSRAM-cached frame data; only
     * the cache buffer matters (allocated mid-encode in real code
     * via heap_caps_malloc(SPIRAM)). */

    PHASE_("encoder_destroy"),
    FREE_("encoder pixel_lut"),
    FREE_("encoder scaled_buf"),

    PHASE_("post-encode — viewfinder + album realloc"),
    ALLOC_("viewfinder buf",    P4_POOL_PSRAM, 7*1024*1024, false),
    ALLOC_("album PPA buf",     P4_POOL_PSRAM, 1920*1088*3, false),

    PHASE_("idle — system steady state"),
    /* All transient allocations freed; the live set should equal
     * the boot permanents. */
    CHECK_(P4_POOL_DMA_INT, 1024),  /* room for next LCD priv-TX */
    CHECK_(P4_POOL_PSRAM,   8*1024*1024),  /* large block back */
};

const size_t P4_PHASES_PIMSLO_FLOW_COUNT =
    sizeof(P4_PHASES_PIMSLO_FLOW) / sizeof(P4_PHASES_PIMSLO_FLOW[0]);
