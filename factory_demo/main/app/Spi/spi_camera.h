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
#include <stdbool.h>

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

/**
 * @brief Fire the shared GPIO34 trigger pulse ONLY. No SPI transfer, no
 *        waiting, no allocations — just the pin toggle. Cameras start
 *        capturing in parallel and hold the JPEG in their own PSRAM
 *        until polled.
 *
 * Meant to be called early (e.g. as soon as the photo button is pressed)
 * so the S3 capture time (~600 ms) overlaps with the P4's own JPEG
 * encode + SD save. The caller must then call
 * spi_camera_capture_all_after_trigger() instead of the normal
 * spi_camera_capture_all() so the trigger is not re-sent.
 *
 * @return ESP_OK unless SPI init failed
 */
esp_err_t spi_camera_send_trigger(void);

/**
 * @brief Same as spi_camera_capture_all but skips the first trigger
 *        pulse because the caller already sent it via
 *        spi_camera_send_trigger(). Retries on failure still fire
 *        their own triggers as usual.
 */
esp_err_t spi_camera_capture_all_after_trigger(uint8_t *jpeg_bufs[4],
                                                size_t jpeg_sizes[4],
                                                uint32_t *total_ms);

/** Control commands — must match esp32s3/src/spi/SPISlave.h. The 10× burst
 *  retry pattern in spi_camera_send_control() makes the 0x01–0x0F range
 *  reliable (the scan on the slave side picks up at least one intact byte). */
#define SPI_CAM_CMD_WIFI_ON         0x04
#define SPI_CAM_CMD_WIFI_OFF        0x05
#define SPI_CAM_CMD_REBOOT          0x06
#define SPI_CAM_CMD_IDENTIFY        0x07
#define SPI_CAM_CMD_AUTOFOCUS       0x08  /* Phase 4 — trigger OV5640 AF */
#define SPI_CAM_CMD_SET_EXPOSURE    0x09  /* Phase 4 — apply gain + exposure */

/** Status byte flag bits in the IDLE poll response */
#define SPI_CAM_STATUS_JPEG_READY       0x01
#define SPI_CAM_STATUS_WIFI_ACTIVE      0x02
#define SPI_CAM_STATUS_WIFI_CONNECTED   0x04
#define SPI_CAM_STATUS_AF_LOCKED        0x08  /* Phase 4 */

/** Extended IDLE header offsets (must match esp32s3/src/spi/SPISlave.h) */
#define SPI_CAM_HEADER_AE_GAIN_OFFSET       5
#define SPI_CAM_HEADER_AE_EXPOSURE_OFFSET   7
#define SPI_CAM_SET_EXPOSURE_PAYLOAD_LEN    5

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

/**
 * @brief Read a camera's current AE values from its IDLE header.
 *
 * Every status-poll response contains the current gain + exposure in bytes
 * 5..9, so we just do a normal status query and unpack. Used by Phase 4's
 * exposure-sync flow: snapshot one camera's AE, broadcast to the others.
 *
 * @param camera_idx     Camera index (0–3)
 * @param[out] ae_gain     16-bit gain (OV5640 regs 0x350A/0x350B)
 * @param[out] ae_exposure 24-bit exposure (OV5640 regs 0x3500/0x3501/0x3502)
 */
esp_err_t spi_camera_read_exposure(int camera_idx,
                                    uint16_t *ae_gain, uint32_t *ae_exposure);

/**
 * @brief Broadcast a SET_EXPOSURE command to one camera.
 *
 * The slave switches to manual AEC/AGC and applies the values to the sensor.
 * Call spi_camera_query_status() afterwards to verify the header reflects
 * the new values.
 */
esp_err_t spi_camera_set_exposure(int camera_idx,
                                   uint16_t ae_gain, uint32_t ae_exposure);

/**
 * @brief Cross-camera exposure sync: read camera ref_idx, broadcast to others.
 *
 * @param ref_idx  Reference camera whose AE values we copy FROM.
 * @return ESP_OK if the reference read and all broadcasts succeeded.
 */
esp_err_t spi_camera_sync_exposure(int ref_idx);

/**
 * @brief Broadcast AUTOFOCUS to all 4 cameras and wait for AF_LOCKED.
 *
 * Returns ESP_OK once all reachable cameras report AF_LOCKED, or after
 * timeout_ms even if some didn't lock (best-effort focus). Currently the
 * slave's AF is a stub — see CameraManager::autofocus() — so AF_LOCKED
 * comes back immediately.
 */
esp_err_t spi_camera_autofocus_all(uint32_t timeout_ms);

/**
 * @brief Runtime toggle for "fast capture mode" — cuts SPI settle delays
 *        in spi_camera_capture_all at the cost of slightly higher error risk.
 *
 * Persisted by the caller (app_pimslo reads/writes an NVS key). When enabled,
 * the post-trigger wait drops from 500→300ms and the inter-camera settle
 * drops from 50→30ms, saving ~280ms per PIMSLO burst. Off by default.
 */
void spi_camera_set_fast_mode(bool enabled);
bool spi_camera_get_fast_mode(void);

#ifdef __cplusplus
}
#endif
