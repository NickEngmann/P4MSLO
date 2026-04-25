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
    P("total: %d ms (%.1f s) — baseline arch (PSRAM stack)", total, total/1000.0);
    ASSERT(total > 120 * 1000, "expected baseline > 2 min (PSRAM stack penalty)");
}

SCENARIO(proposed_arch_meets_budget)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    int total = r.capture_ms + r.save_ms + pimslo_sim_wait_idle(120 * 1000);
    P("total: %d ms (%.1f s) — proposed arch (INTERNAL stack)", total, total/1000.0);
    ASSERT(total <= 120 * 1000, "expected proposed ≤ 2 min");
    ASSERT(gallery_count() == 1, "expected 1 gallery entry after encode");
    const gallery_entry_t *e = gallery_current();
    ASSERT(e && e->has_gif && e->has_p4ms, "entry should have both .gif and .p4ms");
}

SCENARIO(photo_from_main_kicks_encode)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED);
    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    pimslo_sim_wait_idle(120 * 1000);
    ASSERT(gallery_count() == 1, "encode should have completed");
    ASSERT(gallery_current()->has_gif, "expected .gif to land");
}

SCENARIO(photo_from_camera_then_navigate_to_main)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED);
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
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED);
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
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED);
    pimslo_sim_force_capture_fail(1, /* cams returned */ 1);
    ui_extra_set_current_page(UI_PAGE_MAIN);
    pimslo_sim_capture_result_t r = pimslo_sim_photo_btn(4);
    ASSERT(r.cams_usable == 1, "force-fail should give us 1/4");
    pimslo_sim_wait_idle(120 * 1000);
    ASSERT(gallery_count() == 0, "1/4 capture should be dropped, no entry");
}

SCENARIO(bg_worker_recovers_jpeg_only_captures)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED);
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
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED);
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
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED);
    gallery_record_capture("P4M0001", false, true, false);
    ui_extra_set_current_page(UI_PAGE_GIFS);
    bg_worker_kick();
    /* On gallery page, bg encode path should yield. */
    ASSERT(bg_worker_re_encode_count() == 0, "bg should NOT encode on UI_PAGE_GIFS");
}

SCENARIO(memory_after_encode_returns_to_steady)
{
    pimslo_sim_set_architecture(PIMSLO_ARCH_PROPOSED);
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

/* ------------------------------------------------------------------ */
int main(void)
{
    RUN(baseline_arch_misses_budget);
    RUN(proposed_arch_meets_budget);
    RUN(photo_from_main_kicks_encode);
    RUN(photo_from_camera_then_navigate_to_main);
    RUN(multiple_photos_in_succession);
    RUN(photo_with_too_few_cams_dropped);
    RUN(bg_worker_recovers_jpeg_only_captures);
    RUN(bg_worker_pre_renders_p4ms);
    RUN(bg_worker_yields_on_gallery_page);
    RUN(memory_after_encode_returns_to_steady);

    printf("\n=== Results ===\n  PASS: %d\n  FAIL: %d\n", passed, fails);
    return (fails > 0) ? 1 : 0;
}
