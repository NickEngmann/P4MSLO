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

/** Crop rectangle for parallax cropping during decode+scale */
typedef struct {
    int x;      /**< Left offset in source image pixels */
    int y;      /**< Top offset in source image pixels */
    int w;      /**< Width of crop region */
    int h;      /**< Height of crop region */
} gif_crop_rect_t;

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
 * @brief Pass 1: Add a frame from in-memory JPEG data with optional crop
 */
esp_err_t gif_encoder_pass1_add_frame_from_buffer(gif_encoder_t *enc,
    const uint8_t *jpeg_data, size_t jpeg_size, const gif_crop_rect_t *crop);

/**
 * @brief Pass 2: Encode a frame from in-memory JPEG data with optional crop
 */
esp_err_t gif_encoder_pass2_add_frame_from_buffer(gif_encoder_t *enc,
    const uint8_t *jpeg_data, size_t jpeg_size, const gif_crop_rect_t *crop);

/**
 * @brief Get current write position in the GIF file
 *
 * Use before/after pass2_add_frame to record frame byte ranges,
 * then use pass2_replay_frame_data to copy them for duplicate frames.
 */
long gif_encoder_get_file_pos(gif_encoder_t *enc);

/**
 * @brief Replay previously-written frame data from the GIF file
 *
 * Copies bytes from [src_offset, src_offset+length) to the current
 * write position. Used for oscillating GIF sequences where reverse
 * frames are identical to forward frames — avoids re-decoding and
 * re-encoding, saving ~5 seconds per duplicated frame.
 *
 * @param enc         Encoder context
 * @param src_offset  Byte offset in the GIF file where the frame starts
 * @param length      Number of bytes to copy
 * @return ESP_OK on success
 */
esp_err_t gif_encoder_pass2_replay_frame(gif_encoder_t *enc,
                                          long src_offset, size_t length);

/**
 * @brief Write raw frame data directly to the GIF file from a memory buffer
 *
 * Used for PSRAM-cached frame replay. The buffer must contain a complete
 * GIF frame (GCE + Image Descriptor + LZW data + terminator).
 */
esp_err_t gif_encoder_pass2_write_raw_frame(gif_encoder_t *enc,
                                             const void *data, size_t length);

/**
 * @brief Read back frame data from the GIF file into a buffer
 *
 * Reads bytes from [offset, offset+length) without moving the write cursor.
 */
esp_err_t gif_encoder_read_back(gif_encoder_t *enc, long offset, void *buf, size_t length);

/**
 * @brief Destroy encoder and free all resources
 */
void gif_encoder_destroy(gif_encoder_t *enc);

#ifdef __cplusplus
}
#endif
