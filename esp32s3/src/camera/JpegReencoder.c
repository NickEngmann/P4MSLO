/**
 * @file      JpegReencoder.c
 * @brief     Encode raw YUV422 to 4:2:0 JPEG for ESP32-P4 compatibility
 */

#include "JpegReencoder.h"

#ifndef NATIVE_BUILD

#include "esp_jpeg_enc.h"
#include "esp_jpeg_common.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "jpeg_enc";

int yuv422_to_jpeg_420(const uint8_t *yuv_data, int width, int height,
                       uint8_t **out_jpeg, size_t *out_len,
                       int quality)
{
    *out_jpeg = NULL;
    *out_len = 0;

    uint64_t t0 = esp_timer_get_time();

    /* Configure encoder: YCbYCr input (YUV422 packed) → 4:2:0 JPEG */
    jpeg_enc_config_t enc_cfg = DEFAULT_JPEG_ENC_CONFIG();
    enc_cfg.src_type = JPEG_PIXEL_FORMAT_YCbYCr;
    enc_cfg.subsampling = JPEG_SUBSAMPLE_420;
    enc_cfg.quality = quality;
    enc_cfg.width = width;
    enc_cfg.height = height;
    enc_cfg.task_enable = false;
    enc_cfg.rotate = JPEG_ROTATE_0D;

    jpeg_enc_handle_t enc = NULL;
    jpeg_error_t jerr = jpeg_enc_open(&enc_cfg, &enc);
    if (jerr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Encoder open failed: %d", jerr);
        return -1;
    }

    /* Allocate output buffer — JPEG compressed is much smaller than raw */
    size_t raw_size = (size_t)width * height * 2;
    size_t max_out = raw_size / 2;  /* JPEG is typically <50% of raw */
    uint8_t *enc_buf = (uint8_t *)heap_caps_malloc(max_out, MALLOC_CAP_SPIRAM);
    if (!enc_buf) {
        ESP_LOGE(TAG, "Cannot allocate encode buffer (%zu bytes)", max_out);
        jpeg_enc_close(enc);
        return -2;
    }

    int out_size = 0;
    jerr = jpeg_enc_process(enc, yuv_data, (int)raw_size,
                            enc_buf, (int)max_out, &out_size);
    jpeg_enc_close(enc);

    if (jerr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Encode failed: %d", jerr);
        free(enc_buf);
        return -3;
    }

    uint64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "Encoded %dx%d YUV422 → %d bytes JPEG (4:2:0, q=%d) in %llums",
             width, height, out_size, quality, (t1 - t0) / 1000);

    *out_jpeg = enc_buf;
    *out_len = (size_t)out_size;
    return 0;
}

#else  /* NATIVE_BUILD */

int yuv422_to_jpeg_420(const uint8_t *yuv_data, int width, int height,
                       uint8_t **out_jpeg, size_t *out_len,
                       int quality)
{
    (void)yuv_data; (void)width; (void)height; (void)quality;
    *out_jpeg = NULL;
    *out_len = 0;
    return -1;
}

#endif
