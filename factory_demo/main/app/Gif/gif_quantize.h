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
 * @brief Free quantizer context
 */
void gif_quantize_destroy(gif_quantize_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
