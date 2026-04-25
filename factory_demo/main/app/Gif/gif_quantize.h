/**
 * @file gif_quantize.h
 * @brief Color quantization for GIF encoding (median-cut algorithm)
 *
 * Two-phase usage:
 *   1. Accumulate colors from multiple images via gif_quantize_accumulate()
 *   2. Build a 256-color palette via gif_quantize_build_palette()
 *   3. Map individual pixels to palette indices via gif_quantize_map_pixel()
 */
#pragma once

#include "gif_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque quantizer context */
typedef struct gif_quantize_ctx gif_quantize_ctx_t;

/**
 * @brief Create a quantizer context
 * @param[out] out  Pointer to receive the context
 * @return ESP_OK on success
 */
esp_err_t gif_quantize_create(gif_quantize_ctx_t **out);

/**
 * @brief Accumulate pixel colors from an RGB565 image
 *
 * Subsamples every Nth pixel to keep histogram cheap.
 *
 * @param ctx       Quantizer context
 * @param rgb565    Image data in RGB565 format
 * @param width     Image width
 * @param height    Image height
 * @param subsample Sample every Nth pixel (e.g. 4)
 */
esp_err_t gif_quantize_accumulate_rgb565(gif_quantize_ctx_t *ctx,
                                         const uint16_t *rgb565,
                                         int width, int height,
                                         int subsample);

/**
 * @brief Build the final 256-color palette from accumulated colors
 * @param ctx       Quantizer context
 * @param palette   Output palette
 */
esp_err_t gif_quantize_build_palette(gif_quantize_ctx_t *ctx, gif_palette_t *palette);

/**
 * @brief Map an RGB888 pixel to the nearest palette index
 * @param palette  The palette to map against
 * @param r        Red component (0-255)
 * @param g        Green component (0-255)
 * @param b        Blue component (0-255)
 * @return Palette index (0-255)
 */
uint8_t gif_quantize_map_pixel(const gif_palette_t *palette, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Build a lookup table mapping every RGB565 value to a palette index
 *
 * Precomputes the nearest palette entry for all 65536 possible RGB565 values.
 * After building, use gif_quantize_lut_map() for O(1) pixel mapping.
 *
 * @param palette  The palette to build the LUT against
 * @param[out] lut Output array of 65536 uint8_t entries (allocated by caller, 64KB)
 */
void gif_quantize_build_lut(const gif_palette_t *palette, uint8_t *lut);

/**
 * @brief Map an RGB565 pixel to palette index via precomputed LUT (O(1))
 */
static inline uint8_t gif_quantize_lut_map(const uint8_t *lut, uint16_t rgb565)
{
    return lut[rgb565];
}

/**
 * @brief Build a 4 KB R4-G4-B4 LUT for the TCM-resident hot path
 *
 * 12-bit address: top 4 bits of red, top 4 bits of green, top 4 bits
 * of blue. 4096 palette indices = 4 KB. Single indirection in TCM
 * (~3 ns) vs 64 KB LUT in PSRAM (~80 ns) ≈ 20× speedup on Pass 2.
 *
 * 4 KB (not 8 KB) because ~2.8 KB of TCM is already in use by
 * pmu_init.c and friends — only ~5.4 KB remains for user data. The
 * earlier 8 KB R4G5B4 attempt overflowed the tcm_idram_seg region by
 * 2816 bytes; back-off to 4 KB.
 *
 * Quality: 1 LSB less precision on R and B vs RGB565, 2 bits less on
 * green. Floyd-Steinberg dithering propagates errors larger than these
 * LSB drops, so visual difference is below the encoder's noise floor.
 *
 * @param palette  The 256-color palette to map against
 * @param[out] lut Output array of 4096 uint8_t (caller-allocated, must
 *                 live in TCM via TCM_DRAM_ATTR for the speedup to land)
 */
void gif_quantize_build_lut12(const gif_palette_t *palette, uint8_t *lut);

/**
 * @brief Map an RGB565 pixel to a palette index via the 12-bit TCM LUT
 *
 * Address layout: bits 11..8 = R top4, bits 7..4 = G top4, bits 3..0 = B top4
 */
static inline uint8_t gif_quantize_lut12_map(const uint8_t *lut, uint16_t rgb565)
{
    int r5 = (rgb565 >> 11) & 0x1F;
    int g6 = (rgb565 >> 5) & 0x3F;
    int b5 = rgb565 & 0x1F;
    int r4 = r5 >> 1;
    int g4 = g6 >> 2;
    int b4 = b5 >> 1;
    return lut[(r4 << 8) | (g4 << 4) | b4];
}

/**
 * @brief Free quantizer context
 */
void gif_quantize_destroy(gif_quantize_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
