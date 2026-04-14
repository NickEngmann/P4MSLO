/**
 * @file app_gifs.h
 * @brief GIF feature application layer
 *
 * Manages GIF creation from album JPEGs and GIF playback on the display.
 * Follows the same pattern as app_album.h.
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Directory for GIF files on SD card */
#define GIF_FOLDER_NAME "esp32_p4_gif_save"

/**
 * @brief Initialize the GIF feature
 * @param canvas  LVGL canvas object for rendering (from ui_ScreenGifs)
 */
esp_err_t app_gifs_init(lv_obj_t *canvas);

/** @brief Deinitialize and free resources */
void app_gifs_deinit(void);

/** @brief Scan SD card for existing .gif files */
esp_err_t app_gifs_scan(void);

/** @brief Get number of GIF files found */
int app_gifs_get_count(void);

/** @brief Get current GIF index */
int app_gifs_get_current_index(void);

/** @brief Navigate to next GIF */
esp_err_t app_gifs_next(void);

/** @brief Navigate to previous GIF */
esp_err_t app_gifs_prev(void);

/** @brief Start playback of the current GIF */
esp_err_t app_gifs_play_current(void);

/** @brief Stop playback */
void app_gifs_stop(void);

/** @brief Check if currently playing */
bool app_gifs_is_playing(void);

/**
 * @brief Create a GIF from all album JPEGs
 *
 * Runs in a background FreeRTOS task. Progress updates are posted
 * to the LVGL status label via lv_async_call.
 *
 * @param frame_delay_ms  Frame delay in milliseconds (e.g. 500)
 * @param max_frames      Maximum number of frames (0 = all photos)
 */
esp_err_t app_gifs_create_from_album(int frame_delay_ms, int max_frames);

/** @brief Check if GIF creation is in progress */
bool app_gifs_is_encoding(void);

/**
 * @brief Create a PIMSLO stereoscopic 3D GIF from 4 camera JPEGs on SD card
 *
 * Reads 4 JPEGs from /sdcard/pimslo/pos{1-4}.jpg, applies parallax crop
 * per position, and creates an oscillating 7-frame GIF (1→2→3→4→3→2→1).
 *
 * @param frame_delay_ms  Frame delay (default 150ms)
 * @param parallax        Parallax strength (0.0-1.0, default 0.05)
 */
esp_err_t app_gifs_create_pimslo(int frame_delay_ms, float parallax);

#ifdef __cplusplus
}
#endif
