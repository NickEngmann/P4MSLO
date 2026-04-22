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
 * Also flips the "is_capturing" flag IMMEDIATELY so the saving overlay
 * can start animating in the same LVGL tick as the button press,
 * instead of waiting for the capture task to schedule + grab the
 * semaphore (observed ~500 ms lag). The capture task clears the flag
 * once the full cycle (SPI + save) is done.
 *
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
 * @brief Check if an SPI capture + save is currently running on the
 * capture task. True between the semaphore-take and the end of the
 * capture + JPEG-save cycle. Use from background workers that need to
 * defer PSRAM-heavy work to avoid contending with the capture buffers.
 */
bool app_pimslo_is_capturing(void);

/**
 * @brief Copy the most recent P4 photo into the preview directory, renaming
 * it to P4M<num>.jpg so the gallery can use it as a placeholder while the
 * matching GIF is still encoding.
 *
 * Called from the pimslo capture task (on its 8 KB stack) right after the
 * video-stream callback has given us the capture semaphore — at that point
 * the P4 photo file has already been fsync'd to SD. Uses a heap-allocated
 * copy buffer so it's safe to call from any task.
 */
esp_err_t app_pimslo_save_preview_from_latest_photo(uint16_t num);

/**
 * @brief Directory where P4 preview JPEGs are stored while their GIF is
 * being encoded. Format: <dir>/P4M0001.jpg
 */
#define PIMSLO_PREVIEW_DIR "/sdcard/p4mslo_previews"

/**
 * @brief Get/set the PIMSLO "fast capture" mode (persisted in NVS).
 *
 * When enabled, the pre-capture AF + exposure-sync pass is skipped and the
 * SPI settle delays inside spi_camera_capture_all are reduced. Trades image
 * quality for ~2 seconds of capture latency.
 */
bool app_pimslo_get_fast_mode(void);
esp_err_t app_pimslo_set_fast_mode(bool enabled);

#ifdef __cplusplus
}
#endif
