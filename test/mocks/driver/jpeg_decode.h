/**
 * @brief ESP-IDF jpeg_decode mock — host-side stubs.
 *
 * The actual P4MSLO encode pipeline doesn't use the HW JPEG decoder
 * (it's all tjpgd software, see gif_encoder.c::decode_and_scale_jpeg).
 * `enc->jpeg_handle` is set to NULL at create time and never used,
 * but we still need the type symbols to compile.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

typedef void *jpeg_decoder_handle_t;

typedef struct {
    int timeout_ms;
} jpeg_decode_engine_cfg_t;

typedef struct {
    int buffer_direction;
} jpeg_decode_memory_alloc_cfg_t;

#define JPEG_DEC_ALLOC_OUTPUT_BUFFER 0

typedef struct {
    int width;
    int height;
} jpeg_decode_picture_info_t;

static inline esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *cfg, jpeg_decoder_handle_t *out) {
    (void)cfg; *out = NULL; return ESP_OK;
}
static inline esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t h) { (void)h; return ESP_OK; }
static inline void *jpeg_alloc_decoder_mem(size_t sz, jpeg_decode_memory_alloc_cfg_t *cfg, size_t *out_sz) {
    (void)cfg; if (out_sz) *out_sz = sz; return malloc(sz);
}
static inline esp_err_t jpeg_decoder_get_info(const void *data, size_t len, jpeg_decode_picture_info_t *info) {
    (void)data; (void)len; if (info) { info->width = 0; info->height = 0; } return ESP_OK;
}
