/**
 * @file spi_camera.h
 * @brief SPI master for receiving JPEG data from ESP32-S3 cameras
 *
 * Communicates with ESP32-S3 SPI slaves to receive captured JPEG photos.
 * Supports up to 4 cameras on the same SPI bus with different CS lines.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** SPI camera pin configuration */
#define SPI_CAM_CLK_PIN     37
#define SPI_CAM_MOSI_PIN    38
#define SPI_CAM_MISO_PIN    50
#define SPI_CAM_CS0_PIN     51   /* Camera #1 */
#define SPI_CAM_CS1_PIN     52   /* Camera #2 */
#define SPI_CAM_CS2_PIN     53   /* Camera #3 */
#define SPI_CAM_CS3_PIN     54   /* Camera #4 */
#define SPI_CAM_COUNT       4

/**
 * @brief Initialize the SPI master bus and all 4 camera devices
 */
esp_err_t spi_camera_init(void);

/**
 * @brief Receive JPEG data from a camera via SPI
 *
 * Polls the camera for JPEG ready status, reads the size, then
 * DMA-transfers the JPEG data into a PSRAM buffer.
 *
 * @param camera_idx  Camera index (0-3)
 * @param[out] jpeg_buf   Pointer to receive JPEG data (caller frees with free())
 * @param[out] jpeg_size  Size of received JPEG data
 * @param[out] transfer_ms  Transfer time in milliseconds
 * @return ESP_OK on success
 */
esp_err_t spi_camera_receive_jpeg(int camera_idx,
                                   uint8_t **jpeg_buf, size_t *jpeg_size,
                                   uint32_t *transfer_ms);

/**
 * @brief Trigger GPIO34 + receive JPEGs from all 4 cameras sequentially
 *
 * @param[out] jpeg_bufs   Array of 4 JPEG buffer pointers (caller frees each)
 * @param[out] jpeg_sizes  Array of 4 sizes
 * @param[out] total_ms    Total time from trigger to last transfer complete
 * @return ESP_OK if all 4 succeeded
 */
esp_err_t spi_camera_capture_all(uint8_t *jpeg_bufs[4], size_t jpeg_sizes[4],
                                  uint32_t *total_ms);

#ifdef __cplusplus
}
#endif
