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
#define GIF_FOLDER_NAME "p4mslo_gifs"

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

/**
 * @brief Free the cross-GIF decoded-frame cache.
 *
 * The gallery keeps recently-watched GIFs' decoded canvases in PSRAM
 * so scrolling between them replays instantly. That cache is ~700 KB
 * per GIF × up to 5 GIFs = ~3.5 MB pinned. Call this when leaving the
 * gallery so camera / video / GIF-encoder paths get that PSRAM back.
 */
void app_gifs_flush_cache(void);

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

/**
 * @brief Encode a PIMSLO GIF from JPEGs in a capture directory
 *
 * Blocking call — runs the full two-pass GIF encode pipeline.
 * Frees camera buffers during encoding and restores them after.
 *
 * @param capture_dir  Directory containing pos{1-4}.jpg files
 * @param frame_delay_ms  Frame delay (default 150ms)
 * @param parallax        Parallax strength (0.0-1.0, default 0.05)
 */
esp_err_t app_gifs_encode_pimslo_from_dir(const char *capture_dir,
                                           int frame_delay_ms, float parallax);

/**
 * @brief Start the background worker that pre-renders `.p4ms` files and
 *        auto-encodes stale PIMSLO captures.
 *
 * One shot; safe to call once at boot after app_gifs_init() and
 * app_pimslo_init(). The task lives on Core 1 at priority 2 and yields
 * to foreground activity (open gallery, active PIMSLO capture / encode).
 *
 * Work priority (per iteration):
 *   1. `.gif` files that don't have a matching `.p4ms` — pre-render
 *      them so next gallery entry plays instantly from disk.
 *   2. PIMSLO captures that still have a JPEG preview but no `.gif`
 *      (e.g. encode interrupted by a reboot) — re-run the PIMSLO
 *      encode from `/sdcard/p4mslo/<stem>/`.
 *
 * When both queues are empty the worker idles at low duty cycle.
 */
void app_gifs_start_background_worker(void);

/**
 * @brief Tell the background worker whether the user is on the gallery
 *        page. When true, the worker aborts any in-progress pre-render
 *        and pauses scheduling — foreground playback has exclusive
 *        access to the gif_decoder and the PSRAM it pins.
 */
void app_gifs_set_gallery_open(bool open);

/**
 * @brief True once the user has opened the gallery at least once since
 *        boot. Used by the PIMSLO encode task as a gate: the ~7 MB GIF
 *        encode is deferred until the user has demonstrated they care
 *        about GIFs right now, so boot-time / camera-time PSRAM stays
 *        available for viewfinder buffers.
 */
bool app_gifs_gallery_ever_opened(void);

#ifdef __cplusplus
}
#endif
