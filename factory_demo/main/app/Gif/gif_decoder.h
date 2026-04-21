/**
 * @file gif_decoder.h
 * @brief Streaming GIF decoder for playback on ESP32-P4
 *
 * Reads one frame at a time from SD card, decodes LZW data,
 * and outputs RGB565 pixels for direct LVGL canvas rendering.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "gif_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gif_decoder gif_decoder_t;

/**
 * @brief Open a GIF file for playback
 * @param path    Full path to GIF file
 * @param[out] out  Pointer to receive decoder
 */
esp_err_t gif_decoder_open(const char *path, gif_decoder_t **out);

/** @brief Get GIF width */
int gif_decoder_get_width(gif_decoder_t *dec);

/** @brief Get GIF height */
int gif_decoder_get_height(gif_decoder_t *dec);

/**
 * @brief Decode the next frame directly into a scaled RGB565 buffer.
 *
 * The decoder feeds its LZW output into a 1-row-wide scanline scratch
 * owned by the decoder (~src_w bytes), then nearest-neighbor scales each
 * source row that maps to a new output row into `target_rgb565`. It never
 * materializes the full-resolution RGB565 intermediate, so peak memory
 * is bounded regardless of GIF dimensions.
 *
 * @param dec             Decoder context.
 * @param target_rgb565   Output buffer (`target_w * target_h * 2` bytes).
 *                        Typically this is the LCD canvas (240×240×2).
 * @param target_w        Target (output) width in pixels.
 * @param target_h        Target (output) height in pixels.
 * @param[out] delay_cs   Frame delay in centiseconds.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND when all frames decoded.
 */
esp_err_t gif_decoder_next_frame(gif_decoder_t *dec,
                                  uint16_t *target_rgb565,
                                  int target_w, int target_h,
                                  int *delay_cs);

/**
 * @brief Reset to the first frame (seek to beginning)
 */
esp_err_t gif_decoder_reset(gif_decoder_t *dec);

/**
 * @brief Close the decoder and free resources
 */
void gif_decoder_close(gif_decoder_t *dec);

#ifdef __cplusplus
}
#endif
