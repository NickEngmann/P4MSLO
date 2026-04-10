/**
 * @file gif_lzw.h
 * @brief GIF LZW compression and decompression
 *
 * Implements the standard GIF variable-length-code LZW algorithm with
 * 4096-entry dictionary, outputting 255-byte sub-blocks as the GIF spec
 * requires.
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- LZW Encoder ---- */

typedef struct gif_lzw_enc gif_lzw_enc_t;

/**
 * @brief Create an LZW encoder
 * @param min_code_size  Minimum LZW code size (typically 8 for 256 colors)
 * @param output         Output FILE* to write sub-blocks to
 * @param[out] out       Pointer to receive encoder
 */
esp_err_t gif_lzw_enc_create(int min_code_size, FILE *output, gif_lzw_enc_t **out);

/**
 * @brief Encode a single pixel index
 */
esp_err_t gif_lzw_enc_pixel(gif_lzw_enc_t *enc, uint8_t index);

/**
 * @brief Finish encoding — flushes remaining data and writes block terminator
 */
esp_err_t gif_lzw_enc_finish(gif_lzw_enc_t *enc);

/**
 * @brief Free encoder
 */
void gif_lzw_enc_destroy(gif_lzw_enc_t *enc);

/* ---- LZW Decoder ---- */

typedef struct gif_lzw_dec gif_lzw_dec_t;

/**
 * @brief Create an LZW decoder
 * @param min_code_size  Minimum code size from GIF image data
 * @param[out] out       Pointer to receive decoder
 */
esp_err_t gif_lzw_dec_create(int min_code_size, gif_lzw_dec_t **out);

/**
 * @brief Feed compressed sub-block data into the decoder
 * @param dec   Decoder context
 * @param data  Sub-block data (up to 255 bytes)
 * @param len   Length of data
 * @param out_pixels  Output buffer for decoded pixel indices
 * @param out_cap     Capacity of output buffer
 * @param[out] out_len Number of pixels actually decoded
 */
esp_err_t gif_lzw_dec_feed(gif_lzw_dec_t *dec, const uint8_t *data, int len,
                           uint8_t *out_pixels, int out_cap, int *out_len);

/**
 * @brief Free decoder
 */
void gif_lzw_dec_destroy(gif_lzw_dec_t *dec);

#ifdef __cplusplus
}
#endif
