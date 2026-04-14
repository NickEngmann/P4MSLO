/**
 * @file      JpegReencoder.h
 * @brief     Encode raw YUV422 to 4:2:0 JPEG for ESP32-P4 HW decoder compatibility
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Encode raw YUV422 frame to 4:2:0 JPEG
 *
 * @param yuv_data    Raw YUV422 (YUYV packed) data from camera
 * @param width       Frame width
 * @param height      Frame height
 * @param out_jpeg    Output JPEG pointer (caller must free with free())
 * @param out_len     Output JPEG length
 * @param quality     JPEG quality (1-100, higher = better)
 * @return 0 on success, negative on error
 */
int yuv422_to_jpeg_420(const uint8_t *yuv_data, int width, int height,
                       uint8_t **out_jpeg, size_t *out_len,
                       int quality);

#ifdef __cplusplus
}
#endif
