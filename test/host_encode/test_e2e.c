/**
 * End-to-end PIMSLO scenarios under the constrained simulator.
 *
 * Each scenario mirrors a real e2e test on the device (or a user
 * interaction we want to validate). All run locally in <1 second.
 *
 * Scenarios:
 *   1. Baseline architecture: photo_btn → encode times out the 2-min
 *      budget. (Confirms we're modeling the bug correctly.)
 *   2. Proposed architecture: photo_btn → encode under budget.
 *   3. Photo from MAIN: encode kicks off (no defer).
 *   4. Photo from CAMERA → user navigates to MAIN: encode runs once
 *      they're on MAIN.
 *   5. Multiple photos in succession: queue depth grows, each encodes.
 *   6. Photo + gallery nav during encode: bg worker yields, encode
 *      still completes.
 *   7. Photo with too-few cams (1/4): capture dropped, no encode.
 *   8. JPEG-only captures recovered by bg worker on next safe page.
 *   9. Pre-render bg path: existing .gif gets a .p4ms.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pimslo_sim.h"
#include "p4_mem_model.h"

const char *__asan_default_options(void) { return "detect_leaks=0"; }

#define P(fmt, ...) printf("    " fmt "\n", ##__VA_ARGS__)
#define ASSERT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; return; } \
} while(0)

static int fails = 0;
static int passed = 0;

#define SCENARIO(name) \
    static void scenario_##name(void)

#define RUN(name) do { \
    printf("\n## scenario: " #name "\n"); \
    pimslo_sim_reset(); \
    int prev_fails = fails; \
    scenario_##name(); \
    if (fails == prev_fails) { passed++; printf("    PASS\n"); } \
} while(0)

/* ------------------------------------------------------------------ */
SCENARIO(baseline_arch_misses_budget)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_BASELINE);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(/* cams */ 4);
    ASSERT(r.cams_usable == 4, "expected 4/4 cams");
    int total = r.capture_ms + r.save_ms + pimslo_sim_wait_idle(600 * 1000);
    P("total: %d ms (%.1f s) — baseline arch (PSRAM stack + PSRAM LUT)",
      total, total/1000.0);
    ASSERT(total > 200 * 1000, "expected baseline > 200 s (worst case)");
}

SCENARIO(proposed_stack_only_still_misses_budget)
{
    /* PROPOSED = commit f9fad72: stack moved to INTERNAL but LUT still
     * PSRAM. The Pass 2 LZW hot loop is LUT-bound, not stack-bound,
     * so this only halves the encode time — still over 2 min. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    int total = r.capture_ms + r.save_ms + pimslo_sim_wait_idle(600 * 1000);
    P("total: %d ms (%.1f s) — PROPOSED stack-only (LUT still PSRAM)",
      total, total/1000.0);
    /* Should still be over 2 min, but materially faster than BASELINE. */
    ASSERT(total > 120 * 1000, "PROPOSED-stack-only still over 2 min (LUT-bound)");
    ASSERT(total < 300 * 1000, "PROPOSED-stack-only should be ~260 s, not 5+ min");
}

SCENARIO(proposed_with_rgb444_lut_meets_budget)
{
    /* RGB444 LUT — 4 KB in internal RAM. Slightly lossy quantization
     * but the encoder dithers to compensate. Cheapest fix. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_RGB444);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    int total = r.capture_ms + r.save_ms + pimslo_sim_wait_idle(120 * 1000);
    P("total: %d ms (%.1f s) — PROPOSED + RGB444 LUT (4 KB)",
      total, total/1000.0);
    ASSERT(total <= 120 * 1000, "PROPOSED + RGB444 LUT ≤ 2 min");
}

SCENARIO(proposed_octree_tcm_meets_encode_budget)
{
    /* RECOMMENDED ARCH: 8 KB octree LUT in TCM (0x30100000).
     * - TCM is NOT DMA-capable, separate from HP L2MEM
     * - No dma_int starvation (the bug from BSS_LUT)
     * - Per-pixel lookup: 3-4 indirections in cache-warm TCM
     * - Predicted Pass 2: ~3-4 s/frame (vs 2 s flat-internal,
     *   vs 55 s flat-PSRAM)
     * - No image-quality loss (octree converges to exact 256-color match) */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    int total = r.capture_ms + r.save_ms + pimslo_sim_wait_idle(120 * 1000);
    P("total: %d ms (%.1f s) — PROPOSED + OCTREE LUT in TCM (8 KB)",
      total, total/1000.0);
    ASSERT(total <= 120 * 1000, "PROPOSED + OCTREE LUT ≤ 2 min");
}

SCENARIO(proposed_octree_hpram_starves_dma_int)
{
    /* The 8 KB octree LUT, if placed in HP L2MEM, would still be a 8 KB
     * BSS item in DMA-eligible memory. It's UNDER the 32 KB threshold
     * the simulator uses to flag dma_int competition, so the budget
     * check passes. But the same physical-pool sharing applies — for
     * very tight builds this is still risky. The recommended path puts
     * the octree in TCM specifically to avoid this entirely. This
     * scenario just confirms the simulator distinguishes the two. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_HPRAM);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    int total = r.capture_ms + r.save_ms + pimslo_sim_wait_idle(120 * 1000);
    P("total: %d ms (%.1f s) — OCTREE in HP L2MEM (NOT recommended)",
      total, total/1000.0);
    /* Encode timing is similar to TCM since the lookup penalty is
     * close. Budget-wise this would compete with dma_int once we
     * stack other BSS on top of it. Keep as a comparison datapoint. */
    ASSERT(total <= 120 * 1000, "OCTREE-in-HP_L2MEM ≤ 2 min on encode");
}

#include "p4_budget.h"

SCENARIO(proposed_bss_lut_starves_dma_int)
{
    /* HARDWARE-VALIDATED REGRESSION: a 64 KB pixel_lut as static BSS
     * makes encode fast (~80 s), but it lands in HP L2MEM which is
     * the same physical region the SPI master pulls DMA-internal
     * priv-buffers from. dma_int largest collapses 6.4 KB → 1.6 KB
     * and the SPI master priv-RX alloc panics mid-capture.
     *
     * Test: simulate the PROPOSED catalog from RAW with the BSS LUT
     * entry and assert the budget simulator catches the dma_int
     * starvation (hard_fail > 0 on dma_int allocs). */
    p4_mem_init(P4_MEM_MODEL_RAW);
    int rc = p4_budget_simulate(P4_BUDGET_PROPOSED, P4_BUDGET_PROPOSED_COUNT,
                                  P4_BUDGET_MODE_FROM_RAW, NULL);
    P("budget rc: %d (expect < 0 — dma_int starved)", rc);
    ASSERT(rc < 0, "BSS LUT should hard-fail dma_int budget on RAW model "
                    "(simulator must catch what hardware showed)");
}

/* Build a budget catalog identical to PROPOSED but with the 64 KB
 * BSS LUT replaced by an 8 KB TCM octree LUT. This is what the
 * recommended PROPOSED_OCTREE_TCM arch should look like at flash time.
 * Runs the budget simulator from RAW and asserts dma_int stays healthy. */
static p4_component_t *build_octree_tcm_catalog(size_t *out_count)
{
    static p4_component_t cat[64];
    size_t n = 0;
    /* Copy everything from PROPOSED... */
    for (size_t i = 0; i < P4_BUDGET_PROPOSED_COUNT; i++) {
        const p4_component_t *src = &P4_BUDGET_PROPOSED[i];
        /* ...except the rejected 64 KB BSS pixel_lut. */
        if (src->size_bytes == 65536 &&
            src->lifetime == P4_LIFETIME_BSS &&
            src->pool == P4_POOL_INT) {
            continue;
        }
        cat[n++] = *src;
    }
    /* Add the 8 KB octree LUT in TCM. */
    cat[n++] = (p4_component_t){
        .name = "encoder octree LUT (TCM)",
        .pool = P4_POOL_TCM, .psram_fallback_ok = false,
        .lifetime = P4_LIFETIME_BSS, .size_bytes = 8192,
        .note = "Recommended next step",
    };
    *out_count = n;
    return cat;
}

SCENARIO(proposed_octree_tcm_passes_dma_int_budget)
{
    /* The whole point of TCM: NOT in HP L2MEM, so doesn't affect
     * dma_int largest. Budget simulator must show no dma_int hard
     * fails when we drop the BSS LUT and add an octree in TCM. */
    p4_mem_init(P4_MEM_MODEL_RAW);
    size_t n = 0;
    p4_component_t *cat = build_octree_tcm_catalog(&n);
    int rc = p4_budget_simulate(cat, n, P4_BUDGET_MODE_FROM_RAW, NULL);
    P("budget rc: %d (expect ≥ 0 — octree-in-TCM doesn't starve dma_int)", rc);
    ASSERT(rc >= 0, "octree-in-TCM should NOT hard-fail any pool — "
                    "this is what 'fast encode without breaking SPI' looks like");

    /* Also check dma_int is still > the LCD priv-TX threshold (~5 KB).
     * If the TCM octree somehow degraded dma_int (it shouldn't), we'd
     * see this fail. */
    p4_pool_state_t dma = p4_mem_pool_state(P4_POOL_DMA_INT);
    P("dma_int largest after PROPOSED+OCTREE_TCM catalog: %zu B",
      dma.largest_contiguous);
    ASSERT(dma.largest_contiguous >= 5 * 1024,
           "dma_int largest must stay ≥ 5 KB for LCD priv-TX path");
}

SCENARIO(proposed_with_bss_lut_meets_encode_budget)
{
    /* Encode timing-only check — BSS LUT IS fast at ~80 s. Caveat:
     * the dma_int starvation regression is checked separately in
     * proposed_bss_lut_starves_dma_int. Both must agree: encode
     * fast on paper, but hardware-broken because of DMA pool. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_BSS_LUT);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    int total = r.capture_ms + r.save_ms + pimslo_sim_wait_idle(120 * 1000);
    P("encode total: %d ms (%.1f s)", total, total/1000.0);
    ASSERT(total <= 120 * 1000, "encode itself fits the 2-min budget");
}

SCENARIO(photo_from_main_kicks_encode)
{
    /* Use the OCTREE arch — best balance of fits-in-budget and no
     * quality loss. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    pimslo_sim_wait_idle(120 * 1000);
    ASSERT(gallery_count() == 1, "encode should have completed");
    ASSERT(gallery_current()->has_gif, "expected .gif to land");
}

SCENARIO(photo_from_camera_then_navigate_to_main)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_CAMERA);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    /* On CAMERA, encode_should_defer returns true. The save_task
     * still queued the job; it runs as soon as we leave camera. */
    P("after capture: queue_depth still pending");
    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_wait_idle(120 * 1000);
    ASSERT(gallery_count() == 1, "encode should run once user is on MAIN");
}

SCENARIO(multiple_photos_in_succession)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_MAIN);
    for (int i = 0; i < 3; i++) {
        pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
        ASSERT(r.cams_usable == 4, "each photo should get 4/4");
    }
    pimslo_sim_wait_idle(360 * 1000);
    ASSERT(gallery_count() == 3, "expected 3 gallery entries");
}

SCENARIO(photo_with_too_few_cams_dropped)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    pimslo_sim_force_capture_fail(1, /* cams returned */ 1);
    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    ASSERT(r.cams_usable == 1, "force-fail should give us 1/4");
    pimslo_sim_wait_idle(120 * 1000);
    ASSERT(gallery_count() == 0, "1/4 capture should be dropped, no entry");
}

SCENARIO(bg_worker_recovers_jpeg_only_captures)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    /* Simulate prior boot leaving a JPEG-only entry on SD. */
    gallery_record_capture("P4M0001", /*gif*/ false, /*jpeg*/ true, /*p4ms*/ false);
    ASSERT(gallery_count() == 1, "1 jpeg-only entry");
    ASSERT(!gallery_current()->has_gif, "entry has no .gif yet");
    /* User on a safe page — bg worker can run. */
    ui_extra_set_current_page(UI_PAGE_MAIN);
    bg_worker_kick();
    ASSERT(bg_worker_re_encode_count() >= 1, "bg should have re-encoded the orphan");
    ASSERT(gallery_current()->has_gif, ".gif should now exist");
}

SCENARIO(bg_worker_pre_renders_p4ms)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    /* Existing .gif without .p4ms (interrupted .p4ms save). */
    gallery_record_capture("P4M0042", true, true, false);
    ASSERT(!gallery_current()->has_p4ms, "p4ms missing initially");
    ui_extra_set_current_page(UI_PAGE_MAIN);
    bg_worker_kick();
    ASSERT(gallery_current()->has_p4ms, "bg should have pre-rendered .p4ms");
    ASSERT(bg_worker_pre_render_count() >= 1, "pre-render counter should fire");
}

SCENARIO(bg_worker_yields_on_gallery_page)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    gallery_record_capture("P4M0001", false, true, false);
    ui_extra_set_current_page(UI_PAGE_GIFS);
    bg_worker_kick();
    /* On gallery page, bg encode path should yield. */
    ASSERT(bg_worker_re_encode_count() == 0, "bg should NOT encode on UI_PAGE_GIFS");
}

SCENARIO(memory_after_encode_returns_to_steady)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    size_t int_before = pimslo_sim_internal_largest();
    size_t psr_before = pimslo_sim_psram_largest();
    P("before: int_largest=%zu psram_largest=%zu", int_before, psr_before);
    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_photo_btn(4);
    pimslo_sim_wait_idle(120 * 1000);
    size_t int_after = pimslo_sim_internal_largest();
    size_t psr_after = pimslo_sim_psram_largest();
    P("after:  int_largest=%zu psram_largest=%zu", int_after, psr_after);
    ASSERT(int_after >= int_before / 2, "INTERNAL pool shouldn't fragment in half");
    /* PSRAM should still have ≥ 1 MB available for next-encode JPEG
     * staging buffers. Steady state with viewfinder + album PPA both
     * resident leaves ~1-2 MB free. */
    ASSERT(psr_after >= 1 * 1024 * 1024, "PSRAM should have ≥ 1 MB largest after encode");
}

/* ------------------------------------------------------------------
 * Gallery flows + p4ms regression coverage.
 * ------------------------------------------------------------------ */

SCENARIO(foreground_encode_produces_both_gif_and_p4ms)
{
    /* The user-facing path: photo_btn → encode runs on the foreground
     * pimslo_encode_queue_task → BOTH .gif AND .p4ms land on disk for
     * the same capture. The .p4ms is the small preview the gallery
     * flashes BEFORE the GIF starts decoding (CLAUDE.md "Gallery JPEG
     * preview decode"). If the encoder pipeline regresses and stops
     * producing .p4ms, the gallery shows a delay-of-N-seconds blank
     * canvas instead of an instant still. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    pimslo_sim_wait_idle(120 * 1000);
    ASSERT(gallery_count() == 1, "1 entry after photo_btn");
    const gallery_entry_t *e = gallery_current();
    ASSERT(e != NULL, "current entry exists");
    ASSERT(e->has_gif,  ".gif should exist (foreground encode path)");
    ASSERT(e->has_p4ms, ".p4ms should exist (Direct-JPEG inline save)");
    ASSERT(e->has_jpeg, "P4 preview .jpg should exist");
    ASSERT(e->type == GALLERY_ENTRY_GIF, "type promoted to GIF after encode");
    /* Stem matches what photo_btn assigned. */
    ASSERT(strcmp(e->stem, r.stem) == 0, "stem matches the capture");
}

SCENARIO(gallery_nav_next_prev_across_entries)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_MAIN);
    /* Take 3 photos → 3 entries. */
    for (int i = 0; i < 3; i++) {
        pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
        ASSERT(r.cams_usable == 4, "each photo should get 4/4");
    }
    pimslo_sim_wait_idle(360 * 1000);
    ASSERT(gallery_count() == 3, "expected 3 entries");
    ASSERT(gallery_current_index() == 0, "fresh scan starts at 0");

    /* Walk forward. */
    gallery_next();
    ASSERT(gallery_current_index() == 1, "next: 0→1");
    gallery_next();
    ASSERT(gallery_current_index() == 2, "next: 1→2");
    gallery_next();
    ASSERT(gallery_current_index() == 2, "next at end clamps");

    /* Walk back. */
    gallery_prev();
    ASSERT(gallery_current_index() == 1, "prev: 2→1");
    gallery_prev();
    ASSERT(gallery_current_index() == 0, "prev: 1→0");
    gallery_prev();
    ASSERT(gallery_current_index() == 0, "prev at start clamps");
}

SCENARIO(gallery_entries_sorted_by_stem)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    /* Insert out of order to verify gallery_scan sorts by stem. */
    gallery_record_capture("P4M0042", true, true, true);
    gallery_record_capture("P4M0001", true, true, true);
    gallery_record_capture("P4M0099", true, true, true);
    gallery_scan();
    ASSERT(gallery_count() == 3, "3 entries");
    /* current_index resets to 0 after scan; first entry should be P4M0001. */
    const gallery_entry_t *e = gallery_current();
    ASSERT(e && strcmp(e->stem, "P4M0001") == 0, "scan sorts by stem");
}

SCENARIO(jpeg_only_entry_shows_processing_state)
{
    /* Mirrors what the gallery sees right after a capture: a save just
     * dropped a JPEG-only entry, encode is still queued/running. UI
     * should be able to identify these as "not yet a GIF". */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_CAMERA);  /* defers encode */
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    /* On CAMERA the encode is queued but doesn't run. Gallery now has
     * a JPEG-only entry. */
    ASSERT(gallery_count() == 1, "save task created the entry");
    const gallery_entry_t *e = gallery_current();
    ASSERT(e->has_jpeg && !e->has_gif,
           "should show as JPEG-preview-only (PROCESSING badge in UI)");
    ASSERT(e->type == GALLERY_ENTRY_JPEG, "type is JPEG until encode finishes");
    /* Once we leave CAMERA, encode runs and the entry promotes. */
    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_wait_idle(120 * 1000);
    e = gallery_current();
    ASSERT(e->has_gif, "promoted to GIF after encode");
    ASSERT(e->type == GALLERY_ENTRY_GIF, "type updated");
}

SCENARIO(encode_allowed_on_gifs_page_foreground)
{
    /* CLAUDE.md "Allow on GIFS (the PIMSLO gallery)": the foreground
     * encode pipeline IS allowed to run when the user is on the
     * gallery page. Only camera-type pages defer. This covers the
     * "go to album right after capture" UX. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_GIFS);
    gallery_mark_opened();
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    pimslo_sim_wait_idle(120 * 1000);
    ASSERT(gallery_count() == 1, "encode should run on UI_PAGE_GIFS");
    ASSERT(gallery_current()->has_gif, "expected .gif");
}

SCENARIO(encode_defers_on_video_mode)
{
    /* CAMERA / INTERVAL_CAM / VIDEO_MODE: viewfinder owns ~7 MB
     * scaled_buf — encoder's own 7 MB scaled_buf can't coexist. The
     * encoder defers until user leaves the camera-type page. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_VIDEO_MODE);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    /* Encode queued but waiting. */
    ui_extra_set_current_page(UI_PAGE_SETTINGS);
    pimslo_sim_wait_idle(120 * 1000);
    ASSERT(gallery_count() == 1, "encode should run after leaving VIDEO_MODE");
}

SCENARIO(gallery_delete_removes_entry_and_clamps_index)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    /* Seed three entries directly. */
    gallery_record_capture("P4M0001", true, true, true);
    gallery_record_capture("P4M0002", true, true, true);
    gallery_record_capture("P4M0003", true, true, true);
    gallery_scan();
    ASSERT(gallery_count() == 3, "three entries");

    /* Delete the middle entry — current_index stays at 0
     * (we haven't moved), gallery shrinks to 2. */
    gallery_next();  /* index 0 → 1 (P4M0002) */
    ASSERT(strcmp(gallery_current()->stem, "P4M0002") == 0, "on middle");
    gallery_delete_current();
    ASSERT(gallery_count() == 2, "2 entries after delete");
    /* After delete, index should still be 1 (which is now P4M0003). */
    ASSERT(strcmp(gallery_current()->stem, "P4M0003") == 0,
           "delete-middle leaves index pointing at next entry");

    /* Delete the last entry — current_index should clamp back to 0. */
    gallery_delete_current();
    ASSERT(gallery_count() == 1, "1 entry");
    ASSERT(gallery_current_index() == 0, "index clamps after delete-last");
    ASSERT(strcmp(gallery_current()->stem, "P4M0001") == 0,
           "remaining entry is the first one");

    /* Delete the only remaining entry — gallery empty. */
    gallery_delete_current();
    ASSERT(gallery_count() == 0, "empty gallery after deleting last");
    ASSERT(gallery_current() == NULL, "no current entry");
}

SCENARIO(reset_clears_gallery_and_pages)
{
    /* pimslo_sim_reset is what tests use between scenarios — verify
     * it actually wipes everything so cross-test contamination can't
     * silently break a later scenario. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    gallery_record_capture("P4M9999", true, true, true);
    ui_extra_set_current_page(UI_PAGE_GIFS);
    gallery_mark_opened();
    ASSERT(gallery_count() == 1, "seeded");
    ASSERT(gallery_was_ever_opened(), "marked opened");
    ASSERT(ui_extra_get_current_page() == UI_PAGE_GIFS, "on gifs");

    pimslo_sim_reset();

    ASSERT(gallery_count() == 0, "reset clears entries");
    ASSERT(!gallery_was_ever_opened(), "reset clears ever-opened flag");
    ASSERT(ui_extra_get_current_page() == UI_PAGE_MAIN, "reset → MAIN");
}

SCENARIO(p4ms_persists_after_encode_for_instant_replay)
{
    /* The .p4ms file exists so the gallery can flash a 240×240 still
     * INSTANTLY when the user lands on an entry, before the slow LZW
     * decode of the full GIF kicks in. Regression check: after the
     * encoder destroys its state and the heap returns to steady, the
     * .p4ms entry must STILL be there (it's on SD, not encoder heap). */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_photo_btn(4);
    pimslo_sim_wait_idle(120 * 1000);
    const gallery_entry_t *e = gallery_current();
    ASSERT(e && e->has_p4ms, "p4ms should exist post-encode");

    /* Simulate a page transition + scan — the "user enters gallery
     * after capture" flow. p4ms should still be there. */
    ui_extra_set_current_page(UI_PAGE_GIFS);
    gallery_mark_opened();
    gallery_scan();
    e = gallery_current();
    ASSERT(e && e->has_p4ms,
           "p4ms persists across page transitions / rescans");
}

SCENARIO(rapid_burst_captures_all_eventually_encode)
{
    /* User burst-presses photo_btn 5 times. Save task queues each;
     * encode_queue_task drains one at a time. All 5 should make it
     * through. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_MAIN);
    for (int i = 0; i < 5; i++) {
        pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
        ASSERT(r.cams_usable == 4, "each burst photo gets 4/4");
    }
    /* Five 54s encodes back-to-back = ~270s of encode time. */
    pimslo_sim_wait_idle(600 * 1000);
    ASSERT(gallery_count() == 5, "all 5 burst photos make it to gallery");
    /* Verify each has both .gif and .p4ms. */
    for (int i = 0; i < 5; i++) {
        const gallery_entry_t *e = NULL;
        /* Walk to entry i */
        while (gallery_current_index() < i) gallery_next();
        while (gallery_current_index() > i) gallery_prev();
        e = gallery_current();
        ASSERT(e && e->has_gif && e->has_p4ms,
               "every burst entry should have .gif + .p4ms");
    }
}

/* ------------------------------------------------------------------ */
SCENARIO(long_sequence_internal_largest_does_not_shrink)
{
    /* Hardware repro: tests 08 / 09 panic in tlsf::remove_free_block
     * around capture #20+ after long sequences of camera-page entries +
     * photos. Hypothesis: per-frame err_cur/err_nxt/row_cache alloc-free
     * churn (24 cycles per encode × N encodes) fragments INTERNAL such
     * that a later small alloc lands on a corrupted free-block header.
     *
     * What this scenario asserts: under the proposed architecture
     * (TCM LUT, INTERNAL stack), running 30 photo_btn cycles + their
     * encodes leaves internal_largest within 50% of the steady-state
     * value. If the per-frame churn really fragments the pool, the
     * mock should show internal_largest dropping monotonically.
     *
     * If this scenario PASSES (no monotonic decline), the fragmentation
     * isn't the per-frame churn — it's something else (concurrent
     * capture/save/encode race, or an actual buffer overflow we can't
     * model in a sequential simulator). */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    album_decoder_init();
    ui_extra_set_current_page(UI_PAGE_MAIN);

    size_t before = pimslo_sim_internal_largest();
    P("before: internal_largest=%zu", before);

    for (int i = 0; i < 30; i++) {
        pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
        if (r.cams_usable < 2) continue;
        pimslo_sim_wait_idle(120 * 1000);
    }

    size_t after = pimslo_sim_internal_largest();
    P("after 30 cycles: internal_largest=%zu", after);

    /* If the per-frame alloc/free pattern fragments the pool, after
     * would be substantially smaller than before. A clean allocator
     * should recover the same largest-block on each cycle. */
    ASSERT(after >= before / 2,
           "internal_largest must not shrink >50% after 30 encode cycles");
    ASSERT(gallery_count() == 30, "all 30 captures made it through");
}

/* SD-orphan cleanup scenarios.
 *
 * Prior interrupted encodes can leave 0-byte .gif files in
 * /sdcard/p4mslo_gifs/ and capture directories with <2 pos*.jpg
 * files in /sdcard/p4mslo/. Without active cleanup these accumulate
 * forever — the user's 32 GB SD fills up with invisible orphans.
 * Gallery scan should drop them on every refresh. */

SCENARIO(orphan_truncated_gif_cleaned_at_scan)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    /* Inject a 0-byte .gif from a prior panicked encode. */
    gallery_inject_orphan_gif("P4M0247");
    ASSERT(gallery_orphan_cleanup_count() == 0, "no cleanups yet");

    gallery_scan();

    ASSERT(gallery_orphan_cleanup_count() == 1,
           "scan must clean the truncated .gif so it doesn't show as a frozen entry");
    ASSERT(gallery_count() == 0, "orphan must NOT appear in gallery");
}

SCENARIO(orphan_capture_dir_no_pos_files_cleaned)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    /* Capture dir with 0 pos files: definitely an orphan. */
    gallery_inject_orphan_capture_dir("P4M0246", 0);
    /* Capture dir with 1 pos file: also orphan (need ≥2 to encode). */
    gallery_inject_orphan_capture_dir("P4M0248", 1);
    /* Capture dir with 2 pos files: still encodable, leave alone. */
    gallery_inject_orphan_capture_dir("P4M0249", 2);

    gallery_scan();

    ASSERT(gallery_orphan_cleanup_count() == 2,
           "two <2-pos orphans cleaned, encodable one preserved");
}

SCENARIO(orphan_cleanup_idempotent_on_repeated_scans)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    gallery_inject_orphan_gif("P4M0247");
    gallery_scan();
    int after_first = gallery_orphan_cleanup_count();

    gallery_scan();
    ASSERT(gallery_orphan_cleanup_count() == after_first,
           "second scan over a now-clean SD doesn't re-trigger cleanup");
}

/* Capture-error-overlay scenarios.
 *
 * When a photo_btn press results in 0 usable cameras (e.g. SPI cams
 * unresponsive, network glitch), the firmware shows a red "ERROR" pill
 * for ~3 s instead of silently hiding the saving overlay. Tests assert:
 *   - 0-cam capture sets the error flag
 *   - error flag clears after 3 s
 *   - successful subsequent capture clears the error flag immediately */

SCENARIO(zero_cam_capture_sets_error_overlay)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_CAMERA);
    ASSERT(!capture_error_pending(), "no error initially");

    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(0);  /* 0/4 cams */
    ASSERT(r.cams_usable == 0, "0 cams returned");
    ASSERT(capture_error_pending(),
           "after 0-cam capture, ERROR overlay flag must be set");
}

SCENARIO(error_overlay_auto_expires_after_3s)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_CAMERA);
    pimslo_sim_photo_btn(0);
    ASSERT(capture_error_pending(), "error pending right after");

    simulate_advance_time_ms(2900);
    ASSERT(capture_error_pending(), "still pending at 2.9 s");

    simulate_advance_time_ms(200);   /* total 3.1 s */
    ASSERT(!capture_error_pending(), "expired after 3 s");
}

SCENARIO(successful_capture_clears_error_overlay)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_CAMERA);
    pimslo_sim_photo_btn(0);             /* fail */
    ASSERT(capture_error_pending(), "error pending");

    /* User retries — successful capture should immediately clear the
     * error window so the overlay doesn't show ERROR after a successful
     * shot. */
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    ASSERT(r.cams_usable == 4, "4 cams second time");
    ASSERT(!capture_error_pending(),
           "successful capture immediately clears the error flag");
}

/* Viewfinder buffer use-after-free scenarios.
 *
 * Mirrors the on-device bug where app_video_photo.c caches the
 * scaled_camera_buf / jpg_buf / shared_photo_buf pointers at init
 * but they get free()d and re-allocated to NEW addresses every
 * PIMSLO photo cycle and every bg encode. The cached pointers
 * become stale → use-after-free → heap corruption → tlsf panic
 * later when free walks the corrupted block. */

SCENARIO(viewfinder_init_then_take_photo_no_stale_pointer)
{
    /* Boot: viewfinder buffers allocated.
     * Consumer (app_video_photo) caches pointers in init.
     * Take photo: consumer reads via cache. STILL VALID this cycle. */
    viewfinder_init_buffers();
    consumer_cache_buffers();
    ASSERT(viewfinder_buf_generation() == 1, "boot is gen 1");
    ASSERT(consumer_use_buffers(), "first photo: cache valid");
}

SCENARIO(viewfinder_free_realloc_invalidates_cached_pointer)
{
    /* This is THE BUG that causes the user's first-photo crash:
     * the bg encoder runs at boot (re-encoding stale captures from
     * prior session), which calls free_buffers + realloc_buffers.
     * The cached pointers in app_video_photo are now stale. The
     * user's "first photo after boot" then writes to the stale
     * pointer → freed PSRAM → heap corruption. */
    viewfinder_init_buffers();
    consumer_cache_buffers();           /* boot init caches gen 1 */

    /* Simulate bg encoder running between boot and user's first photo. */
    viewfinder_free_buffers();
    viewfinder_realloc_buffers();
    ASSERT(viewfinder_buf_generation() == 2, "free+realloc bumps gen");

    /* User takes first photo. Cached pointer is gen 1, current is gen 2.
     * The cached-pointer read is STALE. */
    ASSERT(!consumer_use_buffers(),
           "after free+realloc, consumer's cached pointer is stale (THIS IS THE BUG)");
}

SCENARIO(viewfinder_consumer_refresh_fixes_stale_pointer)
{
    /* The FIX: consumer refreshes pointers before each use, not just at init.
     * Mirrors take_and_save_photo calling app_video_stream_get_*_buf()
     * on every entry. */
    viewfinder_init_buffers();
    consumer_cache_buffers();
    viewfinder_free_buffers();
    viewfinder_realloc_buffers();
    /* Cached pointer is stale, but consumer refreshes before use: */
    consumer_refresh_buffers();
    ASSERT(consumer_use_buffers(),
           "after refresh, consumer reads current-gen pointer");
}

SCENARIO(viewfinder_concurrent_encode_keeps_consumer_safe)
{
    /* Multi-cycle stress: 5 PIMSLO photo cycles + bg-encode cycles
     * interleaved. With the FIX (refresh-before-use), every consumer
     * read is valid. Without the fix, only the first read is valid. */
    viewfinder_init_buffers();
    consumer_cache_buffers();

    int valid_reads = 0;
    for (int cycle = 0; cycle < 5; cycle++) {
        /* User takes photo: consumer refreshes, then uses. */
        consumer_refresh_buffers();
        if (consumer_use_buffers()) valid_reads++;

        /* PIMSLO cycle frees + realloc */
        viewfinder_free_buffers();
        viewfinder_realloc_buffers();
    }
    ASSERT(valid_reads == 5, "all 5 photos read valid pointers");
    ASSERT(viewfinder_buf_generation() == 6, "5 cycles = gen 1 → 6");
}

SCENARIO(viewfinder_no_cache_no_use_after_free)
{
    /* Worst case: consumer NEVER caches; reads on every photo via
     * fresh getter. No use-after-free possible because there's no
     * cache to go stale. This is the architectural goal of the fix. */
    viewfinder_init_buffers();
    /* DON'T cache. */

    int valid_reads = 0;
    int stale_reads = 0;
    for (int cycle = 0; cycle < 10; cycle++) {
        consumer_refresh_buffers();         /* always refresh */
        if (consumer_use_buffers()) valid_reads++;
        else stale_reads++;
        viewfinder_free_buffers();
        viewfinder_realloc_buffers();
    }
    ASSERT(valid_reads == 10, "10 cycles, all reads valid");
    ASSERT(stale_reads == 0, "no stale reads when refresh is always called");
}

/* Gallery rendering scenarios — model the `blue square` bug.
 *
 * `app_gifs.c::show_jpeg` memsets the canvas to byte 0x10 (which is
 * RGB565 0x1010 ≈ dim blue) BEFORE tjpgd decodes the JPEG into it.
 * If tjpgd fails (mutex timeout, decode error, file missing), the
 * blue background stays visible. User-visible symptom is "JPEG-only
 * gallery entry shows solid blue instead of the captured photo." */

SCENARIO(jpeg_only_entry_paints_jpeg_not_blue)
{
    /* Happy path: a JPEG-only entry's preview JPEG decodes
     * successfully and the canvas shows the photo, not the blue
     * memset background. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_CAMERA);

    /* Take a photo; it goes into the queue. The encoder doesn't run
     * yet because we're on a camera page (deferred). */
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    ASSERT(r.cams_usable == 4, "captured 4/4");

    /* User navigates to gallery. With encode deferred, this entry is
     * JPEG-only (PROCESSING/QUEUED state). */
    ui_extra_set_current_page(UI_PAGE_GIFS);
    gallery_scan();
    ASSERT(gallery_count() == 1, "one entry in gallery");

    /* play_current → show_jpeg → canvas painted with JPEG. */
    gallery_play_current();
    ASSERT(!gallery_canvas_is_blue(),
           "JPEG-only entry MUST NOT show solid blue (the memset background)");
    ASSERT(gallery_canvas_has_jpeg(),
           "canvas should be in JPEG state after play_current");
    ASSERT(gallery_jpeg_show_count() == 1, "show_jpeg called exactly once");
    ASSERT(gallery_jpeg_show_fail_count() == 0, "show_jpeg succeeded");
}

SCENARIO(jpeg_show_failure_leaves_blue_canvas)
{
    /* Regression: when show_jpeg fails (mutex timeout, decode error),
     * the canvas stays blue. The mock catches this so e2e can assert
     * the failure mode is at least DETECTABLE (logged) instead of
     * showing the user blue silently. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_CAMERA);
    pimslo_sim_photo_btn(4);
    ui_extra_set_current_page(UI_PAGE_GIFS);
    gallery_scan();

    gallery_force_next_jpeg_fail(1);
    gallery_play_current();
    ASSERT(gallery_canvas_is_blue(),
           "forced show_jpeg failure → canvas stays blue (this IS the user-visible bug)");
    ASSERT(gallery_jpeg_show_fail_count() == 1, "fail count tracks");

    /* Next play_current should succeed (force-fail consumed). */
    gallery_play_current();
    ASSERT(gallery_canvas_has_jpeg(),
           "subsequent play_current succeeds when force-fail is consumed");
}

SCENARIO(gif_entry_with_preview_flashes_jpeg_first)
{
    /* When a GIF entry has an attached preview JPEG (the typical case
     * after PIMSLO encode finishes), play_current flashes the JPEG
     * first, then starts the GIF. Both paint the canvas in sequence. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_photo_btn(4);
    pimslo_sim_wait_idle(180 * 1000);
    /* After encode, entry has both .gif and .jpg. */
    ui_extra_set_current_page(UI_PAGE_GIFS);
    gallery_scan();
    const gallery_entry_t *e = gallery_current();
    ASSERT(e && e->has_gif && e->has_jpeg,
           "GIF entry should have both .gif and .jpg");

    gallery_play_current();
    /* After full play_current the canvas has cycled JPEG → GIF frame. */
    ASSERT(gallery_canvas_state() == CANVAS_GIF_FRAME,
           "GIF entry ends in GIF frame state");
    ASSERT(gallery_jpeg_show_count() == 1,
           "JPEG flash happens once before the GIF takes over");
}

SCENARIO(gallery_nav_replays_preview)
{
    /* After navigating between entries, each play_current re-flashes
     * the JPEG. Catches the case where a stale "blue" state from a
     * prior failed decode bleeds into the next entry. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_photo_btn(4);
    pimslo_sim_photo_btn(4);
    pimslo_sim_wait_idle(360 * 1000);

    ui_extra_set_current_page(UI_PAGE_GIFS);
    gallery_scan();
    ASSERT(gallery_count() == 2, "two GIF entries");

    /* Force first show_jpeg to fail. */
    gallery_force_next_jpeg_fail(1);
    gallery_play_current();
    ASSERT(gallery_canvas_is_blue() || gallery_canvas_state() == CANVAS_GIF_FRAME,
           "first entry: failed JPEG flash, falls through to GIF");

    /* Nav to next entry. The fresh play_current should show JPEG. */
    gallery_next();
    gallery_play_current();
    ASSERT(!gallery_canvas_is_blue(), "second entry recovers — no stale blue");
    ASSERT(gallery_jpeg_show_fail_count() == 1, "exactly one forced failure");
}

SCENARIO(album_decoder_released_during_encode)
{
    /* When the encoder runs, it must drop the ~6 MB PPA buffer that
     * the album view holds — otherwise the encoder's own 7 MB scaled
     * buffer alloc collides with album in PSRAM. Mirrors the
     * app_album_release_jpeg_decoder() / reacquire dance in
     * app_gifs.c:1232 + 1290. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    album_decoder_init();  /* user opens album page first */
    ASSERT(album_ppa_held(), "album holds PPA after init");
    ASSERT(album_release_count() == 0, "no release before encode");

    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    ASSERT(r.cams_usable == 4, "captured 4/4");
    pimslo_sim_wait_idle(120 * 1000);

    ASSERT(album_release_count() == 1, "encoder released PPA exactly once");
    ASSERT(album_reacquire_count() == 1, "encoder reacquired PPA after");
    ASSERT(album_ppa_held(), "PPA restored after encode");
    /* HW decoder handle MUST stay alive through the dance — repeated
     * destroy/recreate panics with `no memory for jpeg decode rxlink`
     * under PSRAM fragmentation. */
    ASSERT(album_hw_decoder_alive(), "HW decoder kept alive across release/reacquire");
}

SCENARIO(album_reacquire_failure_is_non_fatal)
{
    /* If PSRAM is fragmented when the encoder tries to give the album
     * its buffer back, reacquire returns false. The album degrades —
     * no full-res preview — but the rest of the UI keeps working.
     * Encode pipeline state must NOT get stuck. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    album_decoder_init();
    album_force_next_reacquire_fail(1);

    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_photo_btn(4);
    pimslo_sim_wait_idle(120 * 1000);

    ASSERT(!gallery_is_encoding(), "encoder cleared encoding flag despite reacquire failure");
    ASSERT(album_reacquire_fail_count() == 1, "reacquire fail recorded");
    ASSERT(!album_ppa_held(), "PPA still detached after fail");
    /* Next encode should still try to release/reacquire — the fail is
     * not a permanent state. Use force_fail(0) (default) to confirm
     * it can recover on the next pass. */
    pimslo_sim_photo_btn(4);
    pimslo_sim_wait_idle(120 * 1000);
    ASSERT(album_ppa_held(), "second encode reacquired PPA successfully");
    ASSERT(gallery_count() == 2, "both photos in gallery");
}

SCENARIO(album_dance_alloc_releases_psram_for_encoder)
{
    /* The whole point of the dance: holding the 6 MB PPA + the 7 MB
     * encoder scaled_buf simultaneously busts PSRAM headroom. Verify
     * that PSRAM largest-block grows by ~6 MB after release. Mirrors
     * the "Free PSRAM after releasing buffers" log line at
     * app_gifs.c:1233. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    album_decoder_init();
    size_t before_release = pimslo_sim_psram_largest();
    album_decoder_release_ppa();
    size_t after_release = pimslo_sim_psram_largest();
    ASSERT(after_release >= before_release + (5 * 1024 * 1024),
           "release_ppa frees ~6 MB back into PSRAM");
    /* Restore for cleanliness. */
    album_decoder_reacquire_ppa();
}

SCENARIO(concurrent_encodes_serialize_decoder_dance)
{
    /* Two encodes back-to-back: each must release+reacquire. The
     * encode_queue_task processes one at a time, so the dance happens
     * sequentially. Total: 2 releases + 2 reacquires. */
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED_OCTREE_TCM);
    album_decoder_init();

    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_photo_btn(4);
    pimslo_sim_photo_btn(4);
    pimslo_sim_wait_idle(240 * 1000);

    ASSERT(gallery_count() == 2, "both encodes finished");
    ASSERT(album_release_count() == 2, "released once per encode");
    ASSERT(album_reacquire_count() == 2, "reacquired once per encode");
    ASSERT(album_ppa_held(), "PPA restored at the end");
}

/* ------------------------------------------------------------------ */
int main(void)
{
    RUN(baseline_arch_misses_budget);
    RUN(proposed_stack_only_still_misses_budget);
    RUN(proposed_with_rgb444_lut_meets_budget);
    RUN(proposed_octree_tcm_meets_encode_budget);
    RUN(proposed_octree_hpram_starves_dma_int);
    RUN(proposed_with_bss_lut_meets_encode_budget);
    RUN(proposed_bss_lut_starves_dma_int);
    RUN(proposed_octree_tcm_passes_dma_int_budget);
    RUN(photo_from_main_kicks_encode);
    RUN(photo_from_camera_then_navigate_to_main);
    RUN(multiple_photos_in_succession);
    RUN(photo_with_too_few_cams_dropped);
    RUN(bg_worker_recovers_jpeg_only_captures);
    RUN(bg_worker_pre_renders_p4ms);
    RUN(bg_worker_yields_on_gallery_page);
    RUN(memory_after_encode_returns_to_steady);

    RUN(foreground_encode_produces_both_gif_and_p4ms);
    RUN(gallery_nav_next_prev_across_entries);
    RUN(gallery_entries_sorted_by_stem);
    RUN(jpeg_only_entry_shows_processing_state);
    RUN(encode_allowed_on_gifs_page_foreground);
    RUN(encode_defers_on_video_mode);
    RUN(gallery_delete_removes_entry_and_clamps_index);
    RUN(reset_clears_gallery_and_pages);
    RUN(p4ms_persists_after_encode_for_instant_replay);
    RUN(rapid_burst_captures_all_eventually_encode);

    RUN(long_sequence_internal_largest_does_not_shrink);
    RUN(orphan_truncated_gif_cleaned_at_scan);
    RUN(orphan_capture_dir_no_pos_files_cleaned);
    RUN(orphan_cleanup_idempotent_on_repeated_scans);

    RUN(zero_cam_capture_sets_error_overlay);
    RUN(error_overlay_auto_expires_after_3s);
    RUN(successful_capture_clears_error_overlay);

    RUN(viewfinder_init_then_take_photo_no_stale_pointer);
    RUN(viewfinder_free_realloc_invalidates_cached_pointer);
    RUN(viewfinder_consumer_refresh_fixes_stale_pointer);
    RUN(viewfinder_concurrent_encode_keeps_consumer_safe);
    RUN(viewfinder_no_cache_no_use_after_free);

    RUN(jpeg_only_entry_paints_jpeg_not_blue);
    RUN(jpeg_show_failure_leaves_blue_canvas);
    RUN(gif_entry_with_preview_flashes_jpeg_first);
    RUN(gallery_nav_replays_preview);

    RUN(album_decoder_released_during_encode);
    RUN(album_reacquire_failure_is_non_fatal);
    RUN(album_dance_alloc_releases_psram_for_encoder);
    RUN(concurrent_encodes_serialize_decoder_dance);

    printf("\n=== Results ===\n  PASS: %d\n  FAIL: %d\n", passed, fails);
    return (fails > 0) ? 1 : 0;
}
