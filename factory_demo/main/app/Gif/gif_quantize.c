/**
 * @file gif_quantize.c
 * @brief Median-cut color quantization for GIF encoding
 *
 * Uses a 6-bit-per-channel color cube (64x64x64 = 262144 entries, 1MB) to
 * histogram pixel colors, then applies median-cut to produce a 256-color
 * palette.  Designed for PSRAM allocation on ESP32-P4.
 */

#include "gif_quantize.h"
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "gif_quant";

/* 6-bit color cube: 64x64x64 = 262144 bins (1MB in PSRAM) */
#define CUBE_BITS  6
#define CUBE_SIZE  (1 << CUBE_BITS)  /* 64 */
#define CUBE_TOTAL (CUBE_SIZE * CUBE_SIZE * CUBE_SIZE)  /* 32768 */

#define CUBE_IDX(r5, g5, b5) (((r5) << 10) | ((g5) << 5) | (b5))

struct gif_quantize_ctx {
    uint32_t *histogram;  /* CUBE_TOTAL entries, allocated in PSRAM */
};

/* ---- Median-cut types ---- */
typedef struct {
    uint8_t r_min, r_max;
    uint8_t g_min, g_max;
    uint8_t b_min, b_max;
    uint32_t pixel_count;
    uint64_t r_sum, g_sum, b_sum;
} color_box_t;

/* ---- Helpers ---- */

/* Not used externally — only the accumulate function below */

/* ---- Public API ---- */

esp_err_t gif_quantize_create(gif_quantize_ctx_t **out)
{
    gif_quantize_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return ESP_ERR_NO_MEM;

    ctx->histogram = heap_caps_calloc(CUBE_TOTAL, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!ctx->histogram) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    *out = ctx;
    return ESP_OK;
}

esp_err_t gif_quantize_accumulate_rgb565(gif_quantize_ctx_t *ctx,
                                         const uint16_t *rgb565,
                                         int width, int height,
                                         int subsample)
{
    if (!ctx || !rgb565) return ESP_ERR_INVALID_ARG;
    if (subsample < 1) subsample = 1;

    /* Standard RGB565: R5 in [15:11], G6 in [10:5], B5 in [4:0].
     * Expand R and B from 5-bit to 6-bit for the 6-bit cube.
     * Green is natively 6-bit in RGB565. */
    int total = width * height;
    for (int i = 0; i < total; i += subsample) {
        uint16_t px = rgb565[i];
        uint8_t r6 = ((px >> 11) & 0x1F) << 1;  /* 5-bit R → 6-bit */
        uint8_t g6 =  (px >> 5)  & 0x3F;         /* native 6-bit G */
        uint8_t b6 = ( px        & 0x1F) << 1;   /* 5-bit B → 6-bit */
        ctx->histogram[CUBE_IDX(r6, g6, b6)]++;
    }
    return ESP_OK;
}

/* Scan histogram for a color box and compute statistics */
static void compute_box_stats(const uint32_t *hist, color_box_t *box)
{
    box->pixel_count = 0;
    box->r_sum = box->g_sum = box->b_sum = 0;
    box->r_min = box->g_min = box->b_min = CUBE_SIZE - 1;
    box->r_max = box->g_max = box->b_max = 0;

    for (uint8_t r = 0; r < CUBE_SIZE; r++) {
        for (uint8_t g = 0; g < CUBE_SIZE; g++) {
            for (uint8_t b = 0; b < CUBE_SIZE; b++) {
                uint32_t cnt = hist[CUBE_IDX(r, g, b)];
                if (cnt == 0) continue;
                box->pixel_count += cnt;
                box->r_sum += (uint64_t)r * cnt;
                box->g_sum += (uint64_t)g * cnt;
                box->b_sum += (uint64_t)b * cnt;
                if (r < box->r_min) box->r_min = r;
                if (r > box->r_max) box->r_max = r;
                if (g < box->g_min) box->g_min = g;
                if (g > box->g_max) box->g_max = g;
                if (b < box->b_min) box->b_min = b;
                if (b > box->b_max) box->b_max = b;
            }
        }
    }
}

/* Scan histogram within a box's bounds and compute stats */
static void compute_box_stats_bounded(const uint32_t *hist, color_box_t *box,
                                      uint8_t r_lo, uint8_t r_hi,
                                      uint8_t g_lo, uint8_t g_hi,
                                      uint8_t b_lo, uint8_t b_hi)
{
    box->pixel_count = 0;
    box->r_sum = box->g_sum = box->b_sum = 0;
    box->r_min = box->g_min = box->b_min = CUBE_SIZE - 1;
    box->r_max = box->g_max = box->b_max = 0;

    for (uint8_t r = r_lo; r <= r_hi; r++) {
        for (uint8_t g = g_lo; g <= g_hi; g++) {
            for (uint8_t b = b_lo; b <= b_hi; b++) {
                uint32_t cnt = hist[CUBE_IDX(r, g, b)];
                if (cnt == 0) continue;
                box->pixel_count += cnt;
                box->r_sum += (uint64_t)r * cnt;
                box->g_sum += (uint64_t)g * cnt;
                box->b_sum += (uint64_t)b * cnt;
                if (r < box->r_min) box->r_min = r;
                if (r > box->r_max) box->r_max = r;
                if (g < box->g_min) box->g_min = g;
                if (g > box->g_max) box->g_max = g;
                if (b < box->b_min) box->b_min = b;
                if (b > box->b_max) box->b_max = b;
            }
        }
    }
}

esp_err_t gif_quantize_build_palette(gif_quantize_ctx_t *ctx, gif_palette_t *palette)
{
    if (!ctx || !palette) return ESP_ERR_INVALID_ARG;

    /* Median-cut: start with one box covering all colors, split into 256 */
    #define MAX_BOXES 256
    color_box_t boxes[MAX_BOXES];
    int num_boxes = 1;

    /* Initial box covers the full cube */
    boxes[0].r_min = boxes[0].g_min = boxes[0].b_min = 0;
    boxes[0].r_max = boxes[0].g_max = boxes[0].b_max = CUBE_SIZE - 1;
    compute_box_stats_bounded(ctx->histogram, &boxes[0],
                              0, CUBE_SIZE - 1, 0, CUBE_SIZE - 1, 0, CUBE_SIZE - 1);

    /* Repeatedly split the box with the largest range */
    while (num_boxes < MAX_BOXES) {
        /* Find box with largest range to split */
        int best = -1;
        int best_range = 0;
        for (int i = 0; i < num_boxes; i++) {
            if (boxes[i].pixel_count < 2) continue;
            int rr = boxes[i].r_max - boxes[i].r_min;
            int gr = boxes[i].g_max - boxes[i].g_min;
            int br = boxes[i].b_max - boxes[i].b_min;
            int range = rr;
            if (gr > range) range = gr;
            if (br > range) range = br;
            if (range > best_range) {
                best_range = range;
                best = i;
            }
        }
        if (best < 0) break;  /* No splittable boxes left */

        color_box_t *box = &boxes[best];

        /* Find axis with largest range */
        int rr = box->r_max - box->r_min;
        int gr = box->g_max - box->g_min;
        int br = box->b_max - box->b_min;

        /* Save original bounds before splitting (splitting overwrites box) */
        uint8_t orig_r_min = box->r_min, orig_r_max = box->r_max;
        uint8_t orig_g_min = box->g_min, orig_g_max = box->g_max;
        uint8_t orig_b_min = box->b_min, orig_b_max = box->b_max;

        /* Split at the median of the longest axis */
        uint8_t split;
        color_box_t new_box;

        if (rr >= gr && rr >= br) {
            split = (uint8_t)(box->r_sum / box->pixel_count);
            if (split < orig_r_min) split = orig_r_min;
            if (split >= orig_r_max) split = orig_r_max - 1;
            /* Lower half */
            compute_box_stats_bounded(ctx->histogram, box,
                                      orig_r_min, split,
                                      orig_g_min, orig_g_max,
                                      orig_b_min, orig_b_max);
            /* Upper half */
            compute_box_stats_bounded(ctx->histogram, &new_box,
                                      split + 1, orig_r_max,
                                      orig_g_min, orig_g_max,
                                      orig_b_min, orig_b_max);
        } else if (gr >= br) {
            split = (uint8_t)(box->g_sum / box->pixel_count);
            if (split < orig_g_min) split = orig_g_min;
            if (split >= orig_g_max) split = orig_g_max - 1;
            compute_box_stats_bounded(ctx->histogram, box,
                                      orig_r_min, orig_r_max,
                                      orig_g_min, split,
                                      orig_b_min, orig_b_max);
            compute_box_stats_bounded(ctx->histogram, &new_box,
                                      orig_r_min, orig_r_max,
                                      split + 1, orig_g_max,
                                      orig_b_min, orig_b_max);
        } else {
            split = (uint8_t)(box->b_sum / box->pixel_count);
            if (split < orig_b_min) split = orig_b_min;
            if (split >= orig_b_max) split = orig_b_max - 1;
            compute_box_stats_bounded(ctx->histogram, box,
                                      orig_r_min, orig_r_max,
                                      orig_g_min, orig_g_max,
                                      orig_b_min, split);
            compute_box_stats_bounded(ctx->histogram, &new_box,
                                      orig_r_min, orig_r_max,
                                      orig_g_min, orig_g_max,
                                      split + 1, orig_b_max);
        }

        if (new_box.pixel_count > 0) {
            boxes[num_boxes++] = new_box;
        }
    }

    /* Convert boxes to palette: average color of each box */
    memset(palette, 0, sizeof(*palette));
    palette->count = num_boxes;

    for (int i = 0; i < num_boxes; i++) {
        if (boxes[i].pixel_count > 0) {
            uint8_t r6 = (uint8_t)(boxes[i].r_sum / boxes[i].pixel_count);
            uint8_t g6 = (uint8_t)(boxes[i].g_sum / boxes[i].pixel_count);
            uint8_t b6 = (uint8_t)(boxes[i].b_sum / boxes[i].pixel_count);
            /* Scale 6-bit values to 8-bit */
            palette->entries[i].r = (r6 << 2) | (r6 >> 4);
            palette->entries[i].g = (g6 << 2) | (g6 >> 4);
            palette->entries[i].b = (b6 << 2) | (b6 >> 4);
        }
    }

    ESP_LOGI(TAG, "Built palette with %d colors", palette->count);
    return ESP_OK;
}

uint8_t gif_quantize_map_pixel(const gif_palette_t *palette, uint8_t r, uint8_t g, uint8_t b)
{
    int best_idx = 0;
    int best_dist = INT32_MAX;

    for (int i = 0; i < palette->count; i++) {
        int dr = (int)r - palette->entries[i].r;
        int dg = (int)g - palette->entries[i].g;
        int db = (int)b - palette->entries[i].b;
        int dist = dr * dr + dg * dg + db * db;
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
            if (dist == 0) break;
        }
    }
    return (uint8_t)best_idx;
}

void gif_quantize_build_lut(const gif_palette_t *palette, uint8_t *lut)
{
    ESP_LOGI(TAG, "Building RGB565 → palette LUT (65536 entries)...");

    /* Precompute palette in int for fast distance calc */
    int pr[256], pg[256], pb[256];
    for (int i = 0; i < palette->count; i++) {
        pr[i] = palette->entries[i].r;
        pg[i] = palette->entries[i].g;
        pb[i] = palette->entries[i].b;
    }
    int count = palette->count;

    for (uint32_t px = 0; px < 65536; px++) {
        /* RGB565 → RGB888 */
        int r5 = (px >> 11) & 0x1F;
        int g6 = (px >> 5) & 0x3F;
        int b5 = px & 0x1F;
        int r = (r5 << 3) | (r5 >> 2);
        int g = (g6 << 2) | (g6 >> 4);
        int b = (b5 << 3) | (b5 >> 2);

        int best_idx = 0;
        int best_dist = INT32_MAX;
        for (int i = 0; i < count; i++) {
            int dr = r - pr[i];
            int dg = g - pg[i];
            int db = b - pb[i];
            int dist = dr * dr + dg * dg + db * db;
            if (dist < best_dist) {
                best_dist = dist;
                best_idx = i;
                if (dist == 0) break;
            }
        }
        lut[px] = (uint8_t)best_idx;
    }

    ESP_LOGI(TAG, "LUT built");
}

void gif_quantize_build_lut12(const gif_palette_t *palette, uint8_t *lut)
{
    ESP_LOGI(TAG, "Building R4G4B4 → palette LUT (4096 entries, TCM)...");

    int pr[256], pg[256], pb[256];
    for (int i = 0; i < palette->count; i++) {
        pr[i] = palette->entries[i].r;
        pg[i] = palette->entries[i].g;
        pb[i] = palette->entries[i].b;
    }
    int count = palette->count;

    for (uint32_t addr = 0; addr < 4096; addr++) {
        int r4 = (addr >> 8) & 0x0F;
        int g4 = (addr >> 4) & 0x0F;
        int b4 = addr & 0x0F;
        int r = (r4 << 4) | r4;
        int g = (g4 << 4) | g4;
        int b = (b4 << 4) | b4;

        int best_idx = 0;
        int best_dist = INT32_MAX;
        for (int i = 0; i < count; i++) {
            int dr = r - pr[i];
            int dg = g - pg[i];
            int db = b - pb[i];
            int dist = dr * dr + dg * dg + db * db;
            if (dist < best_dist) {
                best_dist = dist;
                best_idx = i;
                if (dist == 0) break;
            }
        }
        lut[addr] = (uint8_t)best_idx;
    }

    ESP_LOGI(TAG, "12-bit LUT built");
}

void gif_quantize_destroy(gif_quantize_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->histogram) heap_caps_free(ctx->histogram);
    free(ctx);
}
