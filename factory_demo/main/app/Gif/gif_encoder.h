/**
 * @file gif_encoder.h
 * @brief Streaming GIF89a encoder for ESP32-P4
 *
 * Two-pass encoding that keeps only one decoded frame in memory at a time:
 *   Pass 1: Build a shared 256-color palette from all frames (subsampled)
 *   Pass 2: Encode each frame against the palette and stream to SD card
 *
 * Designed for PSRAM-backed operation on memory-constrained devices.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "gif_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int frame_delay_cs;     /**< Frame delay in centiseconds (default 50 = 500ms) */
    int loop_count;         /**< 0 = infinite loop */
    int target_width;       /**< Target GIF width (0 = auto from first frame) */
    int target_height;      /**< Target GIF height (0 = auto from first frame) */
} gif_encoder_config_t;

typedef struct gif_encoder gif_encoder_t;

/** Progress callback — called after each frame is processed */
typedef void (*gif_encoder_progress_cb_t)(int current_frame, int total_frames,
                                          int pass, void *user_data);

/**
 * @brief Create a GIF encoder
 * @param config  Encoder configuration
 * @param[out] out  Pointer to receive encoder
 */
esp_err_t gif_encoder_create(const gif_encoder_config_t *config, gif_encoder_t **out);

/**
 * @brief Set progress callback
 */
void gif_encoder_set_progress_cb(gif_encoder_t *enc, gif_encoder_progress_cb_t cb, void *user_data);

/**
 * @brief Pass 1: Add a frame's colors to the palette builder
 *
 * Reads the JPEG, decodes it, scales to target size, and subsamples pixels
 * for palette construction.  Frame memory is freed before returning.
 *
 * @param enc        Encoder context
 * @param jpeg_path  Full path to JPEG file on SD card
 */
esp_err_t gif_encoder_pass1_add_frame(gif_encoder_t *enc, const char *jpeg_path);

/**
 * @brief Pass 1: Finalize the palette
 */
esp_err_t gif_encoder_pass1_finalize(gif_encoder_t *enc);

/**
 * @brief Pass 2: Begin writing the GIF file
 * @param enc          Encoder context
 * @param output_path  Full path for the output GIF file
 */
esp_err_t gif_encoder_pass2_begin(gif_encoder_t *enc, const char *output_path);

/**
 * @brief Pass 2: Encode and write one frame
 * @param enc        Encoder context
 * @param jpeg_path  Full path to JPEG file on SD card
 */
esp_err_t gif_encoder_pass2_add_frame(gif_encoder_t *enc, const char *jpeg_path);

/**
 * @brief Pass 2: Finish the GIF file (writes trailer, closes file)
 */
esp_err_t gif_encoder_pass2_finish(gif_encoder_t *enc);

/**
 * @brief Destroy encoder and free all resources
 */
void gif_encoder_destroy(gif_encoder_t *enc);

#ifdef __cplusplus
}
#endif
