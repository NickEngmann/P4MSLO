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

/**
 * @brief Peek the capture number that will be assigned to the next SPI burst.
 *
 * Returns without incrementing. Used by the photo-save path to name the
 * P4 preview JPEG with the same index the upcoming PIMSLO directory will
 * use, so the gallery can link them up.
 */
uint16_t app_pimslo_peek_next_num(void);

/**
 * @brief Copy the most recent P4 photo into the preview directory, renaming
 * it to P4M<num>.jpg so the gallery can use it as a placeholder while the
 * matching GIF is still encoding.
 *
 * Called from the video-stream frame callback right after a P4 photo is
 * saved via take_and_save_photo(), in combination with app_pimslo_peek_next_num().
 * Non-blocking-ish (just streams the file in chunks); runs on the caller's task.
 */
esp_err_t app_pimslo_save_preview_from_latest_photo(uint16_t num);

/**
 * @brief Directory where P4 preview JPEGs are stored while their GIF is
 * being encoded. Format: <dir>/P4M0001.jpg
 */
#define PIMSLO_PREVIEW_DIR "/sdcard/p4mslo_previews"

#ifdef __cplusplus
}
#endif
