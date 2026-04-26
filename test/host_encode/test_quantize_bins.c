/**
 * Histogram-bin-uniqueness check for the gif_quantize CUBE_IDX macro.
 *
 * The 6-bit color cube has 64×64×64 = 262144 bins. For a clean
 * spatial hash, every (r,g,b) triple in [0,63]³ must map to a UNIQUE
 * index in [0, 262144).
 *
 * The previous formula `(r << 10) | (g << 5) | b` uses 5-bit spacing
 * (bits 0-4 for b, 5-9 for g, 10-14 for r), so bit 10 is shared
 * between r's LSB and g's MSB. Many (r,g,b) triples collide.
 * Concrete example: (r=0, g=32, b=0) and (r=1, g=32, b=0) both →
 * index 1024.
 *
 * The fixed formula is `(r << 12) | (g << 6) | b` — 6-bit spacing
 * for 6-bit channels, no overlap.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *__asan_default_options(void) { return "detect_leaks=0"; }

#define CUBE_SIZE 64
#define CUBE_TOTAL (CUBE_SIZE * CUBE_SIZE * CUBE_SIZE)

/* Broken (current code as of 2026-04-25 pre-fix). */
static int idx_broken(int r, int g, int b)
{
    return (r << 10) | (g << 5) | b;
}

/* Fixed: 6-bit spacing for 6-bit channels. */
static int idx_fixed(int r, int g, int b)
{
    return (r << 12) | (g << 6) | b;
}

static int run_uniqueness(int (*idx_fn)(int, int, int), const char *label)
{
    int *seen = calloc(CUBE_TOTAL * 4, sizeof(int));  /* 4× to absorb out-of-range hits */
    if (!seen) { perror("calloc"); return 1; }
    int collisions = 0;
    int out_of_range = 0;
    int max_idx = 0;

    for (int r = 0; r < CUBE_SIZE; r++) {
        for (int g = 0; g < CUBE_SIZE; g++) {
            for (int b = 0; b < CUBE_SIZE; b++) {
                int i = idx_fn(r, g, b);
                if (i < 0 || i >= CUBE_TOTAL * 4) { out_of_range++; continue; }
                if (i > max_idx) max_idx = i;
                if (seen[i]) collisions++;
                seen[i]++;
            }
        }
    }
    free(seen);

    int unique = (CUBE_SIZE * CUBE_SIZE * CUBE_SIZE) - collisions;
    int ok = (collisions == 0 && max_idx < CUBE_TOTAL && out_of_range == 0);
    printf("    %-12s collisions=%d max_idx=%d unique=%d (target %d) %s\n",
           label, collisions, max_idx, unique, CUBE_TOTAL,
           ok ? "OK" : "BAD");
    return ok ? 0 : 1;
}

int main(void)
{
    int fails = 0;

    printf("\n## scenario: gif_quantize CUBE_IDX bin-uniqueness\n");

    /* Negative test: prove the broken formula has collisions. */
    int rc = run_uniqueness(idx_broken, "broken");
    if (rc == 0) {
        fprintf(stderr, "FAIL: broken formula didn't collide — test is invalid\n");
        return 1;
    }

    /* Positive test: fixed formula has zero collisions. */
    fails += run_uniqueness(idx_fixed, "fixed");

    /* Specific regression case from the audit. */
    int collision_r0 = idx_broken(0, 32, 0);
    int collision_r1 = idx_broken(1, 32, 0);
    if (collision_r0 != collision_r1) {
        fprintf(stderr, "FAIL: known collision (r=0/1, g=32, b=0) didn't reproduce — "
                        "broken formula may have changed\n");
        return 1;
    }
    printf("    confirmed: broken (r=0,g=32,b=0)→%d and (r=1,g=32,b=0)→%d collide\n",
           collision_r0, collision_r1);

    int fixed_r0 = idx_fixed(0, 32, 0);
    int fixed_r1 = idx_fixed(1, 32, 0);
    if (fixed_r0 == fixed_r1) {
        fprintf(stderr, "FAIL: fixed formula still collides on the regression case\n");
        return 1;
    }
    printf("    fixed:    (r=0,g=32,b=0)→%d and (r=1,g=32,b=0)→%d distinct\n",
           fixed_r0, fixed_r1);

    if (fails) {
        printf("\n=== Results ===\n  FAIL\n");
        return 1;
    }
    printf("\n=== Results ===\n  PASS: every (r,g,b) maps to a unique bin\n");
    return 0;
}
