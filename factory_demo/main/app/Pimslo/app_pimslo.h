/**
 * @file app_pimslo.h
 * @brief Background PIMSLO capture + GIF encoding pipeline
 *
 * Manages the async workflow: button press → SPI capture (4 cameras) →
 * save to numbered directory → queue background GIF encoding.
 * The user can keep taking photos without waiting for GIF encoding.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the PIMSLO subsystem
 *
 * Creates the SPI capture task (Core 0) and GIF encode queue task (Core 1),
 * plus the semaphore and queue for communication. Loads the capture counter
 * from NVS so numbering survives reboots.
 *
 * Call after app_video_stream_init() in main.c.
 */
esp_err_t app_pimslo_init(void);

/**
 * @brief Request an SPI capture of all 4 cameras
 *
 * Non-blocking — gives a binary semaphore that wakes the capture task.
 * Safe to call from the camera frame callback (~0ms).
 * If a capture is already in progress, the request is coalesced.
 */
void app_pimslo_request_capture(void);

/**
 * @brief Get number of GIF encode jobs waiting in the queue
 */
int app_pimslo_get_queue_depth(void);

/**
 * @brief Check if a GIF encode is currently in progress
 */
bool app_pimslo_is_encoding(void);

#ifdef __cplusplus
}
#endif
