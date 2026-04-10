/**
 * @file gif_quantize.c
 * @brief Median-cut color quantization for GIF encoding
 *
 * Uses a 5-bit-per-channel color cube (32x32x32 = 32768 entries) to
 * histogram pixel colors, then applies median-cut to produce a 256-color
 * palette.  Designed for PSRAM allocation on ESP32-P4.
 */

#include "gif_quantize.h"
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "gif_quant";

/* 5-bit color cube: 32x32x32 = 32768 bins */
#define CUBE_BITS  5
#define CUBE_SIZE  (1 << CUBE_BITS)  /* 32 */
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

    /* JPEG decoder outputs BGR565: bits [15:11]=B, [10:5]=G, [4:0]=R.
     * PPA preserves this order. Extract correctly. */
    int total = width * height;
    for (int i = 0; i < total; i += subsample) {
        uint16_t px = rgb565[i];
        uint8_t b5 = (px >> 11) & 0x1F;
        uint8_t g5 = ((px >> 5) & 0x3F) >> 1;  /* 6-bit green → 5-bit */
        uint8_t r5 =  px & 0x1F;
        ctx->histogram[CUBE_IDX(r5, g5, b5)]++;
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
            uint8_t r5 = (uint8_t)(boxes[i].r_sum / boxes[i].pixel_count);
            uint8_t g5 = (uint8_t)(boxes[i].g_sum / boxes[i].pixel_count);
            uint8_t b5 = (uint8_t)(boxes[i].b_sum / boxes[i].pixel_count);
            /* Scale 5-bit values to 8-bit */
            palette->entries[i].r = (r5 << 3) | (r5 >> 2);
            palette->entries[i].g = (g5 << 3) | (g5 >> 2);
            palette->entries[i].b = (b5 << 3) | (b5 >> 2);
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

void gif_quantize_destroy(gif_quantize_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->histogram) heap_caps_free(ctx->histogram);
    free(ctx);
}
