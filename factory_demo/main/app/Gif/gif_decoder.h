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
 * Full-frame indices buffer (width × height bytes, allocated in open())
 * is filled from the LZW stream, then nearest-neighbor-scaled into
 * `target_rgb565`.
 *
 * @param dec             Decoder context.
 * @param target_rgb565   Output buffer (`target_w * target_h * 2` bytes).
 * @param target_w        Target (output) width in pixels.
 * @param target_h        Target (output) height in pixels.
 * @param[out] delay_cs   Frame delay in centiseconds.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND when all frames decoded.
 */
esp_err_t gif_decoder_next_frame(gif_decoder_t *dec,
                                  uint16_t *target_rgb565,
                                  int target_w, int target_h,
                                  int *delay_cs);

/* ---- Two-step API for frame-cache dedup ------------------------------
 *
 * The usage pattern is:
 *   1. call `read_next_frame` — pulls the frame's compressed LZW data
 *      off the SD card into an internal buffer, computes a 32-bit
 *      FNV-1a hash of the compressed bytes, returns that hash + the
 *      frame delay. File cursor is advanced past the frame.
 *   2. caller consults its own cache keyed by that hash.
 *       - cache hit: call `discard_read_frame` and memcpy the cached
 *         canvas into `target_rgb565` yourself. No LZW decode happens.
 *       - cache miss: call `decode_read_frame` which feeds the buffered
 *         LZW data through the normal decode + scale path into
 *         `target_rgb565`, then drops the buffer.
 *
 * For PIMSLO GIFs (reverse frames written byte-for-byte identical to
 * the forward frames), this lets a naive caller skip two full-frame
 * LZW decodes per loop at the cost of one hash per frame. */
esp_err_t gif_decoder_read_next_frame(gif_decoder_t *dec,
                                       uint32_t *hash_out,
                                       int *delay_cs_out);

esp_err_t gif_decoder_decode_read_frame(gif_decoder_t *dec,
                                         uint16_t *target_rgb565,
                                         int target_w, int target_h);

void gif_decoder_discard_read_frame(gif_decoder_t *dec);

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
