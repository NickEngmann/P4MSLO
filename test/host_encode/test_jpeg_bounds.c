/**
 * Bound-math validation for the inverse-floor mapping used by
 * `jpeg_out_cb` (show_jpeg path) and `jpeg_crop_out_cb` (.p4ms path).
 *
 * Both callbacks are tjpgd output callbacks that downscale source-
 * resolution MCU rectangles into a smaller output canvas via
 * nearest-neighbor mapping. The forward map is a floor:
 *
 *      src_x = floor(out_x * sw / cw)
 *      src_y = floor(out_y * sh / ch)
 *
 * For each MCU rect [src_x_start..src_x_end] × [src_y_start..src_y_end]
 * the callback iterates a window of out_x / out_y and picks pixels
 * from the rect. The window bounds must be the consistent INVERSE OF
 * THE FLOOR — NOT the floor of the inverse:
 *
 *      out_x_lo = ceil(src_x_start * cw / sw)
 *               = (src_x_start * cw + sw - 1) / sw          [int div]
 *      out_x_hi = max out_x where floor(out_x * sw / cw) <= src_x_end
 *               = ((src_x_end + 1) * cw - 1) / sw            [int div]
 *
 * The earlier `floor(src_x_end * cw / sw)` formula loses output cells
 * at every MCU boundary where the source pixel that should serve the
 * cell falls exactly on the boundary. Symptom on hardware: faint
 * vertical/horizontal blue gaps every N columns/rows in the JPEG
 * preview canvas (where N = ratio bins between MCU width and canvas).
 *
 * This test:
 *   (a) Tiles the source rectangle into MCU rects (16x16 typical for
 *       4:2:2 JPEGs from OV5640).
 *   (b) Walks every MCU through the bound formula.
 *   (c) Asserts: every output cell in [0, cw)×[0, ch) is hit by
 *       EXACTLY one MCU. No gaps, no double-coverage.
 *
 * Tests both the FIXED formulas (current code) and the BROKEN
 * formulas (regression check — the fix actually does fix it).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *__asan_default_options(void) { return "detect_leaks=0"; }

#define MCU_W 16
#define MCU_H 16

/* The CURRENT (fixed) formulas. */
static int out_lo_fixed(int src_start, int co, int so) {
    return (src_start * co + so - 1) / so;
}
static int out_hi_fixed(int src_end, int co, int so) {
    return ((src_end + 1) * co - 1) / so;
}

/* The BROKEN formula (floor of inverse). */
static int out_hi_broken(int src_end, int co, int so) {
    return (src_end * co) / so;
}

static int run_one(int sw, int sh, int cw, int ch,
                   int (*hi_fn)(int, int, int),
                   const char *label,
                   int *gap_count, int *dup_count)
{
    int total = cw * ch;
    char *hits = calloc(total, 1);
    if (!hits) { perror("calloc"); return 1; }

    /* Iterate MCUs. tjpgd emits rects in MCU-aligned strides. */
    for (int my = 0; my < sh; my += MCU_H) {
        int my_end = my + MCU_H - 1;
        if (my_end >= sh) my_end = sh - 1;
        for (int mx = 0; mx < sw; mx += MCU_W) {
            int mx_end = mx + MCU_W - 1;
            if (mx_end >= sw) mx_end = sw - 1;

            int oy_lo = out_lo_fixed(my, ch, sh);
            int oy_hi = hi_fn(my_end, ch, sh);
            if (oy_lo < 0) oy_lo = 0;
            if (oy_hi >= ch) oy_hi = ch - 1;

            int ox_lo = out_lo_fixed(mx, cw, sw);
            int ox_hi = hi_fn(mx_end, cw, sw);
            if (ox_lo < 0) ox_lo = 0;
            if (ox_hi >= cw) ox_hi = cw - 1;

            for (int oy = oy_lo; oy <= oy_hi; oy++) {
                int sy = (oy * sh) / ch;
                if (sy < my || sy > my_end) continue;
                for (int ox = ox_lo; ox <= ox_hi; ox++) {
                    int sx = (ox * sw) / cw;
                    if (sx < mx || sx > mx_end) continue;
                    hits[oy * cw + ox]++;
                }
            }
        }
    }

    *gap_count = 0;
    *dup_count = 0;
    for (int i = 0; i < total; i++) {
        if (hits[i] == 0) (*gap_count)++;
        else if (hits[i] > 1) (*dup_count)++;
    }

    free(hits);
    int ok = (*gap_count == 0) && (*dup_count == 0);
    printf("    %-30s %dx%d → %dx%d : gaps=%d dups=%d %s\n",
           label, sw, sh, cw, ch, *gap_count, *dup_count,
           ok ? "OK" : "BAD");
    return ok ? 0 : 1;
}

static int gaps_at_bug_ratio(void) {
    /* 1920×1080 source → 240×240 canvas — the show_jpeg ratio that
     * was visibly buggy (faint blue gaps) on hardware before the fix. */
    int g, d;
    int rc = run_one(1920, 1080, 240, 240, out_hi_fixed, "fixed (1920×1080→240×240)", &g, &d);
    if (rc) return 1;
    int rc2 = run_one(1920, 1080, 240, 240, out_hi_broken, "broken (1920×1080→240×240)", &g, &d);
    if (rc2 == 0) {
        fprintf(stderr, "FAIL: broken formula didn't produce gaps — test invalid\n");
        return 1;
    }
    if (g == 0) {
        fprintf(stderr, "FAIL: broken formula produced no gaps — test invalid\n");
        return 1;
    }
    printf("    (broken intentionally produces %d gaps — fix's regression check works)\n", g);
    return 0;
}

static int gaps_at_p4ms_ratio(void) {
    /* 1824×1920 cropped → 240×240 canvas — the .p4ms PIMSLO ratio.
     * Same bug class (jpeg_crop_out_cb formula). */
    int g, d;
    int rc = run_one(1824, 1920, 240, 240, out_hi_fixed, "fixed (1824×1920→240×240)", &g, &d);
    if (rc) return 1;
    int g2, d2;
    int rc2 = run_one(1824, 1920, 240, 240, out_hi_broken, "broken (1824×1920→240×240)", &g2, &d2);
    if (rc2 == 0 || g2 == 0) {
        fprintf(stderr, "FAIL: broken formula didn't produce gaps for .p4ms\n");
        return 1;
    }
    return 0;
}

static int sanity_clean_ratios(void) {
    /* Clean-multiple ratios: bug doesn't manifest. Both fixed and broken
     * should produce zero gaps. Sanity check that the test framework
     * itself isn't biased. */
    int g, d, fail = 0;
    fail += run_one(1920, 1920, 240, 240, out_hi_fixed,  "fixed (1920×1920→240×240, 8:1)", &g, &d);
    fail += run_one(1920, 1920, 240, 240, out_hi_broken, "broken (1920×1920→240×240, 8:1)", &g, &d);
    return fail;
}

static int extreme_ratios(void) {
    /* Stress: very small / very large ratios. */
    int g, d, fail = 0;
    fail += run_one(2560, 1920, 240, 240, out_hi_fixed,  "fixed (2560×1920→240×240)", &g, &d);
    fail += run_one(640,  480,  240, 240, out_hi_fixed,  "fixed (640×480→240×240)",   &g, &d);
    fail += run_one(2048, 1024, 240, 240, out_hi_fixed,  "fixed (2048×1024→240×240)", &g, &d);
    fail += run_one(3840, 2160, 240, 240, out_hi_fixed,  "fixed (3840×2160→240×240)", &g, &d);
    return fail;
}

int main(void)
{
    int fails = 0;

    printf("\n## scenario: jpeg_out_cb bound math correctness\n");
    fails += gaps_at_bug_ratio();
    fails += gaps_at_p4ms_ratio();
    fails += sanity_clean_ratios();
    fails += extreme_ratios();

    if (fails) {
        printf("\n=== Results ===\n  FAIL: %d scenarios failed\n", fails);
        return 1;
    }
    printf("\n=== Results ===\n  PASS: every output cell hit exactly once at every tested ratio\n");
    return 0;
}
