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
 * @brief Decode the next frame into an RGB565 buffer
 * @param dec       Decoder context
 * @param rgb565    Output buffer (must be width*height*2 bytes)
 * @param[out] delay_cs  Frame delay in centiseconds
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND when all frames decoded
 */
esp_err_t gif_decoder_next_frame(gif_decoder_t *dec, uint16_t *rgb565, int *delay_cs);

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
