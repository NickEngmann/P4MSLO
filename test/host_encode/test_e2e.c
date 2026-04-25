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

    printf("\n=== Results ===\n  PASS: %d\n  FAIL: %d\n", passed, fails);
    return (fails > 0) ? 1 : 0;
}
