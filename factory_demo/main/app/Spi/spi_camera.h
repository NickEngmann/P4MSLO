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

/** Control commands matching esp32s3/src/spi/SPISlave.h. Values chosen to be
 *  adjacent to CMD_READ_DATA (0x03) since low-byte wire patterns transmit
 *  reliably from P4 master to S3 slave; higher-bit values get mangled. */
#define SPI_CAM_CMD_WIFI_ON    0x04
#define SPI_CAM_CMD_WIFI_OFF   0x05
#define SPI_CAM_CMD_REBOOT     0x06
#define SPI_CAM_CMD_IDENTIFY   0x07

/** Status byte flag bits in the IDLE poll response */
#define SPI_CAM_STATUS_JPEG_READY       0x01
#define SPI_CAM_STATUS_WIFI_ACTIVE      0x02
#define SPI_CAM_STATUS_WIFI_CONNECTED   0x04

/**
 * @brief Send a one-byte control command to a single camera.
 *
 * Use this to ask a slave to start/stop WiFi, reboot, or blink for
 * identification. The slave executes the command asynchronously; this call
 * returns as soon as the SPI transaction completes.
 *
 * @param camera_idx  Camera index (0–3)
 * @param cmd         One of SPI_CAM_CMD_*
 */
esp_err_t spi_camera_send_control(int camera_idx, uint8_t cmd);

/**
 * @brief Poll one camera and return its status byte (JPEG/WiFi flags).
 *
 * @param camera_idx  Camera index (0–3)
 * @param[out] status_byte  Status flag bits (see SPI_CAM_STATUS_* above)
 */
esp_err_t spi_camera_query_status(int camera_idx, uint8_t *status_byte);

#ifdef __cplusplus
}
#endif
