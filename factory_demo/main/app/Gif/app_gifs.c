/**
 * @file app_gifs.c
 * @brief GIF feature application layer
 *
 * Manages GIF file scanning, creation from album JPEGs, and animated playback.
 * Follows the app_album.c pattern for canvas display and SD card access.
 */

#include "app_gifs.h"
#include "gif_encoder.h"
#include "gif_decoder.h"
#include "gif_tjpgd.h"
#include "driver/jpeg_decode.h"
#include "app_pimslo.h"
#include "ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_video_stream.h"
#include "app_album.h"

static const char *TAG = "app_gifs";

#define MAX_PATH_LEN  512
#define MAX_GIF_FILES 64
#define MAX_FRAME_CACHE 12   /* unique frames we'll cache per GIF — plenty
                              * for PIMSLO 6-frame palindromes with headroom */

/* Photo source directory (same as album) */
#define PIC_FOLDER_NAME "esp32_p4_pic_save"

typedef enum {
    GALLERY_ENTRY_GIF,    /* animated */
    GALLERY_ENTRY_JPEG,   /* still preview (encode not yet done) */
} gallery_entry_type_t;

/* A gallery entry represents one PIMSLO capture. It carries EITHER:
 *   - gif_path set, jpeg_path maybe-set : primary is an animated GIF;
 *     the JPEG preview (if present) is shown as an instant-flash still
 *     while the GIF's first-loop decode is running.
 *   - gif_path NULL, jpeg_path set       : capture still encoding, show
 *     the still with the center "PROCESSING" badge.
 *
 * Scan merges /sdcard/p4mslo_gifs and /sdcard/p4mslo_previews so every
 * capture is represented exactly once. */
typedef struct {
    char *gif_path;    /* animated PIMSLO GIF, or NULL if not yet encoded */
    char *jpeg_path;   /* P4 camera still preview, or NULL if none */
    gallery_entry_type_t type;  /* GIF if gif_path != NULL, else JPEG */
} gallery_entry_t;

/* Per-frame cache entry inside a cached GIF — maps a hash of the frame's
 * LZW bytes to its already-decoded 240×240 canvas. */
typedef struct {
    uint32_t hash;
    uint16_t *canvas;  /* canvas_width * canvas_height * 2 bytes, PSRAM */
    int delay_cs;      /* GIF's per-frame delay for timing replay */
    bool used;
} frame_cache_t;

/* Persistent across-GIF decoded cache. Each slot holds all frames for
 * one GIF keyed by its path. LRU-evicted when capacity is exceeded.
 * Lets the user scroll up and down through recently-viewed GIFs and
 * see them replay instantly from memory instead of re-opening the
 * file + re-decoding every LZW frame. */
#define MAX_CACHED_GIFS   5   /* ~5 × 700 KB worst case = ~3.5 MB of PSRAM */
#define MAX_FRAMES_PER_GIF 12

typedef struct {
    uint16_t *canvas;    /* 115 KB PSRAM buffer, NULL if empty */
    int      delay_cs;   /* per-frame GIF delay */
    uint32_t hash;       /* LZW hash (for dedup when populating) */
} cached_frame_t;

typedef struct {
    char *gif_path;                              /* NULL if slot empty */
    cached_frame_t frames[MAX_FRAMES_PER_GIF];
    int total_frames;                            /* set on first-loop complete */
    bool complete;                               /* true once wrap-around seen */
    int64_t last_used_us;
} gif_cache_slot_t;

static gif_cache_slot_t g_gif_cache[MAX_CACHED_GIFS];

typedef struct {
    lv_obj_t *canvas;
    uint16_t *canvas_buffer;       /* 240x240 for display (decode target) */
    int canvas_width, canvas_height;

    /* Native source dimensions of the currently-open GIF, for logging. */
    int decode_width, decode_height;

    gallery_entry_t *entries;      /* sized `count`, owned */
    int count;
    int current_index;

    gif_decoder_t *decoder;
    lv_timer_t *play_timer;
    bool is_playing;
    bool is_encoding;

    /* Per-GIF decoded-frame cache. Freed on stop(). Entries are populated
     * as frames are decoded — reverse frames in PIMSLO palindromes hit
     * this cache and skip the LZW decode entirely. */
    frame_cache_t frame_cache[MAX_FRAME_CACHE];
    int frame_cache_n;

    /* Short display name of the current entry (e.g. "P4M0007" or
     * "pimslo"). Shown in `name_label` — a regular LVGL label widget
     * positioned at the bottom of the GIFS screen, NOT drawn into the
     * canvas pixel buffer. Keeping this in its own LVGL object layer
     * avoids the earlier black-screen bug where drawing rect+text
     * directly onto the canvas buffer blanked the display. */
    char current_label[32];
    lv_obj_t *name_label;

    /* Shown in the center of the screen ONLY when the current entry is
     * a JPEG preview (i.e. the GIF encode is still queued or running).
     * Tells the user the image is a still frame and a full PIMSLO GIF
     * will replace it once encoding finishes. Hidden for GIF entries. */
    lv_obj_t *processing_label;

    /* Centered "loading..." overlay shown during a GIF's first loop,
     * while the decoder is populating the frame-offset map + canvas
     * cache. Once the decoder's first loop completes and subsequent
     * frames come from the fast path at native framerate, this hides.
     * A 500 ms LVGL timer cycles the dots so the user can see the
     * device is making progress during the ~5-10 s first-loop wait. */
    lv_obj_t *loading_label;
    lv_timer_t *loading_timer;
    int loading_step;
    bool first_loop_complete;

    /* Per-GIF frame counter used only for throttled hash/hit/miss diag
     * logging (first 20 frames of each play_current). Reset per play. */
    int diag_frame_no;

    size_t cache_line_size;
} gifs_context_t;

static gifs_context_t s_ctx = {0};

/* ---- Internal helpers ---- */

static bool is_gif_file(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    return (strcasecmp(ext, ".gif") == 0);
}

static void ensure_gif_dir(void)
{
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, GIF_FOLDER_NAME);
    mkdir(path, 0755);
}

/* Convert RGB888 palette color to RGB565 with byte swap for LVGL */
static inline uint16_t rgb888_to_rgb565_swapped(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t px = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (px >> 8) | (px << 8);  /* Byte swap for LV_COLOR_16_SWAP */
}

/* Cycle the "loading..." overlay's trailing dots so the user sees
 * visible progress while the decoder's first loop is still populating
 * the frame cache. Pattern bounces 0 → 1 → 2 → 3 → 2 → 1 → 0... */
static void loading_anim_cb(lv_timer_t *timer)
{
    static const char *patterns[6] = {
        "loading", "loading.", "loading..", "loading...",
        "loading..", "loading.",
    };
    if (!s_ctx.loading_label) return;
    bsp_display_lock(0);
    lv_label_set_text(s_ctx.loading_label, patterns[s_ctx.loading_step]);
    bsp_display_unlock();
    s_ctx.loading_step = (s_ctx.loading_step + 1) % 6;
}

static void show_loading_overlay(void)
{
    if (!s_ctx.loading_label) return;
    s_ctx.loading_step = 0;
    s_ctx.first_loop_complete = false;
    bsp_display_lock(0);
    lv_label_set_text(s_ctx.loading_label, "loading");
    lv_obj_clear_flag(s_ctx.loading_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ctx.loading_label);
    bsp_display_unlock();
    if (!s_ctx.loading_timer) {
        s_ctx.loading_timer = lv_timer_create(loading_anim_cb, 500, NULL);
    }
}

static void hide_loading_overlay(void)
{
    if (s_ctx.loading_timer) {
        lv_timer_del(s_ctx.loading_timer);
        s_ctx.loading_timer = NULL;
    }
    if (s_ctx.loading_label) {
        bsp_display_lock(0);
        lv_obj_add_flag(s_ctx.loading_label, LV_OBJ_FLAG_HIDDEN);
        bsp_display_unlock();
    }
}

/* Pull the display filename out of a full path. Keeps the extension so
 * the user can tell at a glance whether an entry is a finished animated
 * GIF or the JPEG placeholder for a capture whose encode isn't done yet.
 * "/sdcard/p4mslo_gifs/P4M0007.gif" → "P4M0007.gif"
 * "/sdcard/p4mslo_previews/P4M0006.jpg" → "P4M0006.jpg"  */
static void compute_display_name(const char *path, char *out, size_t out_cap)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t len = strlen(base);
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out, base, len);
    out[len] = 0;
}

static void playback_timer_cb(lv_timer_t *timer)
{
    if (!s_ctx.is_playing || !s_ctx.decoder || !s_ctx.canvas_buffer) {
        return;
    }

    int delay_cs = 10;
    size_t canvas_bytes = s_ctx.canvas_width * s_ctx.canvas_height * 2;

    /* Two-step dedup-aware decode. Step 1: pull the next frame's
     * compressed LZW bytes into the decoder's internal buffer and hash
     * them. Step 2: if that hash is in our frame cache, memcpy the
     * cached canvas (fast path); otherwise decode for real and insert
     * the result into the cache. Reverse frames in PIMSLO palindromes
     * hit the cache and skip LZW decode entirely. */
    uint32_t hash = 0;
    esp_err_t ret = gif_decoder_read_next_frame(s_ctx.decoder, &hash, &delay_cs);
    if (ret == ESP_ERR_NOT_FOUND) {
        /* End of file — loop back to the first frame. This is also the
         * cue that we've completed one full pass: the frame-offset map
         * and canvas cache are now populated, so subsequent frames will
         * fast-path at native framerate. Hide the "loading..." overlay. */
        if (!s_ctx.first_loop_complete) {
            s_ctx.first_loop_complete = true;
            hide_loading_overlay();
        }
        gif_decoder_reset(s_ctx.decoder);
        ret = gif_decoder_read_next_frame(s_ctx.decoder, &hash, &delay_cs);
        if (ret != ESP_OK) { app_gifs_stop(); return; }
    } else if (ret != ESP_OK) {
        app_gifs_stop();
        return;
    }

    /* Cache lookup. */
    int hit_idx = -1;
    for (int i = 0; i < s_ctx.frame_cache_n; i++) {
        if (s_ctx.frame_cache[i].used && s_ctx.frame_cache[i].hash == hash) {
            hit_idx = i; break;
        }
    }

    /* Throttled diagnostic: the first handful of frames of each GIF only.
     * Lets us see hashes + hit/miss to prove (or disprove) the dedup,
     * without flooding serial during long playback. Counter resets on
     * each new play_current(). */
    const int DIAG_LIMIT = 20;
    bool diag_log = s_ctx.diag_frame_no < DIAG_LIMIT;
    int this_frame = s_ctx.diag_frame_no++;

    if (hit_idx >= 0) {
        if (diag_log) {
            ESP_LOGI(TAG, "f#%d HIT  hash=%08x slot=%d delay=%dcs",
                     this_frame, (unsigned)hash, hit_idx, delay_cs);
        }
        memcpy(s_ctx.canvas_buffer, s_ctx.frame_cache[hit_idx].canvas, canvas_bytes);
        gif_decoder_discard_read_frame(s_ctx.decoder);
    } else {
        uint32_t t0 = diag_log ? esp_log_timestamp() : 0;
        ret = gif_decoder_decode_read_frame(s_ctx.decoder,
                                             s_ctx.canvas_buffer,
                                             s_ctx.canvas_width,
                                             s_ctx.canvas_height);
        if (ret != ESP_OK) { app_gifs_stop(); return; }

        bool cached = false;
        if (s_ctx.frame_cache_n < MAX_FRAME_CACHE) {
            uint16_t *copy = heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM);
            if (copy) {
                memcpy(copy, s_ctx.canvas_buffer, canvas_bytes);
                int i = s_ctx.frame_cache_n++;
                s_ctx.frame_cache[i].hash = hash;
                s_ctx.frame_cache[i].canvas = copy;
                s_ctx.frame_cache[i].used = true;
                cached = true;
            }
        }
        if (diag_log) {
            ESP_LOGI(TAG, "f#%d MISS hash=%08x decode=%lums delay=%dcs "
                          "cached=%d total_cached=%d",
                     this_frame, (unsigned)hash,
                     (unsigned long)(esp_log_timestamp() - t0),
                     delay_cs, cached, s_ctx.frame_cache_n);
        }
    }

    /* Push the freshly-rendered canvas to LVGL. */
    bsp_display_lock(0);
    lv_canvas_set_buffer(s_ctx.canvas, s_ctx.canvas_buffer,
                         s_ctx.canvas_width, s_ctx.canvas_height,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_invalidate(s_ctx.canvas);
    bsp_display_unlock();

    /* Adjust timer period for next frame */
    if (delay_cs > 0) {
        lv_timer_set_period(timer, delay_cs * 10);
    }
}

static void display_gif_info(void)
{
    if (s_ctx.count == 0 || !s_ctx.canvas_buffer) return;

    bsp_display_lock(0);

    /* Clear canvas to dark background */
    memset(s_ctx.canvas_buffer, 0x10, s_ctx.canvas_width * s_ctx.canvas_height * 2);
    lv_canvas_set_buffer(s_ctx.canvas, s_ctx.canvas_buffer,
                         s_ctx.canvas_width, s_ctx.canvas_height,
                         LV_IMG_CF_TRUE_COLOR);

    /* Draw GIF filename and index */
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_white();
    label_dsc.font = &lv_font_montserrat_16;
    label_dsc.align = LV_TEXT_ALIGN_CENTER;

    char text[64];
    snprintf(text, sizeof(text), "GIF %d / %d\n\nPress to play",
             s_ctx.current_index + 1, s_ctx.count);
    lv_canvas_draw_text(s_ctx.canvas, 20, s_ctx.canvas_height / 2 - 30,
                        s_ctx.canvas_width - 40, &label_dsc, text);

    lv_obj_invalidate(s_ctx.canvas);
    bsp_display_unlock();
}

/* ---- Encoding task ---- */

typedef struct {
    int frame_delay_ms;
    int max_frames;
} encode_task_params_t;

static void encode_progress_cb(int current, int total, int pass, void *user)
{
    ESP_LOGI(TAG, "Encoding pass %d: frame %d/%d", pass, current, total);
}

static void encode_task(void *param)
{
    encode_task_params_t *p = (encode_task_params_t *)param;
    esp_err_t ret;

    /* Scan for source JPEGs */
    char pic_dir[MAX_PATH_LEN];
    snprintf(pic_dir, sizeof(pic_dir), "%s/%s", BSP_SD_MOUNT_POINT, PIC_FOLDER_NAME);

    DIR *dir = opendir(pic_dir);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open %s", pic_dir);
        s_ctx.is_encoding = false;
        free(p);
        vTaskDelete(NULL);
        return;
    }

    /* Collect JPEG filenames */
    char **jpeg_files = heap_caps_malloc(64 * sizeof(char *), MALLOC_CAP_SPIRAM);
    int jpeg_count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && jpeg_count < 64) {
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext) continue;
        if (strcasecmp(ext, ".jpg") != 0 && strcasecmp(ext, ".jpeg") != 0) continue;
        if (entry->d_name[0] == '.') continue;

        jpeg_files[jpeg_count] = heap_caps_malloc(MAX_PATH_LEN, MALLOC_CAP_SPIRAM);
        snprintf(jpeg_files[jpeg_count], MAX_PATH_LEN, "%.200s/%.255s", pic_dir, entry->d_name);
        jpeg_count++;
    }
    closedir(dir);

    if (jpeg_count == 0) {
        ESP_LOGW(TAG, "No JPEG files found");
        heap_caps_free(jpeg_files);
        s_ctx.is_encoding = false;
        free(p);
        vTaskDelete(NULL);
        return;
    }

    /* Randomly select max_frames images (Fisher-Yates shuffle) */
    int use_count = jpeg_count;
    if (p->max_frames > 0 && p->max_frames < jpeg_count) {
        /* Shuffle and take first max_frames */
        uint32_t seed = (uint32_t)esp_log_timestamp();
        for (int i = jpeg_count - 1; i > 0; i--) {
            seed = seed * 1103515245 + 12345;
            int j = (seed >> 16) % (i + 1);
            char *tmp = jpeg_files[i];
            jpeg_files[i] = jpeg_files[j];
            jpeg_files[j] = tmp;
        }
        use_count = p->max_frames;
    }

    ESP_LOGI(TAG, "Creating GIF from %d JPEGs (of %d available)", use_count, jpeg_count);

    /* Free camera buffers and album JPEG decoder to make PSRAM available */
    app_video_stream_free_buffers();
    app_album_release_jpeg_decoder();
    ESP_LOGI(TAG, "Free PSRAM after releasing buffers: %zu",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Create encoder — use 480xAuto (proportional) by default */
    gif_encoder_config_t cfg = {
        .frame_delay_cs = p->frame_delay_ms / 10,
        .loop_count = 0,
        .target_width = 0,   /* Auto-detect from first frame */
        .target_height = 0,
    };

    gif_encoder_t *enc = NULL;
    ret = gif_encoder_create(&cfg, &enc);
    if (ret != ESP_OK) goto cleanup;

    gif_encoder_set_progress_cb(enc, encode_progress_cb, NULL);

    /* Pass 1: build palette */
    for (int i = 0; i < use_count; i++) {
        ret = gif_encoder_pass1_add_frame(enc, jpeg_files[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Pass 1 failed for %s, skipping", jpeg_files[i]);
        }
    }
    ret = gif_encoder_pass1_finalize(enc);
    if (ret != ESP_OK) goto cleanup;

    /* Pass 2: encode frames */
    ensure_gif_dir();
    char output_path[MAX_PATH_LEN];
    snprintf(output_path, sizeof(output_path), "%s/%s/animation_%ld.gif",
             BSP_SD_MOUNT_POINT, GIF_FOLDER_NAME, (long)esp_log_timestamp());

    ret = gif_encoder_pass2_begin(enc, output_path);
    if (ret != ESP_OK) goto cleanup;

    for (int i = 0; i < use_count; i++) {
        ret = gif_encoder_pass2_add_frame(enc, jpeg_files[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Pass 2 failed for %s, skipping", jpeg_files[i]);
        }
    }

    ret = gif_encoder_pass2_finish(enc);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "GIF saved to %s", output_path);
    }

cleanup:
    if (enc) gif_encoder_destroy(enc);
    for (int i = 0; i < jpeg_count; i++) {
        heap_caps_free(jpeg_files[i]);
    }
    heap_caps_free(jpeg_files);
    free(p);

    /* Restore JPEG decoder and camera buffers */
    app_album_reacquire_jpeg_decoder();
    esp_err_t realloc_ret = app_video_stream_realloc_buffers();
    if (realloc_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restore camera buffers: 0x%x", realloc_ret);
    }

    s_ctx.is_encoding = false;

    /* Rescan to pick up the new GIF */
    app_gifs_scan();

    vTaskDelete(NULL);
}

/* ---- Public API ---- */

esp_err_t app_gifs_init(lv_obj_t *canvas)
{
    memset(&s_ctx, 0, sizeof(s_ctx));

    s_ctx.canvas = canvas;
    s_ctx.canvas_width = BSP_LCD_H_RES;
    s_ctx.canvas_height = BSP_LCD_V_RES;

    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &s_ctx.cache_line_size);

    size_t buf_size = s_ctx.canvas_width * s_ctx.canvas_height * 2;
    s_ctx.canvas_buffer = heap_caps_aligned_calloc(s_ctx.cache_line_size, 1,
                                                    buf_size, MALLOC_CAP_SPIRAM);
    if (!s_ctx.canvas_buffer) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer");
        return ESP_ERR_NO_MEM;
    }

    /* Create the entry-name label as an overlay on the gallery screen.
     * A regular LVGL label stacked above the canvas avoids poking at the
     * canvas pixel buffer directly — much safer than lv_canvas_draw_text
     * and zero per-frame cost. */
    s_ctx.name_label = lv_label_create(ui_ScreenGifs);
    lv_obj_set_style_bg_color(s_ctx.name_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ctx.name_label, LV_OPA_60, 0);
    lv_obj_set_style_text_color(s_ctx.name_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_ctx.name_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_all(s_ctx.name_label, 3, 0);
    lv_obj_set_style_radius(s_ctx.name_label, 4, 0);
    lv_obj_align(s_ctx.name_label, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_label_set_text(s_ctx.name_label, "");
    /* Hidden until the first play_current() updates its text. */
    lv_obj_add_flag(s_ctx.name_label, LV_OBJ_FLAG_HIDDEN);

    /* Center "PROCESSING" badge for JPEG-preview entries — a static
     * still frame with this overlay tells the user the captured burst
     * hasn't finished encoding into a GIF yet. Hidden on GIF entries. */
    s_ctx.processing_label = lv_label_create(ui_ScreenGifs);
    lv_obj_set_style_bg_color(s_ctx.processing_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ctx.processing_label, LV_OPA_70, 0);
    lv_obj_set_style_text_color(s_ctx.processing_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_ctx.processing_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_all(s_ctx.processing_label, 8, 0);
    lv_obj_set_style_radius(s_ctx.processing_label, 6, 0);
    lv_obj_align(s_ctx.processing_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_ctx.processing_label, "PROCESSING");
    lv_obj_add_flag(s_ctx.processing_label, LV_OBJ_FLAG_HIDDEN);

    /* Center "loading..." badge, styled like the PROCESSING one but
     * smaller and dimmer since it's a transient state. Shown only
     * during a GIF's first loop. */
    s_ctx.loading_label = lv_label_create(ui_ScreenGifs);
    lv_obj_set_style_bg_color(s_ctx.loading_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ctx.loading_label, LV_OPA_50, 0);
    lv_obj_set_style_text_color(s_ctx.loading_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_ctx.loading_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_all(s_ctx.loading_label, 6, 0);
    lv_obj_set_style_radius(s_ctx.loading_label, 4, 0);
    lv_obj_align(s_ctx.loading_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_ctx.loading_label, "loading");
    lv_obj_add_flag(s_ctx.loading_label, LV_OBJ_FLAG_HIDDEN);

    return ESP_OK;
}

static void free_entries(void)
{
    if (!s_ctx.entries) return;
    for (int i = 0; i < s_ctx.count; i++) {
        if (s_ctx.entries[i].gif_path) free(s_ctx.entries[i].gif_path);
        if (s_ctx.entries[i].jpeg_path) free(s_ctx.entries[i].jpeg_path);
    }
    free(s_ctx.entries);
    s_ctx.entries = NULL;
    s_ctx.count = 0;
}

/* Return the path that identifies this entry (prefers GIF if available). */
static const char *entry_primary_path(const gallery_entry_t *e)
{
    return e->gif_path ? e->gif_path : e->jpeg_path;
}

void app_gifs_deinit(void)
{
    app_gifs_stop();
    free_entries();
    if (s_ctx.canvas_buffer) {
        heap_caps_free(s_ctx.canvas_buffer);
        s_ctx.canvas_buffer = NULL;
    }
}

/* Extract the PIMSLO capture stem from a filename like "P4M0007.gif"
 * or "P4M0007.jpg" → writes "P4M0007" into `stem_out`.
 * Returns true if the basename matched the PIMSLO pattern. */
static bool extract_pimslo_stem(const char *name, char *stem_out, size_t stem_cap)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    size_t len = (size_t)(dot - name);
    if (len == 0 || len >= stem_cap) return false;
    memcpy(stem_out, name, len);
    stem_out[len] = 0;
    return true;
}

/* Look up an entry by its PIMSLO stem (e.g. "P4M0007"), returning
 * the index, or -1 if none. Used by the scan's Pass 2 to merge a
 * discovered JPEG preview into the already-created entry for the
 * matching GIF, rather than adding a duplicate. */
static int find_entry_by_stem(const char *stem)
{
    for (int i = 0; i < s_ctx.count; i++) {
        const char *p = entry_primary_path(&s_ctx.entries[i]);
        if (!p) continue;
        const char *slash = strrchr(p, '/');
        const char *base = slash ? slash + 1 : p;
        char other_stem[32];
        if (extract_pimslo_stem(base, other_stem, sizeof(other_stem)) &&
            strcmp(stem, other_stem) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t app_gifs_scan(void)
{
    /* Remember the previously-viewed entry by its primary path so we can
     * restore current_index after the rescan (files may have been added
     * or removed in between). This is what makes "leave gallery, come
     * back, land on same GIF" work. */
    char preserved_path[MAX_PATH_LEN] = "";
    if (s_ctx.entries && s_ctx.current_index >= 0 &&
        s_ctx.current_index < s_ctx.count) {
        const char *p = entry_primary_path(&s_ctx.entries[s_ctx.current_index]);
        if (p) {
            strncpy(preserved_path, p, sizeof(preserved_path) - 1);
            preserved_path[sizeof(preserved_path) - 1] = 0;
        }
    }

    free_entries();

    ensure_gif_dir();
    char gif_dir[MAX_PATH_LEN];
    snprintf(gif_dir, sizeof(gif_dir), "%s/%s", BSP_SD_MOUNT_POINT, GIF_FOLDER_NAME);

    s_ctx.entries = calloc(MAX_GIF_FILES, sizeof(gallery_entry_t));
    if (!s_ctx.entries) return ESP_ERR_NO_MEM;

    /* Pass 1: every finished .gif. */
    DIR *dir = opendir(gif_dir);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && s_ctx.count < MAX_GIF_FILES) {
            if (!is_gif_file(entry->d_name) || entry->d_name[0] == '.') continue;
            gallery_entry_t *e = &s_ctx.entries[s_ctx.count++];
            e->type = GALLERY_ENTRY_GIF;
            e->gif_path = malloc(MAX_PATH_LEN);
            snprintf(e->gif_path, MAX_PATH_LEN, "%.200s/%.255s",
                     gif_dir, entry->d_name);
        }
        closedir(dir);
    }

    /* Pass 2: JPEG previews. If a preview's PIMSLO stem matches an
     * already-added GIF entry, attach the JPEG path to that entry so
     * play_current can flash the preview instantly while the GIF's
     * first-loop decode runs. Previews without a matching GIF become
     * their own JPEG-type entries. */
    DIR *pdir = opendir(PIMSLO_PREVIEW_DIR);
    if (pdir) {
        struct dirent *entry;
        while ((entry = readdir(pdir)) != NULL && s_ctx.count < MAX_GIF_FILES) {
            const char *ext = strrchr(entry->d_name, '.');
            if (!ext || strcasecmp(ext, ".jpg") != 0 || entry->d_name[0] == '.') continue;
            char stem[32];
            if (!extract_pimslo_stem(entry->d_name, stem, sizeof(stem))) continue;

            char jpeg_path[MAX_PATH_LEN];
            snprintf(jpeg_path, sizeof(jpeg_path), "%s/%s",
                     PIMSLO_PREVIEW_DIR, entry->d_name);

            int existing = find_entry_by_stem(stem);
            if (existing >= 0 && s_ctx.entries[existing].type == GALLERY_ENTRY_GIF) {
                /* Attach preview to existing GIF entry. */
                s_ctx.entries[existing].jpeg_path = strdup(jpeg_path);
            } else if (existing < 0) {
                /* New JPEG-only entry (GIF encode still pending). */
                gallery_entry_t *e = &s_ctx.entries[s_ctx.count++];
                e->type = GALLERY_ENTRY_JPEG;
                e->jpeg_path = strdup(jpeg_path);
            }
        }
        closedir(pdir);
    }

    /* Restore the previously-viewed entry by path match. If the path no
     * longer exists (e.g. GIF was deleted) fall back to index 0. */
    s_ctx.current_index = 0;
    if (preserved_path[0]) {
        for (int i = 0; i < s_ctx.count; i++) {
            const char *p = entry_primary_path(&s_ctx.entries[i]);
            if (p && strcmp(p, preserved_path) == 0) {
                s_ctx.current_index = i;
                break;
            }
        }
    }

    ESP_LOGI(TAG, "Gallery scan: %d entries, current=%d (preserved_path=%s)",
             s_ctx.count, s_ctx.current_index,
             preserved_path[0] ? preserved_path : "<none>");
    return ESP_OK;
}

int app_gifs_get_count(void) { return s_ctx.count; }
int app_gifs_get_current_index(void) { return s_ctx.current_index; }

esp_err_t app_gifs_next(void)
{
    if (s_ctx.count == 0) return ESP_FAIL;
    app_gifs_stop();
    s_ctx.current_index = (s_ctx.current_index + 1) % s_ctx.count;
    display_gif_info();
    return ESP_OK;
}

esp_err_t app_gifs_prev(void)
{
    if (s_ctx.count == 0) return ESP_FAIL;
    app_gifs_stop();
    s_ctx.current_index = (s_ctx.current_index + s_ctx.count - 1) % s_ctx.count;
    display_gif_info();
    return ESP_OK;
}

/* Decode a full-size JPEG into the 240×240 canvas buffer via tjpgd.
 * Used for entries that don't yet have a GIF (the PIMSLO encode is still
 * running or was interrupted). Static image, no animation. */
typedef struct {
    FILE *fp;
    uint16_t *canvas;
    int canvas_w, canvas_h;
    int src_w, src_h;  /* learned from the JPEG header via jd_prepare */
} jpeg_ctx_t;

static size_t jpeg_in_cb(JDEC *jd, uint8_t *buf, size_t len)
{
    jpeg_ctx_t *c = (jpeg_ctx_t *)jd->device;
    if (buf) return fread(buf, 1, len, c->fp);
    return (size_t)(fseek(c->fp, (long)len, SEEK_CUR) == 0 ? len : 0);
}

static int jpeg_out_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *c = (jpeg_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    int bw = rect->right - rect->left + 1;
    int bh = rect->bottom - rect->top + 1;

    /* Nearest-neighbor downscale each decoded MCU tile into the canvas.
     * For each output pixel we check if there's a source pixel in THIS
     * tile that maps to it; if so, write it. Rows/cols outside this
     * tile are handled by other tile calls. */
    for (int by = 0; by < bh; by++) {
        int src_y = rect->top + by;
        int out_y = (src_y * c->canvas_h) / c->src_h;
        if (out_y < 0 || out_y >= c->canvas_h) continue;
        /* Only emit if this source row is the "first" for that out_y. */
        int first_src_y_for_out = (out_y * c->src_h) / c->canvas_h;
        if (src_y != first_src_y_for_out) continue;

        for (int bx = 0; bx < bw; bx++) {
            int src_x = rect->left + bx;
            int out_x = (src_x * c->canvas_w) / c->src_w;
            if (out_x < 0 || out_x >= c->canvas_w) continue;
            int first_src_x_for_out = (out_x * c->src_w) / c->canvas_w;
            if (src_x != first_src_x_for_out) continue;

            const uint8_t *px = &src[(by * bw + bx) * 3];
            /* Byte-swapped RGB565 for LV_COLOR_16_SWAP (matches the GIF
             * decoder output format so the canvas renders identically). */
            uint16_t pxl = ((px[0] >> 3) << 11)
                         | ((px[1] >> 2) << 5)
                         |  (px[2] >> 3);
            c->canvas[out_y * c->canvas_w + out_x] = (pxl >> 8) | (pxl << 8);
        }
    }
    return 1;
}

static esp_err_t show_jpeg(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return ESP_FAIL;

    /* Reasonable working buffer size for tjpgd's LUT + state. */
    static uint8_t tjpgd_work[32768] __attribute__((aligned(4)));

    jpeg_ctx_t ctx = {
        .fp = fp,
        .canvas = s_ctx.canvas_buffer,
        .canvas_w = s_ctx.canvas_width,
        .canvas_h = s_ctx.canvas_height,
    };

    JDEC jd;
    JRESULT r = gif_jd_prepare(&jd, jpeg_in_cb, tjpgd_work, sizeof(tjpgd_work), &ctx);
    if (r != JDR_OK) {
        ESP_LOGE(TAG, "JPEG header parse failed for %s (jdr=%d)", path, r);
        fclose(fp);
        return ESP_FAIL;
    }
    ctx.src_w = jd.width;
    ctx.src_h = jd.height;

    /* Clear canvas before decode so untouched areas don't show stale data. */
    memset(s_ctx.canvas_buffer, 0x10,
           s_ctx.canvas_width * s_ctx.canvas_height * 2);

    r = gif_jd_decomp(&jd, jpeg_out_cb, 0);
    fclose(fp);
    if (r != JDR_OK) {
        ESP_LOGE(TAG, "JPEG decode failed (jdr=%d)", r);
        return ESP_FAIL;
    }

    /* Push to display. No timer — this is a static preview. */
    bsp_display_lock(0);
    lv_canvas_set_buffer(s_ctx.canvas, s_ctx.canvas_buffer,
                         s_ctx.canvas_width, s_ctx.canvas_height,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_invalidate(s_ctx.canvas);
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t app_gifs_play_current(void)
{
    if (s_ctx.count == 0 || !s_ctx.canvas_buffer) {
        ESP_LOGE(TAG, "Cannot play: count=%d canvas=%p",
                 s_ctx.count, s_ctx.canvas_buffer);
        return ESP_FAIL;
    }

    app_gifs_stop();

    const gallery_entry_t *ent = &s_ctx.entries[s_ctx.current_index];
    const char *primary = entry_primary_path(ent);
    ESP_LOGI(TAG, "Gallery entry %d/%d: %s [%s]%s",
             s_ctx.current_index + 1, s_ctx.count, primary,
             ent->type == GALLERY_ENTRY_GIF ? "GIF" : "JPEG preview",
             (ent->type == GALLERY_ENTRY_GIF && ent->jpeg_path) ?
                 " (with JPEG preview)" : "");

    /* Refresh the on-screen name overlay for this entry, and toggle the
     * "PROCESSING" center badge for JPEG-preview entries (captures whose
     * GIF encode isn't finished yet). */
    compute_display_name(primary, s_ctx.current_label, sizeof(s_ctx.current_label));
    bsp_display_lock(0);
    if (s_ctx.name_label) {
        lv_label_set_text(s_ctx.name_label, s_ctx.current_label);
        lv_obj_clear_flag(s_ctx.name_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ctx.name_label);
    }
    if (s_ctx.processing_label) {
        if (ent->type == GALLERY_ENTRY_JPEG) {
            lv_obj_clear_flag(s_ctx.processing_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_ctx.processing_label);
        } else {
            lv_obj_add_flag(s_ctx.processing_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    bsp_display_unlock();

    if (ent->type == GALLERY_ENTRY_JPEG) {
        /* Static JPEG preview — decode once, no timer. */
        return show_jpeg(ent->jpeg_path);
    }

    /* GIF entry. If we have a preview JPEG for this capture, flash it
     * onto the canvas *right now* so the user sees something within
     * ~100 ms instead of staring at the last frame of the previous GIF
     * for the ~700-2000 ms it takes the decoder's first frame to land.
     * The playback timer will overwrite the JPEG with the first GIF
     * frame as soon as the decoder produces it. */
    if (ent->jpeg_path) {
        esp_err_t jret = show_jpeg(ent->jpeg_path);
        if (jret != ESP_OK) {
            ESP_LOGW(TAG, "JPEG preview flash failed: %s", esp_err_to_name(jret));
        }
    }

    /* GIF — open the decoder and start the frame-paced loop. */
    s_ctx.diag_frame_no = 0;
    esp_err_t ret = gif_decoder_open(ent->gif_path, &s_ctx.decoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open GIF: %s", esp_err_to_name(ret));
        return ret;
    }
    s_ctx.decode_width = gif_decoder_get_width(s_ctx.decoder);
    s_ctx.decode_height = gif_decoder_get_height(s_ctx.decoder);

    ESP_LOGI(TAG, "GIF: %dx%d → canvas %dx%d",
             s_ctx.decode_width, s_ctx.decode_height,
             s_ctx.canvas_width, s_ctx.canvas_height);

    /* Show the "loading..." overlay during first-loop decoding; it self-
     * hides the moment the decoder wraps back to frame 0 in the playback
     * timer callback (meaning all frames are now cached). */
    show_loading_overlay();

    s_ctx.is_playing = true;
    s_ctx.play_timer = lv_timer_create(playback_timer_cb, 100, NULL);
    if (!s_ctx.play_timer) {
        app_gifs_stop();
        return ESP_FAIL;
    }
    playback_timer_cb(s_ctx.play_timer);
    return ESP_OK;
}

void app_gifs_stop(void)
{
    s_ctx.is_playing = false;

    if (s_ctx.play_timer) {
        lv_timer_del(s_ctx.play_timer);
        s_ctx.play_timer = NULL;
    }
    /* Hide the loading overlay + cancel its animation timer. Safe no-op
     * if it wasn't shown (e.g. we stopped mid-JPEG preview). */
    hide_loading_overlay();

    if (s_ctx.decoder) {
        gif_decoder_close(s_ctx.decoder);
        s_ctx.decoder = NULL;
    }

    /* Free per-GIF frame cache. Each canvas is 115 KB in PSRAM — leaving
     * them around after stop would eat several hundred KB for a file
     * we're no longer viewing. */
    for (int i = 0; i < s_ctx.frame_cache_n; i++) {
        if (s_ctx.frame_cache[i].canvas) {
            heap_caps_free(s_ctx.frame_cache[i].canvas);
        }
    }
    memset(s_ctx.frame_cache, 0, sizeof(s_ctx.frame_cache));
    s_ctx.frame_cache_n = 0;
}

bool app_gifs_is_playing(void) { return s_ctx.is_playing; }

esp_err_t app_gifs_create_from_album(int frame_delay_ms, int max_frames)
{
    if (s_ctx.is_encoding) {
        ESP_LOGW(TAG, "Encoding already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    encode_task_params_t *params = malloc(sizeof(encode_task_params_t));
    if (!params) return ESP_ERR_NO_MEM;
    params->frame_delay_ms = (frame_delay_ms > 0) ? frame_delay_ms : 300;
    params->max_frames = (max_frames > 0) ? max_frames : 0;

    s_ctx.is_encoding = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        encode_task, "gif_encode", 16384, params, 5, NULL, 1);

    if (ret != pdPASS) {
        free(params);
        s_ctx.is_encoding = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool app_gifs_is_encoding(void) { return s_ctx.is_encoding; }

/* ---- PIMSLO stereoscopic GIF ---- */

typedef struct {
    int frame_delay_ms;
    float parallax;
} pimslo_task_params_t;

/* ---- Reusable PIMSLO encode function ---- */
#define MAX_PIMSLO_CAMS 4

esp_err_t app_gifs_encode_pimslo_from_dir(const char *capture_dir,
                                           int frame_delay_ms, float parallax)
{
    esp_err_t ret = ESP_FAIL;
    int delay_cs = (frame_delay_ms > 0 ? frame_delay_ms : 150) / 10;
    float strength = (parallax > 0.0f && parallax <= 1.0f) ? parallax : 0.05f;

    s_ctx.is_encoding = true;

    /* Read JPEGs from the capture directory — support 2-4 cameras */
    uint8_t *jpeg_data[MAX_PIMSLO_CAMS] = {NULL};
    size_t jpeg_size[MAX_PIMSLO_CAMS] = {0};
    int num_cams = 0;

    for (int i = 0; i < MAX_PIMSLO_CAMS; i++) {
        char path[80];
        snprintf(path, sizeof(path), "%s/pos%d.jpg", capture_dir, i + 1);
        FILE *f = fopen(path, "rb");
        if (!f) {
            ESP_LOGW(TAG, "No file %s — using %d cameras", path, i);
            break;
        }
        fseek(f, 0, SEEK_END);
        jpeg_size[i] = ftell(f);
        fseek(f, 0, SEEK_SET);
        jpeg_data[i] = heap_caps_malloc(jpeg_size[i], MALLOC_CAP_SPIRAM);
        if (!jpeg_data[i]) {
            fclose(f);
            ESP_LOGE(TAG, "OOM for %s (%zu bytes)", path, jpeg_size[i]);
            break;
        }
        fread(jpeg_data[i], 1, jpeg_size[i], f);
        fclose(f);
        ESP_LOGI(TAG, "Loaded %s: %zu bytes", path, jpeg_size[i]);
        num_cams++;
    }

    if (num_cams < 2) {
        ESP_LOGE(TAG, "Need at least 2 cameras, found %d", num_cams);
        goto cleanup;
    }

    /* Get source dimensions from first JPEG */
    jpeg_decode_picture_info_t info;
    ret = jpeg_decoder_get_info(jpeg_data[0], jpeg_size[0], &info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read JPEG header");
        goto cleanup;
    }

    int src_w = info.width, src_h = info.height;

    /* Square center crop with parallax applied within.
     * Use the shorter dimension as the square size, then apply parallax
     * as horizontal offset within the square. This keeps the output
     * buffer under ~7.4MB (fits in PSRAM's largest free block). */
    int square = (src_w < src_h) ? src_w : src_h;
    int h_margin = (src_w - square) / 2;  /* Horizontal center offset */
    int v_margin = (src_h - square) / 2;  /* Vertical center offset */
    int total_parallax = (int)(square * strength);
    int crop_w = square - total_parallax;

    ESP_LOGI(TAG, "PIMSLO: %dx%d source → %dx%d square, %d cameras, parallax=%.2f, crop_w=%d",
             src_w, src_h, square, square, num_cams, strength, crop_w);

    /* Calculate parallax crop rects for each position */
    gif_crop_rect_t crops[MAX_PIMSLO_CAMS];
    for (int i = 0; i < num_cams; i++) {
        float crop_ratio = (num_cams > 1) ? (float)i / (num_cams - 1) : 0.0f;
        int parallax_offset = (int)(crop_ratio * total_parallax);
        crops[i].x = h_margin + parallax_offset;
        crops[i].y = v_margin;
        crops[i].w = crop_w;
        crops[i].h = square;
        ESP_LOGI(TAG, "  Pos %d: crop(%d, %d, %d, %d)",
                 i+1, crops[i].x, crops[i].y, crop_w, square);
    }

    /* Free camera buffers for PSRAM */
    app_video_stream_free_buffers();

    /* Release album's JPEG decoder — ESP32-P4 has only one HW JPEG decoder.
     * Without this, gif_encoder_create() gets a handle but jpeg_decoder_process()
     * returns ESP_ERR_INVALID_STATE (0x103) because the HW is owned by the album. */
    app_album_release_jpeg_decoder();

    /* Free the JPEG data buffers — they'll be re-read from SD for each pass */
    for (int i = 0; i < num_cams; i++) {
        heap_caps_free(jpeg_data[i]);
        jpeg_data[i] = NULL;
    }

    /* Create encoder — square crop at full resolution */
    gif_encoder_config_t cfg = {
        .frame_delay_cs = delay_cs,
        .loop_count = 0,
        .target_width = crop_w,
        .target_height = square,
    };
    ESP_LOGI(TAG, "PIMSLO GIF: %dx%d (square crop, full resolution)", crop_w, square);

    gif_encoder_t *enc = NULL;
    ret = gif_encoder_create(&cfg, &enc);
    if (ret != ESP_OK) goto cleanup_buffers;

    /* Pass 1: Build palette from all unique frames (re-read each from SD) */
    for (int i = 0; i < num_cams; i++) {
        char path[80];
        snprintf(path, sizeof(path), "%s/pos%d.jpg", capture_dir, i + 1);
        FILE *f = fopen(path, "rb");
        if (!f) { ESP_LOGW(TAG, "Cannot reopen %s", path); continue; }
        fseek(f, 0, SEEK_END);
        size_t sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *data = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (!data) { fclose(f); continue; }
        fread(data, 1, sz, f);
        fclose(f);
        ret = gif_encoder_pass1_add_frame_from_buffer(enc, data, sz, &crops[i]);
        heap_caps_free(data);
        if (ret != ESP_OK)
            ESP_LOGW(TAG, "Pass 1 failed for pos %d", i+1);
    }
    ret = gif_encoder_pass1_finalize(enc);
    if (ret != ESP_OK) goto cleanup_enc;

    /* Pass 2: Encode oscillating sequence with frame replay optimization.
     * Forward pass (0→num_cams-1): full decode + dither + LZW encode
     * Reverse pass: replay the already-encoded frame bytes from the GIF file
     *
     * For 4 cameras: forward = 1,2,3,4, reverse = 3,2,1 (replayed)
     * Saves ~5s per replayed frame = ~15s for 4 cameras */
    ensure_gif_dir();
    char output_path[MAX_PATH_LEN];
    /* Extract capture name from directory (e.g. "/sdcard/p4mslo/P4M0001" → "P4M0001") */
    const char *dir_name = strrchr(capture_dir, '/');
    dir_name = dir_name ? dir_name + 1 : capture_dir;
    snprintf(output_path, sizeof(output_path), "%s/%s/%s.gif",
             BSP_SD_MOUNT_POINT, GIF_FOLDER_NAME, dir_name);

    ret = gif_encoder_pass2_begin(enc, output_path);
    if (ret != ESP_OK) goto cleanup_enc;

    /* Forward pass: encode each unique camera frame.
     * Cache the middle frames' GIF data in PSRAM for instant replay
     * during the reverse pass (avoids SD card read-back). */
    uint8_t *frame_cache[MAX_PIMSLO_CAMS];
    size_t   frame_cache_size[MAX_PIMSLO_CAMS];
    memset(frame_cache, 0, sizeof(frame_cache));
    memset(frame_cache_size, 0, sizeof(frame_cache_size));

    for (int i = 0; i < num_cams; i++) {
        long start_pos = gif_encoder_get_file_pos(enc);

        char path[80];
        snprintf(path, sizeof(path), "%s/pos%d.jpg", capture_dir, i + 1);
        FILE *ff = fopen(path, "rb");
        if (!ff) { ESP_LOGW(TAG, "Cannot reopen %s", path); continue; }
        fseek(ff, 0, SEEK_END);
        size_t sz = ftell(ff);
        fseek(ff, 0, SEEK_SET);
        uint8_t *data = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (!data) { fclose(ff); continue; }
        fread(data, 1, sz, ff);
        fclose(ff);
        ret = gif_encoder_pass2_add_frame_from_buffer(enc, data, sz, &crops[i]);
        heap_caps_free(data);

        long end_pos = gif_encoder_get_file_pos(enc);
        size_t frame_len = (size_t)(end_pos - start_pos);

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Pass 2 encode failed for pos %d", i + 1);
            continue;
        }

        /* Cache middle frames (not first/last) in PSRAM for reverse pass.
         * First frame (i=0) and last frame (i=num_cams-1) are endpoints
         * of the oscillation and don't appear in the reverse. */
        if (i > 0 && i < num_cams - 1) {
            frame_cache[i] = heap_caps_malloc(frame_len, MALLOC_CAP_SPIRAM);
            if (frame_cache[i]) {
                /* Read back the just-written frame data from the GIF file */
                esp_err_t rb = gif_encoder_read_back(enc, start_pos,
                                                      frame_cache[i], frame_len);
                if (rb == ESP_OK) {
                    frame_cache_size[i] = frame_len;
                    ESP_LOGI(TAG, "Cached frame %d: %zu bytes in PSRAM", i + 1, frame_len);
                } else {
                    heap_caps_free(frame_cache[i]);
                    frame_cache[i] = NULL;
                }
            }
        }
    }

    /* Reverse pass: write cached frames directly from PSRAM (no SD read) */
    int reverse_count = 0;
    uint32_t t_replay = esp_log_timestamp();
    for (int i = num_cams - 2; i >= 1; i--) {
        if (frame_cache[i] && frame_cache_size[i] > 0) {
            gif_encoder_pass2_write_raw_frame(enc, frame_cache[i], frame_cache_size[i]);
            heap_caps_free(frame_cache[i]);
            frame_cache[i] = NULL;
            reverse_count++;
        }
    }
    ESP_LOGI(TAG, "Replayed %d frames from PSRAM in %lums",
             reverse_count, (unsigned long)(esp_log_timestamp() - t_replay));

    ret = gif_encoder_pass2_finish(enc);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PIMSLO GIF saved to %s", output_path);
    }

cleanup_enc:
    gif_encoder_destroy(enc);
cleanup_buffers:
    app_album_reacquire_jpeg_decoder();
    app_video_stream_realloc_buffers();
cleanup:
    for (int i = 0; i < MAX_PIMSLO_CAMS; i++) {
        if (jpeg_data[i]) heap_caps_free(jpeg_data[i]);
    }
    s_ctx.is_encoding = false;
    app_gifs_scan();
    return ret;
}

/* Legacy task wrapper for serial command backward compatibility */
static void pimslo_encode_task(void *param)
{
    pimslo_task_params_t *p = (pimslo_task_params_t *)param;
    app_gifs_encode_pimslo_from_dir("/sdcard/pimslo",
                                     p->frame_delay_ms, p->parallax);
    free(p);
    vTaskDelete(NULL);
}

esp_err_t app_gifs_create_pimslo(int frame_delay_ms, float parallax)
{
    if (s_ctx.is_encoding) return ESP_ERR_INVALID_STATE;

    pimslo_task_params_t *params = malloc(sizeof(*params));
    if (!params) return ESP_ERR_NO_MEM;
    params->frame_delay_ms = (frame_delay_ms > 0) ? frame_delay_ms : 150;
    params->parallax = (parallax > 0.0f && parallax <= 1.0f) ? parallax : 0.05f;

    s_ctx.is_encoding = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        pimslo_encode_task, "pimslo_enc", 16384, params, 5, NULL, 1);
    if (ret != pdPASS) {
        free(params);
        s_ctx.is_encoding = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}
