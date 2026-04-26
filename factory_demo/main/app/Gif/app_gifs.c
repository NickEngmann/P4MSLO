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
#include "ui_extra.h"
/* ui_extra_get_sd_card_mounted lives in ui_extra.h already (above). */
#include "ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/esp32_p4_eye.h"     /* bsp_get_sdcard_handle for format_sd */
#include "esp_vfs_fat.h"          /* esp_vfs_fat_sdcard_format */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  /* xTaskCreatePinnedToCoreWithCaps */
#include "app_video_stream.h"
#include "app_album.h"

static const char *TAG = "app_gifs";

/* Shared tjpgd working buffer — used by both show_jpeg() (gallery
 * preview flash onto the LVGL canvas, LVGL task) and
 * decode_jpeg_crop_to_canvas() (.p4ms preview save, pimslo_save_task
 * on Core 1).
 *
 * tjpgd needs 32 KB in internal RAM for its Huffman tables + IDCT
 * state + MCU scratch. Previously show_jpeg had a function-local
 * `static uint8_t tjpgd_work[32768]` and decode_jpeg_crop_to_canvas
 * used a transient `heap_caps_malloc(32768, MALLOC_CAP_INTERNAL)`.
 * The transient alloc started OOM'ing once the gallery was open +
 * the P4 photo save ran SD writes + SPI camera allocated its
 * permanent DMA-internal buffers — largest-free-block could drop
 * to 4-7 KB by the time decode_jpeg_crop fired, well below the
 * 32 KB needed. That made the .p4ms preview save silently fail on
 * every capture under normal e2e test pressure.
 *
 * Since the codebase already has TWO 32 KB statics in BSS for
 * tjpgd (show_jpeg's local + gif_encoder.c's `tjwork[32768]`),
 * promoting show_jpeg's local to file scope and sharing with
 * decode_jpeg_crop is a net-zero BSS change. A mutex prevents
 * races between the LVGL task and pimslo_save_task. */
static uint8_t s_tjpgd_work[32768] __attribute__((aligned(4)));
static SemaphoreHandle_t s_tjpgd_mutex = NULL;

/* Lazy-init the mutex on first acquire — gif_encoder.c uses this
 * during PIMSLO encode, which can run before the gallery has been
 * opened (and thus before app_gifs_init() has fired). */
static void ensure_tjpgd_mutex(void)
{
    if (!s_tjpgd_mutex) s_tjpgd_mutex = xSemaphoreCreateMutex();
}

uint8_t *app_gifs_acquire_tjpgd_work(uint32_t timeout_ms, size_t *out_size)
{
    ensure_tjpgd_mutex();
    if (s_tjpgd_mutex && xSemaphoreTake(s_tjpgd_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return NULL;
    }
    if (out_size) *out_size = sizeof(s_tjpgd_work);
    return s_tjpgd_work;
}

void app_gifs_release_tjpgd_work(void)
{
    if (s_tjpgd_mutex) xSemaphoreGive(s_tjpgd_mutex);
}

/* Compile-time switch for per-step timing in the gallery play_current /
 * show_jpeg hot paths. Off by default (zero overhead — the compiler
 * elides the esp_log_timestamp() calls and the ESP_LOGI lines). Flip
 * to 1 when investigating a slow-load regression. Don't confuse with
 * ESP_LOG_DEBUG — that gates the log line only; the timestamp calls
 * still execute, which would taint the measurements. */
#ifndef APP_GIFS_TIMING
#define APP_GIFS_TIMING 0
#endif
#if APP_GIFS_TIMING
#define TIMING_MARK(name)   uint32_t name = esp_log_timestamp()
#define TIMING_LOG(...)     ESP_LOGI(TAG, __VA_ARGS__)
#else
#define TIMING_MARK(name)   ((void)0)
#define TIMING_LOG(...)     ((void)0)
#endif

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

typedef struct gif_cache_slot {
    char *gif_path;                              /* NULL if slot empty */
    cached_frame_t frames[MAX_FRAMES_PER_GIF];
    int n_frames;                                /* number of canvases cached so far */

    /* Playback order as a sequence of indices into frames[]. Populated
     * on the first pass through a GIF. For a 6-frame palindrome like
     * PIMSLO's this is [0,1,2,3,2,1] — 6 entries pointing at 4 unique
     * canvases. Persisted alongside the canvases in the .p4ms file so
     * the prerendered fast path can replay without re-opening the GIF. */
    uint8_t playback_order[MAX_FRAMES_PER_GIF * 2];
    int playback_count;

    bool complete;                               /* true once wrap-around seen */
    bool from_disk;                              /* loaded from .p4ms, no decoder needed */
    int playback_pos;                            /* for the no-decoder "cached-only" mode */

    int64_t last_used_us;
} gif_cache_slot_t;

/* ---- Prerendered small-GIF cache on SD card ------------------------
 *
 * After the decoder's first loop completes (we have the full hash-
 * deduped canvas set + playback order), we persist a minimal binary
 * representation of the 240×240 version into /sdcard/p4mslo_small/.
 * Next time the user visits that entry — even after a reboot — the
 * gallery loads the .p4ms directly, skipping gif_decoder_open's
 * 3.5 MB pixel_indices allocation and the ~600-700 ms/frame LZW
 * decode. Playback starts instantly at native framerate.
 *
 * Format is deliberately trivial (not a real GIF):
 *   [P4MS header, 16 bytes]
 *   [unique canvas × n_unique]           — 115 200 bytes each
 *   [per-frame meta × n_playback]        — delay_cs + unique_idx
 */
#define P4MS_DIR     "/sdcard/p4mslo_small"
#define P4MS_MAGIC   "P4MS"
#define P4MS_VERSION 1

typedef struct __attribute__((packed)) {
    char     magic[4];      /* "P4MS" */
    uint8_t  version;       /* 1 */
    uint8_t  reserved[3];
    uint16_t width;         /* 240 */
    uint16_t height;        /* 240 */
    uint16_t n_unique;      /* number of distinct canvases stored */
    uint16_t n_playback;    /* length of the playback sequence */
} p4ms_header_t;

typedef struct __attribute__((packed)) {
    uint16_t unique_idx;    /* index into the stored canvas array */
    uint16_t delay_cs;
} p4ms_frame_meta_t;

static gif_cache_slot_t g_gif_cache[MAX_CACHED_GIFS];

/* ---- Background-worker shared state --------------------------------
 *
 * These coordinate the bg pre-render / encode task with foreground
 * playback. They're declared up here (not next to the task) so
 * app_gifs_play_current() can set s_bg_abort_current when it opens a
 * decoder — that's the signal the bg task polls between frames. */
static volatile bool s_gallery_open = false;
static volatile bool s_bg_abort_current = false;

/* Forward declaration of the bg worker task handle. The actual
 * definition lives further down next to app_gifs_start_background_worker.
 * Needed up here because app_gifs_encode_pimslo_from_dir gates its
 * mid-encode abort check on `xTaskGetCurrentTaskHandle() == s_bg_worker`
 * — the pimslo_encode_queue_task must run to completion even during
 * gallery nav, while bg_worker's optional re-encode can bail. */
static TaskHandle_t s_bg_worker = NULL;

/* Timestamp (ms) of the last gallery-nav call (app_gifs_next / _prev).
 * bg_worker uses this to throttle: a new ~50-second PIMSLO encode must
 * not start while the user is actively scrubbing through entries,
 * otherwise the encode's 7 MB PSRAM + JPEG-decoder + SD-I/O load
 * stalls gallery playback for ~50 s and knob nav feels locked up.
 * After NAV_QUIET_MS of no nav, bg_worker is free to start new work.
 * 15 s is long enough to comfortably cover a human scrubbing through
 * 5-10 entries — anyone actively browsing won't trip bg work. */
static volatile uint32_t s_last_nav_ms = 0;
#define NAV_QUIET_MS 15000

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

    /* Points into g_gif_cache[] — the slot for the currently-playing
     * GIF. Persists across stop()/play_current() so navigating away and
     * coming back doesn't force a re-decode. Set by play_current()
     * after finding or allocating a slot; cleared to NULL between plays
     * or when the slot gets LRU-evicted. */
    struct gif_cache_slot *active_slot;

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
     * will replace it once encoding finishes. Text is "PROCESSING"
     * when this specific entry is actively being encoded right now,
     * "QUEUED" if it's waiting in the queue. Hidden for GIF entries. */
    lv_obj_t *processing_label;

    /* Shown when the gallery has zero entries. Also used to surface an
     * SD-card error state so the user isn't left wondering why their
     * photos aren't showing up. Single label, text swapped based on
     * reason. */
    lv_obj_t *empty_label;

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

/* Wrapper for lv_async_call so bg tasks can request a gallery rescan
 * without racing LVGL on s_ctx.entries. Also auto-resumes playback if
 * the user is still parked on the gallery — the bg encode path stops
 * playback + flushes the canvas cache before it allocates its 7 MB
 * scaled_buf, and without this the display would stay frozen on the
 * last pre-encode frame. Safe no-op on every other page. */
static void scan_async_cb(void *arg)
{
    (void)arg;
    app_gifs_scan();
    if (ui_extra_get_current_page() == UI_PAGE_GIFS) {
        /* Always refresh — the count may have just transitioned from
         * 0 (overlay visible) to >0 (overlay must hide). Without
         * this, the "Album empty" label persists on top of the
         * newly-encoded entry. */
        app_gifs_refresh_empty_overlay();
        if (app_gifs_get_count() > 0) {
            app_gifs_play_current();
        }
    }
}

/* Toggle the empty-album / SD-error label visibility based on the
 * current entry count and SD state. Safe to call from any task — uses
 * bsp_display_lock internally. */
void app_gifs_refresh_empty_overlay(void)
{
    if (!s_ctx.empty_label) return;
    bool sd_ok = ui_extra_get_sd_card_mounted();
    bool empty = (s_ctx.count == 0);
    bsp_display_lock(0);
    if (!sd_ok) {
        lv_label_set_text(s_ctx.empty_label,
                          "SD card\nnot detected\n\nInsert a card\nand reboot");
        lv_obj_clear_flag(s_ctx.empty_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ctx.empty_label);
    } else if (empty) {
        lv_label_set_text(s_ctx.empty_label,
                          "Album empty\n\nTake a photo\nfrom the camera");
        lv_obj_clear_flag(s_ctx.empty_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ctx.empty_label);
    } else {
        lv_obj_add_flag(s_ctx.empty_label, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_display_unlock();
}

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

/* ---- Global GIF canvas cache (across-gallery-entries) ---- */

static int64_t now_us(void) { return esp_timer_get_time(); }

static gif_cache_slot_t *slot_find(const char *gif_path)
{
    for (int i = 0; i < MAX_CACHED_GIFS; i++) {
        if (g_gif_cache[i].gif_path &&
            strcmp(g_gif_cache[i].gif_path, gif_path) == 0) {
            return &g_gif_cache[i];
        }
    }
    return NULL;
}

static void slot_free(gif_cache_slot_t *s)
{
    if (!s || !s->gif_path) return;
    for (int i = 0; i < MAX_FRAMES_PER_GIF; i++) {
        if (s->frames[i].canvas) {
            heap_caps_free(s->frames[i].canvas);
            s->frames[i].canvas = NULL;
        }
    }
    free(s->gif_path);
    s->gif_path = NULL;
    s->n_frames = 0;
    s->playback_count = 0;
    s->complete = false;
    s->from_disk = false;
    s->playback_pos = 0;
    s->last_used_us = 0;
}

/* Evict the least-recently-used populated slot and return it (freed
 * and ready to be re-allocated by the caller). */
static gif_cache_slot_t *slot_evict_lru(void)
{
    gif_cache_slot_t *oldest = NULL;
    for (int i = 0; i < MAX_CACHED_GIFS; i++) {
        if (!g_gif_cache[i].gif_path) continue;
        if (!oldest || g_gif_cache[i].last_used_us < oldest->last_used_us) {
            oldest = &g_gif_cache[i];
        }
    }
    if (oldest) {
        ESP_LOGI(TAG, "LRU-evicting slot for %s (n_frames=%d)",
                 oldest->gif_path, oldest->n_frames);
        slot_free(oldest);
    }
    return oldest;
}

/* Find or allocate a slot for the given GIF path. Allocates a new empty
 * slot (possibly evicting LRU) if none exists for this path. Updates
 * last_used_us so slot_evict_lru won't pick this one next. */
static gif_cache_slot_t *slot_find_or_alloc(const char *gif_path)
{
    gif_cache_slot_t *s = slot_find(gif_path);
    if (s) {
        s->last_used_us = now_us();
        return s;
    }
    /* Find an empty slot first. */
    for (int i = 0; i < MAX_CACHED_GIFS; i++) {
        if (!g_gif_cache[i].gif_path) {
            s = &g_gif_cache[i];
            s->gif_path = strdup(gif_path);
            s->n_frames = 0;
            s->playback_count = 0;
            s->complete = false;
            s->from_disk = false;
            s->playback_pos = 0;
            s->last_used_us = now_us();
            return s;
        }
    }
    /* All slots full — evict LRU and take it. */
    s = slot_evict_lru();
    if (s) {
        s->gif_path = strdup(gif_path);
        s->n_frames = 0;
        s->complete = false;
        s->last_used_us = now_us();
    }
    return s;
}

/* Flush the entire global cache. Called when leaving the gallery so
 * the ~3 MB of pinned PSRAM is available for camera / GIF-encoder work. */
void app_gifs_flush_cache(void)
{
    int freed = 0;
    for (int i = 0; i < MAX_CACHED_GIFS; i++) {
        if (g_gif_cache[i].gif_path) {
            slot_free(&g_gif_cache[i]);
            freed++;
        }
    }
    if (freed) ESP_LOGI(TAG, "Flushed %d cached GIF slot(s)", freed);
}

/* ---- Prerendered .p4ms persistence ---------------------------------- */

static void ensure_small_dir(void) { mkdir(P4MS_DIR, 0755); }

/* Given a GIF path like "/sdcard/p4mslo_gifs/P4M0007.gif", compute the
 * matching .p4ms path "/sdcard/p4mslo_small/P4M0007.p4ms". Returns false
 * if the source doesn't have a usable basename + extension. */
static bool gif_path_to_small_path(const char *gif_path,
                                    char *out, size_t out_cap)
{
    const char *slash = strrchr(gif_path, '/');
    const char *base = slash ? slash + 1 : gif_path;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return false;
    size_t stem_len = (size_t)(dot - base);
    if (stem_len == 0) return false;
    int n = snprintf(out, out_cap, "%s/%.*s.p4ms",
                     P4MS_DIR, (int)stem_len, base);
    return n > 0 && (size_t)n < out_cap;
}

static bool small_file_exists(const char *gif_path)
{
    char path[MAX_PATH_LEN];
    if (!gif_path_to_small_path(gif_path, path, sizeof(path))) return false;
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > (off_t)sizeof(p4ms_header_t);
}

/* Persist the slot's unique canvases + playback order to .p4ms. Called
 * once per GIF, right when its cache slot becomes `complete`. On success
 * subsequent visits skip the decoder entirely. Writes atomically via a
 * .tmp suffix → rename so a power cut during write can't leave a
 * truncated file masquerading as valid. */
static esp_err_t save_small_gif(const gif_cache_slot_t *slot)
{
    if (!slot || !slot->gif_path || !slot->complete) return ESP_ERR_INVALID_STATE;
    if (slot->n_frames <= 0 || slot->playback_count <= 0) return ESP_ERR_INVALID_STATE;
    if (slot->from_disk) return ESP_OK;  /* loaded from disk — already persisted */

    char final_path[MAX_PATH_LEN];
    if (!gif_path_to_small_path(slot->gif_path, final_path, sizeof(final_path))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (small_file_exists(slot->gif_path)) return ESP_OK;  /* already on disk */

    ensure_small_dir();

    char tmp_path[MAX_PATH_LEN + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "save_small_gif: fopen(%s) failed", tmp_path);
        return ESP_FAIL;
    }

    p4ms_header_t hdr = {
        .magic   = { 'P', '4', 'M', 'S' },
        .version = P4MS_VERSION,
        .reserved = {0, 0, 0},
        .width   = (uint16_t)s_ctx.canvas_width,
        .height  = (uint16_t)s_ctx.canvas_height,
        .n_unique   = (uint16_t)slot->n_frames,
        .n_playback = (uint16_t)slot->playback_count,
    };
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) goto fail;

    size_t canvas_bytes = (size_t)s_ctx.canvas_width * s_ctx.canvas_height * 2;
    for (int i = 0; i < slot->n_frames; i++) {
        if (!slot->frames[i].canvas) goto fail;
        if (fwrite(slot->frames[i].canvas, 1, canvas_bytes, f) != canvas_bytes) goto fail;
    }

    for (int i = 0; i < slot->playback_count; i++) {
        uint8_t idx = slot->playback_order[i];
        if (idx >= slot->n_frames) goto fail;
        p4ms_frame_meta_t meta = {
            .unique_idx = idx,
            .delay_cs = (uint16_t)slot->frames[idx].delay_cs,
        };
        if (fwrite(&meta, sizeof(meta), 1, f) != 1) goto fail;
    }

    fflush(f);
    fclose(f);

    /* Atomic swap into place. */
    if (rename(tmp_path, final_path) != 0) {
        ESP_LOGW(TAG, "save_small_gif: rename(%s → %s) failed",
                 tmp_path, final_path);
        remove(tmp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved small GIF: %s (%d unique × %d playback, %zu bytes canvas each)",
             final_path, slot->n_frames, slot->playback_count, canvas_bytes);
    return ESP_OK;

fail:
    fclose(f);
    remove(tmp_path);
    ESP_LOGW(TAG, "save_small_gif: write failed for %s", final_path);
    return ESP_FAIL;
}

/* Load a prerendered .p4ms into the given slot. On success the slot is
 * ready for cached-only playback: n_frames = unique canvases, playback
 * sequence in playback_order[], complete=true, from_disk=true. */
static esp_err_t load_small_gif(const char *gif_path, gif_cache_slot_t *slot)
{
    if (!slot) return ESP_ERR_INVALID_ARG;

    char path[MAX_PATH_LEN];
    if (!gif_path_to_small_path(gif_path, path, sizeof(path))) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    p4ms_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return ESP_FAIL; }
    if (memcmp(hdr.magic, P4MS_MAGIC, 4) != 0 || hdr.version != P4MS_VERSION) {
        ESP_LOGW(TAG, "load_small_gif: bad magic/version in %s", path);
        fclose(f);
        return ESP_FAIL;
    }
    if (hdr.width != s_ctx.canvas_width || hdr.height != s_ctx.canvas_height) {
        ESP_LOGW(TAG, "load_small_gif: dim mismatch %dx%d (have %dx%d)",
                 hdr.width, hdr.height, s_ctx.canvas_width, s_ctx.canvas_height);
        fclose(f);
        return ESP_FAIL;
    }
    if (hdr.n_unique == 0 || hdr.n_unique > MAX_FRAMES_PER_GIF ||
        hdr.n_playback == 0 || hdr.n_playback > MAX_FRAMES_PER_GIF * 2) {
        ESP_LOGW(TAG, "load_small_gif: bad counts u=%u p=%u",
                 hdr.n_unique, hdr.n_playback);
        fclose(f);
        return ESP_FAIL;
    }

    size_t canvas_bytes = (size_t)hdr.width * hdr.height * 2;

    /* Drop any stale canvases first (should already be empty if this is
     * a freshly-allocated slot, but be defensive). */
    for (int i = 0; i < MAX_FRAMES_PER_GIF; i++) {
        if (slot->frames[i].canvas) {
            heap_caps_free(slot->frames[i].canvas);
            slot->frames[i].canvas = NULL;
        }
    }

    for (int i = 0; i < hdr.n_unique; i++) {
        uint16_t *buf = heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM);
        if (!buf) {
            ESP_LOGE(TAG, "load_small_gif: OOM on canvas %d", i);
            goto fail_release;
        }
        if (fread(buf, 1, canvas_bytes, f) != canvas_bytes) {
            heap_caps_free(buf);
            ESP_LOGW(TAG, "load_small_gif: short read on canvas %d", i);
            goto fail_release;
        }
        slot->frames[i].canvas = buf;
        slot->frames[i].hash = 0;  /* not used in from_disk mode */
        slot->frames[i].delay_cs = 10;  /* filled in from meta below */
    }

    for (int i = 0; i < hdr.n_playback; i++) {
        p4ms_frame_meta_t meta;
        if (fread(&meta, sizeof(meta), 1, f) != 1) goto fail_release;
        if (meta.unique_idx >= hdr.n_unique) goto fail_release;
        slot->playback_order[i] = (uint8_t)meta.unique_idx;
        slot->frames[meta.unique_idx].delay_cs = (int)meta.delay_cs;
    }

    fclose(f);

    slot->n_frames = hdr.n_unique;
    slot->playback_count = hdr.n_playback;
    slot->complete = true;
    slot->from_disk = true;
    slot->playback_pos = 0;
    slot->last_used_us = now_us();

    ESP_LOGI(TAG, "Loaded small GIF: %s (%d unique × %d playback)",
             path, hdr.n_unique, hdr.n_playback);
    return ESP_OK;

fail_release:
    fclose(f);
    for (int i = 0; i < MAX_FRAMES_PER_GIF; i++) {
        if (slot->frames[i].canvas) {
            heap_caps_free(slot->frames[i].canvas);
            slot->frames[i].canvas = NULL;
        }
    }
    slot->n_frames = 0;
    slot->playback_count = 0;
    slot->complete = false;
    slot->from_disk = false;
    return ESP_FAIL;
}

/* ---- Direct JPEG → 240×240 canvas (no GIF decoder) ------------------
 *
 * Uses tjpgd with a custom out_cb that applies a source-space crop rect
 * and nearest-neighbor downscales into a target canvas. Memory cost:
 * one 32 KB tjpgd work buffer + the destination canvas. The full-res
 * pixel buffer is never materialized — tjpgd emits MCU tiles and we
 * pick the pixels we care about inline. */

/* tjpgd in_cb pattern is extremely fread-unfriendly (many small 2-8
 * byte marker reads). Slurping the whole file into PSRAM first and
 * serving from memory drops a ~1 MB preview JPEG decode from ~15 s
 * to ~650 ms on this board. Same pattern as show_jpeg's jpeg_ctx_t. */
typedef struct {
    const uint8_t *src;   /* full JPEG bytes, memory-resident */
    size_t src_len;
    size_t src_pos;
    uint16_t *canvas;
    int canvas_w, canvas_h;
    int crop_x, crop_y, crop_w, crop_h;    /* source-space crop rect */
} jpeg_crop_ctx_t;

static size_t jpeg_crop_in_cb(JDEC *jd, uint8_t *buf, size_t len)
{
    jpeg_crop_ctx_t *c = (jpeg_crop_ctx_t *)jd->device;
    size_t avail = (c->src_pos < c->src_len) ? (c->src_len - c->src_pos) : 0;
    if (len > avail) len = avail;
    if (buf) memcpy(buf, c->src + c->src_pos, len);
    c->src_pos += len;
    return len;
}

/* This function runs inside tjpgd's per-MCU callback, invoked 19,200
 * times for a 2560×1920 source JPEG. At -Og the loop body dominated
 * the decode — 30+ seconds per JPEG because every integer divide +
 * bit-pack ran unoptimized. gif_encoder.c's equivalent output_cb is in
 * the file-level -O2 list (see factory_demo/CMakeLists.txt), which is
 * why that path decoded the same JPEGs in ~2 s. Rather than pull the
 * whole app_gifs.c into -O2 (lots of LVGL surface we'd have to
 * re-validate), let the compiler fold just this hot function. Same
 * treatment below for jpeg_out_cb. */
__attribute__((optimize("O2")))
static int jpeg_crop_out_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_crop_ctx_t *c = (jpeg_crop_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    int bw = rect->right - rect->left + 1;
    int bh = rect->bottom - rect->top + 1;

    /* Nearest-neighbor downscale — output-cell-driven iteration.
     * For each canvas cell whose nearest source falls inside this MCU
     * AND inside the crop rect, pluck that one source pixel. This is
     * O(canvas_cells_hit) instead of O(MCU_bw · MCU_bh), which at a
     * 1824×1920 crop → 240×240 canvas is ~58× fewer iterations per
     * MCU. Previously this function was clocking 30-44 s per 2560×1920
     * source JPEG because the old per-src-pixel form did 2 × 32-bit
     * divides PER SOURCE PIXEL (4.9 M px × 2 divides = 10 M divides
     * per JPEG).
     *
     * Writer contract matches the prior form: nearest-neighbor,
     * last-write-wins across MCU boundaries. The earlier "only emit
     * the first src that maps to this out" optimization was wrong
     * for non-integer ratios (integer division of (out*crop/canvas)
     * didn't always round-trip with (src*canvas/crop), leaving some
     * output columns unwritten → dark-blue vertical bars on
     * .p4ms previews). Here we rigorously map each canvas cell to
     * exactly one source pixel via cropped_x/y = out * crop/canvas
     * and touch every cell, so the bars can't come back. */
    const int cx = c->crop_x;
    const int cy = c->crop_y;
    const int cw = c->crop_w;
    const int ch = c->crop_h;
    const int ow = c->canvas_w;
    const int oh = c->canvas_h;

    /* Output Y range: out_y whose src_y (= cy + (out_y * ch) / oh) lands
     * in [rect->top, rect->bottom]. The forward map is
     *    cropped_y = floor(out_y * ch / oh)
     * so the inverse must be the consistent inverse of FLOOR, NOT the
     * floor of the inverse:
     *    out_y_lo = smallest out_y where floor(out_y * ch / oh) >= rel_top
     *             = ceil(rel_top * oh / ch)               [rel_top >= 0]
     *    out_y_hi = largest out_y where floor(out_y * ch / oh) <= rel_bot
     *             = floor((rel_bot + 1) * oh / ch) - 1
     *             = ((rel_bot + 1) * oh - 1) / ch         [integer div, rel_bot >= 0]
     *
     * The earlier `(rel_bot * oh) / ch` form was the floor of the
     * inverse, which loses one canvas cell per MCU when the source
     * pixel that should serve it sits exactly on a multi-of-MCU-width
     * boundary. Concretely on a 1824 → 240 horizontal downscale, that
     * dropped out_x = 2 between MCU [320..335] and MCU [336..351],
     * leaving the canvas's 0x10 fill visible as vertical dark bars on
     * .p4ms previews. The earlier comment in this file claimed this
     * was already fixed — the rewrite was correct in spirit but the
     * upper-bound formula stayed wrong. */
    int rel_top = rect->top - cy;
    int rel_bot = rect->bottom - cy;
    int out_y_lo = (rel_top <= 0) ? 0
                                  : (rel_top * oh + ch - 1) / ch;
    int out_y_hi = (rel_bot <  0) ? -1
                                  : ((rel_bot + 1) * oh - 1) / ch;
    if (out_y_hi >= oh) out_y_hi = oh - 1;

    int rel_left  = rect->left  - cx;
    int rel_right = rect->right - cx;
    int out_x_lo = (rel_left  <= 0) ? 0
                                    : (rel_left  * ow + cw - 1) / cw;
    int out_x_hi = (rel_right <  0) ? -1
                                    : ((rel_right + 1) * ow - 1) / cw;
    if (out_x_hi >= ow) out_x_hi = ow - 1;

    for (int out_y = out_y_lo; out_y <= out_y_hi; out_y++) {
        int cropped_y = (out_y * ch) / oh;
        int src_y = cy + cropped_y;
        int by = src_y - rect->top;
        if (by < 0 || by >= bh) continue;
        uint16_t *dst_row = &c->canvas[out_y * ow];
        const uint8_t *src_row = &src[by * bw * 3];
        for (int out_x = out_x_lo; out_x <= out_x_hi; out_x++) {
            int cropped_x = (out_x * cw) / ow;
            int src_x = cx + cropped_x;
            int bx = src_x - rect->left;
            if (bx < 0 || bx >= bw) continue;
            const uint8_t *px = &src_row[bx * 3];
            uint16_t pxl = ((px[0] >> 3) << 11)
                         | ((px[1] >> 2) << 5)
                         |  (px[2] >> 3);
            dst_row[out_x] = (pxl >> 8) | (pxl << 8);
        }
    }
    return 1;
}

/* Decode one JPEG into `canvas` (canvas_w × canvas_h RGB565 byte-swapped),
 * applying the given source-space crop rect with nearest-neighbor scaling.
 * Caller owns `canvas`. */
static esp_err_t decode_jpeg_crop_to_canvas(const char *jpeg_path,
                                              int crop_x, int crop_y,
                                              int crop_w, int crop_h,
                                              uint16_t *canvas,
                                              int canvas_w, int canvas_h)
{
    FILE *fp = fopen(jpeg_path, "rb");
    if (!fp) return ESP_ERR_NOT_FOUND;

    /* Slurp whole file into PSRAM — see jpeg_crop_in_cb comment for why
     * fread-per-callback is untenable here. */
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return ESP_FAIL; }
    uint8_t *jpeg_buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!jpeg_buf) { fclose(fp); return ESP_ERR_NO_MEM; }
    size_t got = fread(jpeg_buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) { heap_caps_free(jpeg_buf); return ESP_FAIL; }

    /* tjpgd working buffer. MUST live in internal RAM — tjpgd reads and
     * writes it hot during decode (IDCT LUTs, MCU state, Huffman
     * tables). The prior MALLOC_CAP_SPIRAM version turned a 2 s decode
     * into a 30 s decode. The prior `static uint8_t s_tjpgd_work[32768]`
     * version fixed the speed but permanently claimed 32 KB of BSS —
     * which, combined with show_jpeg's existing `static uint8_t
     * tjpgd_work[32768]`, dropped RETENT_RAM by 64 KB and made
     * app_video_stream_init OOM at boot (video utils 0x101), dead
     * viewfinder, user can't take a photo. Use a transient INTERNAL
     * alloc per call — fast (not PSRAM), no permanent BSS. */
    /* Shared with show_jpeg() and the encoder's decode_and_scale_jpeg.
     * 60 s mutex acquire timeout — see the long rationale in
     * gif_encoder.c::decode_and_scale_jpeg. Same architecture: better
     * to wait and produce a valid .p4ms than to fail and leave the
     * gallery showing a "PROCESSING" stub forever. */
    if (s_tjpgd_mutex && xSemaphoreTake(s_tjpgd_mutex, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGW(TAG, "decode_jpeg_crop: tjpgd mutex timeout (>60 s — encoder stuck?)");
        heap_caps_free(jpeg_buf);
        return ESP_ERR_TIMEOUT;
    }
    uint8_t *work = s_tjpgd_work;

    jpeg_crop_ctx_t ctx = {
        .src = jpeg_buf,
        .src_len = (size_t)sz,
        .src_pos = 0,
        .canvas = canvas,
        .canvas_w = canvas_w, .canvas_h = canvas_h,
        .crop_x = crop_x, .crop_y = crop_y,
        .crop_w = crop_w, .crop_h = crop_h,
    };

    /* Start with a black canvas so any pixels outside the crop stay clear. */
    memset(canvas, 0x10, (size_t)canvas_w * canvas_h * 2);

    JDEC jd;
    JRESULT r = gif_jd_prepare(&jd, jpeg_crop_in_cb, work, 32768, &ctx);
    if (r != JDR_OK) {
        ESP_LOGW(TAG, "decode_jpeg_crop: prepare jdr=%d sz=%ld path=%s", r, sz, jpeg_path);
    } else {
        /* Always scale=0. The local gif_tjpgd.c is compiled with
         * JD_USE_SCALE=0 so any scale>0 returns JDR_PAR. Enabling it
         * would bloat .text by ~8 KB and we'd need to revalidate the
         * entire encoder/decoder stack against it. The current
         * `jpeg_crop_out_cb` does an output-cell-driven nearest-neighbor
         * downscale inside each MCU, so per-MCU cost is O(canvas cells
         * touched) regardless of how much we're scaling. Measured:
         * 2560×1920 OV5640 JPEG → 240×240 canvas in ~150 ms at -O2.
         * If .p4ms save timing ever becomes a bottleneck, enabling
         * JD_USE_SCALE and a scale=1 path is the optimization to try. */
        r = gif_jd_decomp(&jd, jpeg_crop_out_cb, 0);
        if (r != JDR_OK) {
            ESP_LOGW(TAG, "decode_jpeg_crop: decomp jdr=%d w=%u h=%u sz=%ld src_pos=%zu/%zu path=%s",
                     r, jd.width, jd.height, sz, ctx.src_pos, ctx.src_len, jpeg_path);
        }
    }
    if (s_tjpgd_mutex) xSemaphoreGive(s_tjpgd_mutex);
    heap_caps_free(jpeg_buf);
    return (r == JDR_OK) ? ESP_OK : ESP_FAIL;
}

/* Write a .p4ms file for a PIMSLO capture WITHOUT going through the GIF
 * decoder. Decodes each `pos{1..N}.jpg` to 240×240 via tjpgd with the
 * given parallax crop, builds the palindrome playback order
 * (1,2,…,N,N-1,…,2), and saves via save_small_gif. Memory footprint:
 * ≤ 4 × 115 KB canvases + one 32 KB tjpgd work buffer (≤ ~500 KB). */
static esp_err_t save_small_gif_from_jpegs(const char *capture_dir,
                                             const char *gif_output_path,
                                             const gif_crop_rect_t *crops,
                                             const int *src_pos,
                                             int num_cams,
                                             int delay_cs)
{
    if (num_cams < 2 || num_cams > MAX_FRAMES_PER_GIF) return ESP_ERR_INVALID_ARG;
    if (s_ctx.canvas_width <= 0 || s_ctx.canvas_height <= 0) return ESP_ERR_INVALID_STATE;

    gif_cache_slot_t local = {0};
    local.gif_path = strdup(gif_output_path);
    if (!local.gif_path) return ESP_ERR_NO_MEM;

    size_t canvas_bytes = (size_t)s_ctx.canvas_width * s_ctx.canvas_height * 2;

    for (int i = 0; i < num_cams; i++) {
        char jpeg_path[96];
        snprintf(jpeg_path, sizeof(jpeg_path), "%s/pos%d.jpg",
                 capture_dir, src_pos[i]);

        uint16_t *canvas = heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM);
        if (!canvas) { free(local.gif_path); goto fail; }

        uint32_t td0 = esp_log_timestamp();
        esp_err_t r = decode_jpeg_crop_to_canvas(jpeg_path,
                                                  crops[i].x, crops[i].y,
                                                  crops[i].w, crops[i].h,
                                                  canvas,
                                                  s_ctx.canvas_width, s_ctx.canvas_height);
        ESP_LOGI(TAG, "save_small_gif_from_jpegs: cam %d decode took %lums",
                 src_pos[i], (unsigned long)(esp_log_timestamp() - td0));
        if (r != ESP_OK) {
            heap_caps_free(canvas);
            ESP_LOGW(TAG, "save_small_gif_from_jpegs: decode %s failed", jpeg_path);
            free(local.gif_path);
            goto fail;
        }
        local.frames[i].canvas = canvas;
        local.frames[i].delay_cs = delay_cs;
        local.frames[i].hash = 0;
    }
    local.n_frames = num_cams;

    /* Palindrome playback: 0,1,…,N-1,N-2,…,1  (length = 2N-2). */
    int pc = 0;
    for (int i = 0; i < num_cams && pc < (int)sizeof(local.playback_order); i++) {
        local.playback_order[pc++] = (uint8_t)i;
    }
    for (int i = num_cams - 2; i >= 1 && pc < (int)sizeof(local.playback_order); i--) {
        local.playback_order[pc++] = (uint8_t)i;
    }
    local.playback_count = pc;
    local.complete = true;
    local.from_disk = false;

    esp_err_t ret = save_small_gif(&local);

    for (int i = 0; i < local.n_frames; i++) {
        if (local.frames[i].canvas) heap_caps_free(local.frames[i].canvas);
    }
    free(local.gif_path);
    return ret;

fail:
    for (int i = 0; i < MAX_FRAMES_PER_GIF; i++) {
        if (local.frames[i].canvas) heap_caps_free(local.frames[i].canvas);
    }
    return ESP_FAIL;
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

/* Render one frame from the slot's pre-decoded canvases. Used by both
 * the "from .p4ms" case and the post-first-loop fast path. Advances
 * slot->playback_pos; loops back to 0 at the end. */
static void render_cached_frame(lv_timer_t *timer, gif_cache_slot_t *slot)
{
    int pos = slot->playback_pos;
    if (pos < 0 || pos >= slot->playback_count) pos = 0;
    uint8_t idx = slot->playback_order[pos];
    if (idx >= slot->n_frames) idx = 0;
    cached_frame_t *cf = &slot->frames[idx];
    if (!cf->canvas) return;

    size_t canvas_bytes = (size_t)s_ctx.canvas_width * s_ctx.canvas_height * 2;
    memcpy(s_ctx.canvas_buffer, cf->canvas, canvas_bytes);

    bsp_display_lock(0);
    lv_canvas_set_buffer(s_ctx.canvas, s_ctx.canvas_buffer,
                         s_ctx.canvas_width, s_ctx.canvas_height,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_invalidate(s_ctx.canvas);
    bsp_display_unlock();

    slot->playback_pos = (pos + 1) % slot->playback_count;
    if (cf->delay_cs > 0 && timer) lv_timer_set_period(timer, cf->delay_cs * 10);
}

static void playback_timer_cb(lv_timer_t *timer)
{
    if (!s_ctx.is_playing || !s_ctx.canvas_buffer) return;
    gif_cache_slot_t *slot = s_ctx.active_slot;

play_from_cache:
    /* Fast path: slot is complete (either decoded on a previous loop or
     * loaded from .p4ms). Iterate the stored canvases in the stored
     * playback order — no LZW decode, no SD I/O, no decoder state. */
    if (slot && slot->complete && slot->n_frames > 0 && slot->playback_count > 0) {
        render_cached_frame(timer, slot);
        return;
    }

    if (!s_ctx.decoder) return;

    int delay_cs = 10;
    size_t canvas_bytes = (size_t)s_ctx.canvas_width * s_ctx.canvas_height * 2;

    /* Two-step dedup-aware decode: pull next frame's compressed bytes,
     * hash them, consult the cache, then either memcpy the cached
     * canvas (fast) or run LZW decode. While we're still in the first
     * loop (slot->complete == false) we also append to playback_order
     * so the resulting slot can be persisted to .p4ms. */
    uint32_t hash = 0;
    esp_err_t ret = gif_decoder_read_next_frame(s_ctx.decoder, &hash, &delay_cs);
    if (ret == ESP_ERR_NOT_FOUND) {
        /* End of file — first loop is done. Mark complete, persist to
         * .p4ms, release the decoder (frees ~3.5 MB pixel_indices), and
         * jump into the cached-only fast path for this frame + all
         * subsequent ticks. */
        if (slot && !slot->complete) {
            slot->complete = true;
            s_ctx.first_loop_complete = true;
            hide_loading_overlay();
            (void)save_small_gif(slot);   /* best-effort; logs on failure */
        }
        if (s_ctx.decoder) {
            gif_decoder_close(s_ctx.decoder);
            s_ctx.decoder = NULL;
        }
        goto play_from_cache;
    } else if (ret != ESP_OK) {
        app_gifs_stop();
        return;
    }

    /* Lookup against slot's unique-canvas table. */
    int hit_idx = -1;
    if (slot) {
        for (int i = 0; i < slot->n_frames; i++) {
            if (slot->frames[i].canvas && slot->frames[i].hash == hash) {
                hit_idx = i; break;
            }
        }
    }

    const int DIAG_LIMIT = 20;
    bool diag_log = s_ctx.diag_frame_no < DIAG_LIMIT;
    int this_frame = s_ctx.diag_frame_no++;

    int produced_idx = -1;
    if (hit_idx >= 0) {
        if (diag_log) {
            ESP_LOGI(TAG, "f#%d HIT  hash=%08x slot=%d delay=%dcs",
                     this_frame, (unsigned)hash, hit_idx, delay_cs);
        }
        memcpy(s_ctx.canvas_buffer, slot->frames[hit_idx].canvas, canvas_bytes);
        gif_decoder_discard_read_frame(s_ctx.decoder);
        produced_idx = hit_idx;
    } else {
        uint32_t t0 = diag_log ? esp_log_timestamp() : 0;
        ret = gif_decoder_decode_read_frame(s_ctx.decoder,
                                             s_ctx.canvas_buffer,
                                             s_ctx.canvas_width,
                                             s_ctx.canvas_height);
        if (ret != ESP_OK) { app_gifs_stop(); return; }

        bool cached = false;
        if (slot && slot->n_frames < MAX_FRAMES_PER_GIF) {
            uint16_t *copy = heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM);
            if (copy) {
                memcpy(copy, s_ctx.canvas_buffer, canvas_bytes);
                int i = slot->n_frames++;
                slot->frames[i].hash = hash;
                slot->frames[i].canvas = copy;
                slot->frames[i].delay_cs = delay_cs;
                cached = true;
                produced_idx = i;
            }
        }
        if (diag_log) {
            ESP_LOGI(TAG, "f#%d MISS hash=%08x decode=%lums delay=%dcs "
                          "cached=%d total_cached=%d",
                     this_frame, (unsigned)hash,
                     (unsigned long)(esp_log_timestamp() - t0),
                     delay_cs, cached, slot ? slot->n_frames : 0);
        }
    }

    /* Record this frame's slot index in the playback sequence — used to
     * persist and replay the GIF without re-opening the decoder. */
    if (slot && !slot->complete && produced_idx >= 0 && produced_idx < 256 &&
        slot->playback_count < (int)(sizeof(slot->playback_order))) {
        slot->playback_order[slot->playback_count++] = (uint8_t)produced_idx;
    }

    /* Push the freshly-rendered canvas to LVGL. */
    bsp_display_lock(0);
    lv_canvas_set_buffer(s_ctx.canvas, s_ctx.canvas_buffer,
                         s_ctx.canvas_width, s_ctx.canvas_height,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_invalidate(s_ctx.canvas);
    bsp_display_unlock();

    if (delay_cs > 0) lv_timer_set_period(timer, delay_cs * 10);
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

    /* Rescan to pick up the new GIF — deferred to LVGL task. */
    lv_async_call(scan_async_cb, NULL);

    vTaskDelete(NULL);
}

/* ---- Public API ---- */

esp_err_t app_gifs_init(lv_obj_t *canvas)
{
    memset(&s_ctx, 0, sizeof(s_ctx));

    if (!s_tjpgd_mutex) {
        s_tjpgd_mutex = xSemaphoreCreateMutex();
    }

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

    /* Center "PROCESSING"/"QUEUED" badge for JPEG-preview entries.
     * The text is set each time play_current runs: "PROCESSING" while
     * this specific capture is being encoded, "QUEUED" otherwise. */
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

    /* Empty-album overlay — shown whenever the gallery count is 0.
     * Text is updated contextually (empty vs SD error) right before
     * it's made visible. */
    s_ctx.empty_label = lv_label_create(ui_ScreenGifs);
    lv_obj_set_style_bg_color(s_ctx.empty_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ctx.empty_label, LV_OPA_70, 0);
    lv_obj_set_style_text_color(s_ctx.empty_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_ctx.empty_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_ctx.empty_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(s_ctx.empty_label, 12, 0);
    lv_obj_set_style_radius(s_ctx.empty_label, 8, 0);
    lv_obj_align(s_ctx.empty_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_ctx.empty_label, "Album empty");
    lv_obj_add_flag(s_ctx.empty_label, LV_OBJ_FLAG_HIDDEN);

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

    /* Pass 1: every finished .gif. Also clean up 0-byte / truncated
     * .gif files that an earlier interrupted encode left behind —
     * those would otherwise show up as empty gallery entries that
     * play_current can't decode (the user sees a frozen blank canvas
     * with a "PROCESSING" badge that never goes away). */
    DIR *dir = opendir(gif_dir);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && s_ctx.count < MAX_GIF_FILES) {
            if (!is_gif_file(entry->d_name) || entry->d_name[0] == '.') continue;
            char full[MAX_PATH_LEN];
            snprintf(full, sizeof(full), "%.200s/%.255s",
                     gif_dir, entry->d_name);
            /* GIF89a header is 13 bytes minimum (signature + LSD). A
             * file smaller than that is definitely an interrupted-
             * encode orphan. Use 32 B as the practical floor since
             * even a single-frame valid GIF needs more than the bare
             * header. */
            struct stat st;
            if (stat(full, &st) == 0 && st.st_size < 32) {
                ESP_LOGW(TAG, "Cleaning truncated .gif (%lld B): %s",
                         (long long)st.st_size, full);
                unlink(full);
                continue;
            }
            gallery_entry_t *e = &s_ctx.entries[s_ctx.count++];
            e->type = GALLERY_ENTRY_GIF;
            e->gif_path = malloc(MAX_PATH_LEN);
            snprintf(e->gif_path, MAX_PATH_LEN, "%s", full);
        }
        closedir(dir);
    }

    /* Pass 1b: walk /sdcard/p4mslo for capture dirs that have <2
     * pos*.jpg files AND no matching .gif AND no matching preview.
     * These are orphans from a failed/interrupted capture. Without
     * cleanup they accumulate forever, eating SD space and leaving
     * users wondering why their card fills up faster than the GIF
     * counter implies. */
    char p4mslo_root[MAX_PATH_LEN];
    snprintf(p4mslo_root, sizeof(p4mslo_root), "%s/p4mslo", BSP_SD_MOUNT_POINT);
    DIR *capdir = opendir(p4mslo_root);
    if (capdir) {
        struct dirent *centry;
        while ((centry = readdir(capdir)) != NULL) {
            if (centry->d_name[0] != 'P') continue;
            if (strncmp(centry->d_name, "P4M", 3) != 0) continue;
            char capture_dir[MAX_PATH_LEN];
            /* Bound %s to appease -Werror=format-truncation. d_name
             * can be up to NAME_MAX bytes (POSIX 255) which exceeds
             * the remaining slack in MAX_PATH_LEN once p4mslo_root
             * is in there. PIMSLO stems are 7 chars (P4M0001) so
             * 32 is plenty. */
            snprintf(capture_dir, sizeof(capture_dir),
                     "%.200s/%.32s", p4mslo_root, centry->d_name);
            int pos_count = 0;
            for (int k = 1; k <= 4; k++) {
                char p[MAX_PATH_LEN + 16];
                snprintf(p, sizeof(p), "%s/pos%d.jpg", capture_dir, k);
                struct stat ps;
                if (stat(p, &ps) == 0 && ps.st_size > 4) pos_count++;
            }
            if (pos_count >= 2) continue;   /* still encodable, leave alone */

            /* Check if there's a matching .gif or .p4ms — if so, this
             * dir is the encoder's working copy that's about to be
             * cleaned up by the encode finalize. Don't fight it. */
            char gif_path[MAX_PATH_LEN];
            snprintf(gif_path, sizeof(gif_path), "%.200s/%.32s.gif",
                     gif_dir, centry->d_name);
            struct stat gst;
            if (stat(gif_path, &gst) == 0) continue;

            ESP_LOGW(TAG, "Cleaning orphan capture dir (%d/4 pos files): %s",
                     pos_count, capture_dir);
            for (int k = 1; k <= 4; k++) {
                char p[MAX_PATH_LEN + 16];
                snprintf(p, sizeof(p), "%s/pos%d.jpg", capture_dir, k);
                unlink(p);
            }
            rmdir(capture_dir);
        }
        closedir(capdir);
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
                /* New JPEG-only entry (GIF encode still pending). Only
                 * add if the capture dir actually has at least 2 usable
                 * pos*.jpg files — otherwise the encode can never run
                 * and the gallery entry would be stuck as PROCESSING
                 * forever. In that case, auto-clean the orphan preview. */
                char capture_dir[MAX_PATH_LEN];
                snprintf(capture_dir, sizeof(capture_dir),
                         "/sdcard/p4mslo/%s", stem);
                int pos_count = 0;
                for (int k = 1; k <= 4; k++) {
                    char p[MAX_PATH_LEN + 16];
                    snprintf(p, sizeof(p), "%s/pos%d.jpg", capture_dir, k);
                    struct stat st;
                    if (stat(p, &st) == 0 && st.st_size > 4) pos_count++;
                }
                if (pos_count >= 2) {
                    gallery_entry_t *e = &s_ctx.entries[s_ctx.count++];
                    e->type = GALLERY_ENTRY_JPEG;
                    e->jpeg_path = strdup(jpeg_path);
                } else {
                    ESP_LOGW(TAG, "Cleaning orphan preview %s "
                                   "(capture has only %d/4 pos files)",
                              jpeg_path, pos_count);
                    unlink(jpeg_path);
                    /* Also wipe any leftover partial capture dir. */
                    for (int k = 1; k <= 4; k++) {
                        char p[MAX_PATH_LEN + 16];
                        snprintf(p, sizeof(p), "%s/pos%d.jpg",
                                 capture_dir, k);
                        unlink(p);
                    }
                    rmdir(capture_dir);
                }
            }
        }
        closedir(pdir);
    }

    /* Restore the previously-viewed entry. Match on PIMSLO stem, not
     * exact path — when a JPEG-only entry gets its encode completed,
     * its primary path flips from P4M0007.jpg to P4M0007.gif, and a
     * strict path compare would bounce the user back to index 0. The
     * stem (P4M0007) is stable across that transition. */
    s_ctx.current_index = 0;
    if (preserved_path[0]) {
        const char *slash = strrchr(preserved_path, '/');
        const char *base = slash ? slash + 1 : preserved_path;
        char preserved_stem[32] = "";
        extract_pimslo_stem(base, preserved_stem, sizeof(preserved_stem));

        int fallback_idx = -1;
        for (int i = 0; i < s_ctx.count; i++) {
            const char *p = entry_primary_path(&s_ctx.entries[i]);
            if (!p) continue;
            /* Exact match always wins */
            if (strcmp(p, preserved_path) == 0) {
                s_ctx.current_index = i;
                fallback_idx = -1;
                break;
            }
            /* Stem match = the JPEG promoted to a GIF; record it as
             * fallback in case no exact match exists. */
            if (preserved_stem[0]) {
                const char *s2 = strrchr(p, '/');
                const char *b2 = s2 ? s2 + 1 : p;
                char cand_stem[32];
                if (extract_pimslo_stem(b2, cand_stem, sizeof(cand_stem)) &&
                    strcmp(preserved_stem, cand_stem) == 0) {
                    fallback_idx = i;
                }
            }
        }
        if (fallback_idx >= 0) s_ctx.current_index = fallback_idx;
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
    s_last_nav_ms = esp_log_timestamp();
    s_bg_abort_current = true;    /* nudge bg_worker to pack it up */
    app_gifs_stop();
    s_ctx.current_index = (s_ctx.current_index + 1) % s_ctx.count;
    /* No display_gif_info() here — the UI auto-calls app_gifs_play_current()
     * immediately after this, and leaving the prior frame on-screen for
     * the brief window until the new one renders looks smoother than
     * flashing a "Press to play" placeholder over the canvas. */
    return ESP_OK;
}

esp_err_t app_gifs_prev(void)
{
    if (s_ctx.count == 0) return ESP_FAIL;
    s_last_nav_ms = esp_log_timestamp();
    s_bg_abort_current = true;    /* nudge bg_worker to pack it up */
    app_gifs_stop();
    s_ctx.current_index = (s_ctx.current_index + s_ctx.count - 1) % s_ctx.count;
    return ESP_OK;
}

esp_err_t app_gifs_delete_current(void)
{
    if (s_ctx.count == 0 || s_ctx.current_index < 0 ||
        s_ctx.current_index >= s_ctx.count) {
        return ESP_FAIL;
    }

    gallery_entry_t *ent = &s_ctx.entries[s_ctx.current_index];
    int deleted_index = s_ctx.current_index;

    /* Stop playback + drop any cache slot for this entry. Slot_free will
     * no-op if no slot was ever allocated. */
    app_gifs_stop();
    if (ent->gif_path) {
        gif_cache_slot_t *slot = slot_find(ent->gif_path);
        if (slot) slot_free(slot);
    }

    /* Extract the PIMSLO stem from whichever path we have. Same stem is
     * used across .gif / .p4ms / .jpg — that's how the scan merges them. */
    char stem[32] = {0};
    const char *primary = entry_primary_path(ent);
    if (primary) {
        const char *slash = strrchr(primary, '/');
        const char *base = slash ? slash + 1 : primary;
        extract_pimslo_stem(base, stem, sizeof(stem));
    }

    int freed = 0;

    /* 1. Delete the animated .gif if we have it. */
    if (ent->gif_path) {
        if (unlink(ent->gif_path) == 0) {
            ESP_LOGI(TAG, "Deleted %s", ent->gif_path);
            freed++;
        } else {
            ESP_LOGW(TAG, "unlink(%s) failed", ent->gif_path);
        }
    }

    /* 2. Delete the JPEG preview if attached. */
    if (ent->jpeg_path) {
        if (unlink(ent->jpeg_path) == 0) {
            ESP_LOGI(TAG, "Deleted %s", ent->jpeg_path);
            freed++;
        } else {
            ESP_LOGW(TAG, "unlink(%s) failed", ent->jpeg_path);
        }
    }

    /* 3. Delete the .p4ms prerendered file (path derived from stem) —
     * and also any stray preview/gif that wasn't attached to this entry
     * but shares the stem (e.g. if scan hadn't re-merged yet). */
    if (stem[0]) {
        char path[MAX_PATH_LEN];

        snprintf(path, sizeof(path), "%s/%s.p4ms", P4MS_DIR, stem);
        if (unlink(path) == 0) {
            ESP_LOGI(TAG, "Deleted %s", path);
            freed++;
        }

        snprintf(path, sizeof(path), "%s/%s/%s.gif",
                 BSP_SD_MOUNT_POINT, GIF_FOLDER_NAME, stem);
        if (!ent->gif_path && unlink(path) == 0) {
            ESP_LOGI(TAG, "Deleted %s (orphan)", path);
            freed++;
        }

        snprintf(path, sizeof(path), "%s/%s.jpg", PIMSLO_PREVIEW_DIR, stem);
        if (!ent->jpeg_path && unlink(path) == 0) {
            ESP_LOGI(TAG, "Deleted %s (orphan)", path);
            freed++;
        }
    }

    ESP_LOGI(TAG, "Deleted gallery entry #%d (stem=%s, %d file(s))",
             deleted_index, stem[0] ? stem : "<unknown>", freed);

    /* Rescan. current_index is preserved-by-path in app_gifs_scan; since
     * our current entry's files are gone it won't match — scan falls back
     * to index 0, but we want to land on "next entry after the deleted
     * one" for continuity. Clear primary-path memory by zeroing then
     * call scan, then re-seat the index ourselves. */
    app_gifs_scan();

    if (s_ctx.count > 0) {
        /* Clamp to [0, count-1]. If the deleted index was not the last,
         * staying at the same numeric index lands on what used to be the
         * next one. If it was the last, fall back one. */
        int new_idx = deleted_index;
        if (new_idx >= s_ctx.count) new_idx = s_ctx.count - 1;
        s_ctx.current_index = new_idx;
    } else {
        s_ctx.current_index = 0;
    }

    return ESP_OK;
}

void app_gifs_signal_bg_abort(void)
{
    s_last_nav_ms = esp_log_timestamp();
    s_bg_abort_current = true;
}

esp_err_t app_gifs_format_sd(void)
{
    if (!ui_extra_get_sd_card_mounted()) {
        ESP_LOGW(TAG, "format_sd: SD not mounted, refusing");
        return ESP_ERR_INVALID_STATE;
    }

    sdmmc_card_t *card = NULL;
    esp_err_t err = bsp_get_sdcard_handle(&card);
    if (err != ESP_OK || card == NULL) {
        ESP_LOGE(TAG, "format_sd: bsp_get_sdcard_handle failed (err=0x%x)", err);
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }

    /* Drop the foreground player + cross-GIF cache (cache key is the
     * file path — those paths are about to vanish). Nudge bg_worker
     * abort so it doesn't try to reopen a file mid-format. */
    app_gifs_stop();
    app_gifs_flush_cache();
    s_last_nav_ms = esp_log_timestamp();
    s_bg_abort_current = true;

    ESP_LOGI(TAG, "format_sd: starting esp_vfs_fat_sdcard_format on %s",
             BSP_SD_MOUNT_POINT);
    uint32_t t0 = esp_log_timestamp();
    err = esp_vfs_fat_sdcard_format(BSP_SD_MOUNT_POINT, card);
    uint32_t dt = esp_log_timestamp() - t0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "format_sd: format failed err=0x%x after %lu ms",
                 err, (unsigned long)dt);
        /* Try to refresh the gallery anyway — partial-format leaves
         * an indeterminate FS, but the rescan won't make it worse. */
        app_gifs_scan();
        app_gifs_refresh_empty_overlay();
        return err;
    }
    ESP_LOGI(TAG, "format_sd: done in %lu ms", (unsigned long)dt);

    /* Recreate the PIMSLO output directories so the next capture /
     * encode doesn't trip on ENOENT. Mirrors the lazy mkdirs scattered
     * across pimslo_save_task / encode pipeline / ensure_small_dir(). */
    char gif_dir[MAX_PATH_LEN];
    snprintf(gif_dir, sizeof(gif_dir), "%s/%s",
             BSP_SD_MOUNT_POINT, GIF_FOLDER_NAME);
    mkdir("/sdcard/p4mslo", 0755);
    mkdir(gif_dir, 0755);
    mkdir(P4MS_DIR, 0755);
    mkdir(PIMSLO_PREVIEW_DIR, 0755);

    app_gifs_scan();
    app_gifs_refresh_empty_overlay();
    return ESP_OK;
}

/* Decode a full-size JPEG into the 240×240 canvas buffer via tjpgd.
 * Used for entries that don't yet have a GIF (the PIMSLO encode is still
 * running or was interrupted). Static image, no animation.
 *
 * tjpgd reads the source JPEG via `jpeg_in_cb` in many SMALL chunks
 * (marker scans are typically 2-8 bytes, entropy-coded data in ~512 B
 * bursts). Serving those directly from fread() incurs a full FATFS
 * round-trip per call — caught a 14.9 s decode for a 200 KB 1920×1080
 * preview JPEG on this board because of that. The fix is to slurp the
 * whole JPEG into PSRAM up front and have the callback just memcpy
 * from the in-memory buffer. Mirrors what the PIMSLO encoder already
 * does (`app_gifs_encode_pimslo_from_dir` heap_caps_mallocs each
 * pos*.jpg). Observed post-fix decode: ~300 ms for the same 1920x1080
 * preview. */
typedef struct {
    const uint8_t *src;   /* full JPEG bytes, memory-resident */
    size_t src_len;
    size_t src_pos;
    uint16_t *canvas;
    int canvas_w, canvas_h;
    int src_w, src_h;  /* learned from the JPEG header via jd_prepare */
} jpeg_ctx_t;

static size_t jpeg_in_cb(JDEC *jd, uint8_t *buf, size_t len)
{
    jpeg_ctx_t *c = (jpeg_ctx_t *)jd->device;
    size_t avail = (c->src_pos < c->src_len) ? (c->src_len - c->src_pos) : 0;
    if (len > avail) len = avail;
    if (buf) memcpy(buf, c->src + c->src_pos, len);
    c->src_pos += len;
    return len;
}

/* -O2 for the same reason as jpeg_crop_out_cb above: this is a per-MCU
 * tjpgd output callback and runs thousands of times per JPEG decode. */
__attribute__((optimize("O2")))
static int jpeg_out_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *c = (jpeg_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    int bw = rect->right - rect->left + 1;
    int bh = rect->bottom - rect->top + 1;

    /* Nearest-neighbor downscale. Source-row scan: only step through
     * the rows we actually need. For each output row in this MCU,
     * compute the nearest source row once, then for each output column
     * that falls inside the MCU pluck that one source pixel. This is
     * O(canvas_cells_hit) instead of O(MCU_bw * MCU_bh), which at
     * 1920×1080 source → 240×240 canvas is 64x fewer iterations.
     *
     * The earlier per-source-pixel loop did the divides in the inner
     * loop (bw × bh times per MCU), and with scale=0 tjpgd emits every
     * MCU at native resolution — ~8100 MCUs × 256 pixels × 2 divides =
     * over 4 million integer divides per frame on a ~1920×1080 JPEG.
     * On this Core-0 task that was clocking 14-20 s per preview. */
    const int src_y_start = rect->top;
    const int src_y_end   = rect->bottom;         /* inclusive */
    const int src_x_start = rect->left;
    const int src_x_end   = rect->right;          /* inclusive */
    const int sw = c->src_w;
    const int sh = c->src_h;
    const int cw = c->canvas_w;
    const int ch = c->canvas_h;

    /* Output rows whose nearest source falls inside this MCU.
     *
     * The forward map is `src_y = (out_y * sh) / ch` (integer floor).
     * The inverse must be the consistent inverse OF FLOOR, NOT the
     * floor of the inverse:
     *   out_y_lo = smallest out_y where floor(out_y * sh / ch) >= src_y_start
     *            = ceil(src_y_start * ch / sh)
     *   out_y_hi = largest  out_y where floor(out_y * sh / ch) <= src_y_end
     *            = ((src_y_end + 1) * ch - 1) / sh    [integer div]
     *
     * The earlier `(src_y_end * ch) / sh` form was the floor of the
     * inverse, which loses one canvas cell at every MCU boundary
     * where the source pixel that should serve it falls exactly on
     * the boundary. Same bug fixed in jpeg_crop_out_cb (.p4ms path)
     * earlier — they share the inverse-floor pattern. Symptom on
     * the show_jpeg path: faint visible gaps / blue pixels every
     * 8 columns/rows on the JPEG preview at 1920→240 ratios. */
    int out_y_lo = (src_y_start * ch + sh - 1) / sh;
    int out_y_hi = ((src_y_end + 1) * ch - 1) / sh;
    if (out_y_lo < 0) out_y_lo = 0;
    if (out_y_hi >= ch) out_y_hi = ch - 1;

    int out_x_lo = (src_x_start * cw + sw - 1) / sw;
    int out_x_hi = ((src_x_end + 1) * cw - 1) / sw;
    if (out_x_lo < 0) out_x_lo = 0;
    if (out_x_hi >= cw) out_x_hi = cw - 1;

    for (int out_y = out_y_lo; out_y <= out_y_hi; out_y++) {
        int src_y = (out_y * sh) / ch;
        int by = src_y - src_y_start;
        if (by < 0 || by >= bh) continue;
        uint16_t *dst_row = &c->canvas[out_y * cw];
        const uint8_t *src_row = &src[by * bw * 3];
        for (int out_x = out_x_lo; out_x <= out_x_hi; out_x++) {
            int src_x = (out_x * sw) / cw;
            int bx = src_x - src_x_start;
            if (bx < 0 || bx >= bw) continue;
            const uint8_t *px = &src_row[bx * 3];
            /* Byte-swapped RGB565 for LV_COLOR_16_SWAP (matches the GIF
             * decoder output format so the canvas renders identically). */
            uint16_t pxl = ((px[0] >> 3) << 11)
                         | ((px[1] >> 2) << 5)
                         |  (px[2] >> 3);
            dst_row[out_x] = (pxl >> 8) | (pxl << 8);
        }
    }
    return 1;
}

static esp_err_t show_jpeg(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return ESP_FAIL;

    /* Slurp whole file into PSRAM before handing to tjpgd. tjpgd's
     * in_cb pattern is extremely fread-unfriendly (thousands of small
     * marker / MCU reads), and an SD-backed fread per call dwarfs the
     * actual decode work. See comment on jpeg_ctx_t for the diagnosis. */
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return ESP_FAIL; }

    uint8_t *jpeg_buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!jpeg_buf) { fclose(fp); return ESP_ERR_NO_MEM; }
    size_t got = fread(jpeg_buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        heap_caps_free(jpeg_buf);
        return ESP_FAIL;
    }

    /* Shared tjpgd work buffer — see s_tjpgd_work comment at file top.
     *
     * 30 s timeout (was 5 s). Why 30 s: with err_cur/err_nxt now in
     * PSRAM (commit 1ac42a1), encoder Pass 2 frames take ~22 s each
     * including the encoder's ~1.7 s tjpgd-mutex hold. If show_jpeg
     * runs concurrently with an active encoder frame, 5 s wasn't
     * enough — show_jpeg returned TIMEOUT and the canvas's blue 0x10
     * memset background stayed visible. User-visible symptom: gallery
     * preview is solid blue when entered while another encode is mid-
     * frame. 30 s covers the worst case (encoder frame + p4ms decode
     * + small slack). The encoder releases the mutex AROUND each frame
     * (decode → release → dither+LZW → next-frame decode → reacquire)
     * so the longest single hold is the decode phase, ~1.7 s. */
    if (s_tjpgd_mutex && xSemaphoreTake(s_tjpgd_mutex, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGW(TAG, "show_jpeg: tjpgd mutex timeout (>30 s — encoder stuck?)");
        heap_caps_free(jpeg_buf);
        return ESP_ERR_TIMEOUT;
    }

    jpeg_ctx_t ctx = {
        .src = jpeg_buf,
        .src_len = (size_t)sz,
        .src_pos = 0,
        .canvas = s_ctx.canvas_buffer,
        .canvas_w = s_ctx.canvas_width,
        .canvas_h = s_ctx.canvas_height,
    };

    JDEC jd;
    TIMING_MARK(t_prep0);
    JRESULT r = gif_jd_prepare(&jd, jpeg_in_cb, s_tjpgd_work, sizeof(s_tjpgd_work), &ctx);
    TIMING_MARK(t_prep1);
    if (r != JDR_OK) {
        ESP_LOGE(TAG, "JPEG header parse failed for %s (jdr=%d)", path, r);
        if (s_tjpgd_mutex) xSemaphoreGive(s_tjpgd_mutex);
        heap_caps_free(jpeg_buf);
        return ESP_FAIL;
    }

    /* Pick a tjpgd scale that gets the JPEG close to (but not below)
     * the canvas size. scale=0→1:1, scale=1→1:2, scale=2→1:4, scale=3→1:8.
     * tjpgd skips most of the DCT work at higher scales, and the
     * hand-rolled nearest-neighbor step in jpeg_out_cb has much less
     * work when the tjpgd output is already close to canvas size.
     * Measured 1920×1080 preview decode on this board: ~10 s at
     * scale 0 with a naive per-src-pixel downscale → ~650 ms at
     * scale 2 (1920/4 = 480 px out, still > canvas so one more
     * tier of nearest-neighbor in out_cb). */
    uint8_t scale = 0;
    while (scale < 3 &&
           (int)jd.width  >> (scale + 1) >= s_ctx.canvas_width &&
           (int)jd.height >> (scale + 1) >= s_ctx.canvas_height) {
        scale++;
    }
    /* After scale selection, the effective source dimensions tjpgd
     * emits are (jd.width >> scale, jd.height >> scale). The RECT
     * coordinates in jpeg_out_cb are in this scaled space, so our
     * downscale math needs the scaled numbers. */
    ctx.src_w = (int)jd.width  >> scale;
    ctx.src_h = (int)jd.height >> scale;

    /* Clear canvas before decode so untouched areas don't show stale data. */
    memset(s_ctx.canvas_buffer, 0x10,
           s_ctx.canvas_width * s_ctx.canvas_height * 2);

    TIMING_MARK(t_dec0);
    r = gif_jd_decomp(&jd, jpeg_out_cb, scale);
    TIMING_MARK(t_dec1);
    TIMING_LOG("show_jpeg(%ux%u scale=%d → %dx%d): prep=%lums decode=%lums",
               (unsigned)jd.width, (unsigned)jd.height, (int)scale,
               ctx.src_w, ctx.src_h,
               (unsigned long)(t_prep1 - t_prep0),
               (unsigned long)(t_dec1 - t_dec0));
    if (s_tjpgd_mutex) xSemaphoreGive(s_tjpgd_mutex);
    heap_caps_free(jpeg_buf);
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
        /* Nothing to play — surface the empty-album (or SD-error)
         * overlay. Also clear any per-entry badges that might be
         * showing from a stale state. */
        if (s_ctx.processing_label) {
            bsp_display_lock(0);
            lv_obj_add_flag(s_ctx.processing_label, LV_OBJ_FLAG_HIDDEN);
            bsp_display_unlock();
        }
        app_gifs_refresh_empty_overlay();
        return ESP_FAIL;
    }

    /* There IS something to play — make sure the empty-album overlay
     * is hidden regardless of prior state. */
    if (s_ctx.empty_label) {
        bsp_display_lock(0);
        lv_obj_add_flag(s_ctx.empty_label, LV_OBJ_FLAG_HIDDEN);
        bsp_display_unlock();
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
            /* Is THIS entry the one currently being encoded? */
            char stem[32] = {0};
            if (ent->jpeg_path) {
                const char *slash = strrchr(ent->jpeg_path, '/');
                const char *base = slash ? slash + 1 : ent->jpeg_path;
                extract_pimslo_stem(base, stem, sizeof(stem));
            }
            uint16_t encoding_num = app_pimslo_encoding_capture_num();
            char encoding_stem[16] = {0};
            if (encoding_num > 0) {
                snprintf(encoding_stem, sizeof(encoding_stem),
                         "P4M%04u", (unsigned)encoding_num);
            }
            bool is_processing = (encoding_num > 0 &&
                                   strcmp(stem, encoding_stem) == 0);
            lv_label_set_text(s_ctx.processing_label,
                              is_processing ? "PROCESSING" : "QUEUED");
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
        TIMING_MARK(tj0);
        esp_err_t jret = show_jpeg(ent->jpeg_path);
        TIMING_LOG("play_current: show_jpeg took %lums",
                   (unsigned long)(esp_log_timestamp() - tj0));
        if (jret != ESP_OK) {
            ESP_LOGW(TAG, "JPEG preview flash failed: %s", esp_err_to_name(jret));
        }
    }

    /* GIF — acquire the global cache slot first so the playback timer
     * can mirror decoded frames into it for later replays. Slot is
     * reused on revisits: a GIF already watched once comes back with
     * its canvases in memory, and we skip the decoder entirely. */
    TIMING_MARK(ts0);
    s_ctx.active_slot = slot_find_or_alloc(ent->gif_path);
    TIMING_LOG("play_current: slot_find_or_alloc took %lums",
               (unsigned long)(esp_log_timestamp() - ts0));
    if (!s_ctx.active_slot) {
        ESP_LOGW(TAG, "No cache slot available (all full, eviction failed)");
    }

    /* Empty slot (fresh or just evicted) — try the on-disk prerendered
     * 240×240 copy first. If it loads, playback runs entirely from
     * memory with no GIF decoder and no ~3.5 MB pixel_indices. */
    if (s_ctx.active_slot &&
        !s_ctx.active_slot->complete &&
        s_ctx.active_slot->n_frames == 0) {
        TIMING_MARK(tl0);
        (void)load_small_gif(ent->gif_path, s_ctx.active_slot);
        TIMING_LOG("play_current: load_small_gif took %lums",
                   (unsigned long)(esp_log_timestamp() - tl0));
    }

    if (s_ctx.active_slot) {
        s_ctx.active_slot->playback_pos = 0;
        /* If a previous play was interrupted mid-decode, drop the
         * partial playback-order log — the decoder-driven path restarts
         * from frame 0 and would otherwise double-log entries. */
        if (!s_ctx.active_slot->complete) {
            s_ctx.active_slot->playback_count = 0;
        }
        ESP_LOGI(TAG, "cache slot: n_frames=%d playback=%d complete=%d from_disk=%d",
                 s_ctx.active_slot->n_frames,
                 s_ctx.active_slot->playback_count,
                 s_ctx.active_slot->complete,
                 s_ctx.active_slot->from_disk);
    }
    s_ctx.first_loop_complete = s_ctx.active_slot && s_ctx.active_slot->complete;
    s_ctx.diag_frame_no = 0;

    /* Only open the GIF decoder when we actually need to do a first-
     * loop decode. A complete slot (memory-resident OR just loaded from
     * .p4ms) plays entirely from the cached canvas table. */
    if (!s_ctx.first_loop_complete) {
        /* Encoder-busy guard. The GIF decoder needs ~7 MB of PSRAM
         * (decode_buffer + working state) and the encoder is sitting
         * on its own 7 MB scaled_buf — opening both at once OOMs.
         * Pre-2026-04-26 this guard lived in
         * `ui_extra_redirect_to_gifs_page` and skipped play_current
         * entirely, but that also skipped the safe .p4ms path. Now
         * the gate is here, narrowly targeting the decoder open:
         * if the entry doesn't have a .p4ms cached and an encode is
         * in flight, leave the static JPEG preview on screen and
         * bail. The user can re-enter the gallery (or wait for the
         * encode to finish) to trigger the full decoder path. */
        if (app_gifs_is_encoding() || app_pimslo_is_encoding()) {
            ESP_LOGI(TAG, "play_current: encoder busy — skipping GIF decoder "
                          "open; static JPEG preview remains on canvas");
            return ESP_OK;
        }
        /* Tell the bg worker to release its own decoder NOW — foreground
         * is about to claim the ~7 MB PSRAM that a decoder needs. */
        s_bg_abort_current = true;
        esp_err_t ret = gif_decoder_open(ent->gif_path, &s_ctx.decoder);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open GIF: %s", esp_err_to_name(ret));
            return ret;
        }
        s_ctx.decode_width = gif_decoder_get_width(s_ctx.decoder);
        s_ctx.decode_height = gif_decoder_get_height(s_ctx.decoder);
        ESP_LOGI(TAG, "GIF: %dx%d → canvas %dx%d (first-loop decode)",
                 s_ctx.decode_width, s_ctx.decode_height,
                 s_ctx.canvas_width, s_ctx.canvas_height);
        show_loading_overlay();
    } else {
        s_ctx.decode_width = s_ctx.canvas_width;
        s_ctx.decode_height = s_ctx.canvas_height;
        ESP_LOGI(TAG, "Playing %s from cached canvases (no decoder)",
                 ent->gif_path);
    }

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

    /* Note: we DO NOT free the canvas cache here. It lives in
     * g_gif_cache[] across play_current calls so navigating between
     * GIFs or leaving and re-entering the gallery can replay previously
     * decoded frames instantly. Eviction happens LRU-style when a new
     * GIF needs a slot, or wholesale via app_gifs_flush_cache() when
     * leaving the gallery for a page that needs the PSRAM. */
    s_ctx.active_slot = NULL;
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

    /* s_bg_abort_current is set by gallery-nav (app_gifs_next / _prev)
     * so the bg_worker's optional PIMSLO re-encode can bail early. It
     * must NOT apply to the main pimslo_encode_queue_task, which is
     * the first-class path from a fresh capture → .gif on SD — that
     * path has to run to completion even if the user is scrubbing
     * through the gallery. Gate the abort check by caller identity:
     * only honor the flag when we're executing on the bg_worker task.
     *
     * Prior bug: test 02's capture → encode chain fired gallery nav
     * during encode, which set the flag, which aborted the encode,
     * producing "GIF encode failed for P4M%04d: 0x103" and no
     * finished .gif.
     */
    const bool is_bg_caller = (xTaskGetCurrentTaskHandle() == s_bg_worker);
    if (is_bg_caller && s_bg_abort_current) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.is_encoding = true;

    /* Read JPEGs from the capture directory — support 2-4 cameras */
    uint8_t *jpeg_data[MAX_PIMSLO_CAMS] = {NULL};
    size_t jpeg_size[MAX_PIMSLO_CAMS] = {0};
    int num_cams = 0;
    /* Track original camera position (1-indexed) alongside each loaded
     * JPEG so we still compute parallax correctly when captures have
     * gaps (e.g. cam 2 failed, saved pos1+pos3+pos4 — num_cams=3, but
     * src_pos = [1, 3, 4] so parallax is spread across the true
     * positions, not collapsed into consecutive positions). */
    int src_pos[MAX_PIMSLO_CAMS] = {0};

    for (int i = 0; i < MAX_PIMSLO_CAMS; i++) {
        /* Per-JPEG abort check — bg_worker only (see main guard above). */
        if (is_bg_caller && s_bg_abort_current) {
            ret = ESP_ERR_INVALID_STATE;
            ESP_LOGI(TAG, "Encode aborted mid-load (%d/%d cams loaded)",
                     num_cams, MAX_PIMSLO_CAMS);
            goto cleanup;
        }

        char path[80];
        snprintf(path, sizeof(path), "%s/pos%d.jpg", capture_dir, i + 1);
        FILE *f = fopen(path, "rb");
        if (!f) continue;          /* skip gaps instead of breaking */

        fseek(f, 0, SEEK_END);
        jpeg_size[num_cams] = ftell(f);
        fseek(f, 0, SEEK_SET);
        jpeg_data[num_cams] = heap_caps_malloc(jpeg_size[num_cams], MALLOC_CAP_SPIRAM);
        if (!jpeg_data[num_cams]) {
            fclose(f);
            ESP_LOGE(TAG, "OOM for %s (%zu bytes)", path, jpeg_size[num_cams]);
            continue;
        }
        fread(jpeg_data[num_cams], 1, jpeg_size[num_cams], f);
        fclose(f);
        src_pos[num_cams] = i + 1;      /* 1-indexed original position */
        ESP_LOGI(TAG, "Loaded %s: %zu bytes", path, jpeg_size[num_cams]);
        num_cams++;
    }

    /* Second gate: bg_worker caller only (see main guard above). */
    if (is_bg_caller && s_bg_abort_current) {
        ret = ESP_ERR_INVALID_STATE;
        ESP_LOGI(TAG, "Encode aborted pre-palette (all JPEGs loaded, %d cams)",
                 num_cams);
        goto cleanup;
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

    /* Calculate parallax crop rects for each loaded camera. Use each
     * camera's TRUE position (src_pos[i]) instead of its index in the
     * compact loaded-only array, so gaps don't squish the parallax.
     * If cam 2 is missing (src_pos = [1, 3, 4]), positions 1 and 3
     * keep their original parallax offsets and the resulting sequence
     * just lacks the middle frame. */
    gif_crop_rect_t crops[MAX_PIMSLO_CAMS];
    for (int i = 0; i < num_cams; i++) {
        /* src_pos is 1-indexed; convert to 0..3 position for ratio */
        float crop_ratio = (MAX_PIMSLO_CAMS > 1)
            ? (float)(src_pos[i] - 1) / (MAX_PIMSLO_CAMS - 1)
            : 0.0f;
        int parallax_offset = (int)(crop_ratio * total_parallax);
        crops[i].x = h_margin + parallax_offset;
        crops[i].y = v_margin;
        crops[i].w = crop_w;
        crops[i].h = square;
        ESP_LOGI(TAG, "  Pos %d: crop(%d, %d, %d, %d)",
                 src_pos[i], crops[i].x, crops[i].y, crop_w, square);
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

    /* Compute the output stem once. Normally the capture dir basename
     * (e.g. "P4M0007") is the stem. The legacy `pimslo` / `spi_pimslo`
     * serial commands write to `/sdcard/pimslo` (literal, no P4M number)
     * so the basename is "pimslo" — that would land a `pimslo.gif` in
     * the gallery, breaking the P4M%04d naming convention every other
     * code path uses. When the input is that legacy path, reserve the
     * next P4M slot from the same NVS-backed counter as photo_btn so
     * the gallery sees a normal P4Mxxxx entry. */
    const char *dir_name_raw = strrchr(capture_dir, '/');
    dir_name_raw = dir_name_raw ? dir_name_raw + 1 : capture_dir;
    char output_stem[16];
    if (strcmp(dir_name_raw, "pimslo") == 0) {
        uint16_t reserved = app_pimslo_reserve_capture_num();
        snprintf(output_stem, sizeof(output_stem), "P4M%04u", reserved);
        ESP_LOGI(TAG, "Legacy capture dir — assigning stem %s", output_stem);
    } else {
        snprintf(output_stem, sizeof(output_stem), "%.15s", dir_name_raw);
    }

    /* Produce the 240×240 .p4ms now — BEFORE gif_encoder_create()
     * allocates its ~7 MB scaled_buf. Uses tjpgd on the source JPEGs
     * with the already-computed parallax crops; no GIF decoder involved,
     * total working set stays under ~500 KB. Gallery can then replay
     * this capture from disk on future visits without ever touching
     * the GIF decoder. */
    {
        char p4ms_gif_path[MAX_PATH_LEN];
        snprintf(p4ms_gif_path, sizeof(p4ms_gif_path), "%s/%s/%s.gif",
                 BSP_SD_MOUNT_POINT, GIF_FOLDER_NAME, output_stem);
        esp_err_t psave = save_small_gif_from_jpegs(capture_dir, p4ms_gif_path,
                                                     crops, src_pos, num_cams, delay_cs);
        if (psave == ESP_OK) {
            ESP_LOGI(TAG, "Direct-JPEG .p4ms saved for %s", output_stem);
        } else {
            ESP_LOGW(TAG, "Direct-JPEG .p4ms save failed (0x%x) — bg worker "
                          "will retry from .gif later", psave);
        }
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

    /* Pass 1: Build palette from all loaded frames (re-read each from SD,
     * using the actual saved file number src_pos[i] rather than the
     * compact index). */
    for (int i = 0; i < num_cams; i++) {
        char path[80];
        snprintf(path, sizeof(path), "%s/pos%d.jpg", capture_dir, src_pos[i]);
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
            ESP_LOGW(TAG, "Pass 1 failed for pos %d", src_pos[i]);
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
    /* Stem already computed above (handles both regular P4Mxxxx dirs
     * and the legacy `/sdcard/pimslo` literal-dir path). */
    snprintf(output_path, sizeof(output_path), "%s/%s/%s.gif",
             BSP_SD_MOUNT_POINT, GIF_FOLDER_NAME, output_stem);

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
        snprintf(path, sizeof(path), "%s/pos%d.jpg", capture_dir, src_pos[i]);
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
    /* Only restore viewfinder when the user is actually on a camera-type
     * page. On ALBUM / GIFS / USB / SETTINGS / MAIN the ~7 MB of camera
     * buffers aren't needed and reallocating them after every encode
     * fragments PSRAM — the NEXT encode then fails to load its 800 KB
     * source JPEGs (observed: "OOM for pos2.jpg (858825 bytes)"). The
     * camera-page redirect will reallocate when the user returns. */
    {
        ui_page_t p = ui_extra_get_current_page();
        if (p == UI_PAGE_CAMERA || p == UI_PAGE_INTERVAL_CAM ||
            p == UI_PAGE_VIDEO_MODE) {
            app_video_stream_realloc_buffers();
        } else {
            ESP_LOGI(TAG, "Skipping viewfinder realloc — user is off a camera page");
        }
    }
cleanup:
    for (int i = 0; i < MAX_PIMSLO_CAMS; i++) {
        if (jpeg_data[i]) heap_caps_free(jpeg_data[i]);
    }
    s_ctx.is_encoding = false;
    /* Defer the gallery rescan to the LVGL task. This function runs on
     * the pimslo-encode / bg-worker task, and app_gifs_scan() mutates
     * s_ctx.entries — the exact array the LVGL task reads on every
     * button event / redraw. Running it async via LVGL serializes it
     * onto the same single-threaded loop the gallery UI uses, so there's
     * no chance LVGL is mid-read when we free_entries + re-populate. */
    lv_async_call(scan_async_cb, NULL);
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

    /* The legacy `pimslo` / `spi_pimslo` serial cmd path. PSRAM stack
     * via xTaskCreatePinnedToCoreWithCaps + MALLOC_CAP_SPIRAM.
     *
     * Why PSRAM here when pimslo_encode_queue_task is BSS-internal?
     * The two encoder-task slots BSS-internally are already taken by
     * pimslo_encode_queue_task (16 KB) and gif_bg (16 KB). Internal
     * RAM is too tight to stand up a third 16 KB BSS stack without
     * pushing other things (SPI scratch, LCD priv-TX) past their
     * minimums. This path is test/serial-cmd only — the user-facing
     * photo_btn flow uses pimslo_encode_queue_task which has the BSS
     * stack and the encoder is fast there.
     *
     * SAFETY caveat (same as pimslo_save in app_pimslo.c): the
     * FreeRTOS canary doesn't reliably cover PSRAM stacks on
     * ESP32-P4. Encoder hot-loop call chain via this path can
     * overflow if anything is added to the encoder. If a tlsf::
     * remove_free_block panic shows up after exercising this serial
     * cmd, look here first. */
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        pimslo_encode_task, "pimslo_enc", 16384, params, 5, NULL, 1,
        MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        free(params);
        s_ctx.is_encoding = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- Background worker ---------------------------------------------
 *
 * Persistent Core-1 task. When the system is otherwise idle (no user in
 * the gallery, no active PIMSLO capture / encode, no in-flight album
 * encode), the worker walks two queues:
 *
 *   1. `.gif` files under /sdcard/p4mslo_gifs/ that don't yet have a
 *      matching /sdcard/p4mslo_small/<stem>.p4ms. Opens the decoder,
 *      iterates the frames, and saves the pre-rendered small version.
 *   2. JPEG-only gallery entries (preview exists but the .gif was never
 *      produced — e.g. encode interrupted by a reboot). If the source
 *      /sdcard/p4mslo/<stem>/pos*.jpg directory still exists, kicks off
 *      app_gifs_encode_pimslo_from_dir(). That function produces both
 *      the .gif and the .p4ms in one pass (the direct-JPEG save above).
 *
 * Between work items the worker checks `s_gallery_open` and the PIMSLO
 * flags, and any in-progress pre-render polls `s_bg_abort_current` so
 * foreground activity can preempt it. */

/* Actual storage — declared up near the other module-scope statics
 * via the `s_bg_worker` forward decl; see comment there. */
/* s_gallery_open / s_bg_abort_current are declared near the top of the
 * file so play_current() can poke them. */

/* True after the user opens the gallery for the first time. Gates the
 * PSRAM-heavy bg encode work off until the user has actually
 * demonstrated interest in GIFs — before that, camera captures are the
 * priority and every byte of PSRAM the viewfinder can find is needed. */
static volatile bool s_gallery_ever_opened = false;

void app_gifs_set_gallery_open(bool open)
{
    s_gallery_open = open;
    if (open) {
        s_bg_abort_current = true;
        s_gallery_ever_opened = true;
        /* Treat the open event itself as a nav. Without this, bg_worker
         * sees the gallery as "open but idle" the instant the user
         * lands on it (no nav yet) and happily starts a ~50 s PIMSLO
         * encode before the user can press a single button. Their
         * first knob press then has to wait out the whole encode.
         * Pretending the user just nav'd gives them NAV_QUIET_MS to
         * actually start nav'ing before heavy bg work kicks in. */
        s_last_nav_ms = esp_log_timestamp();
    }
}

bool app_gifs_gallery_ever_opened(void)
{
    return s_gallery_ever_opened;
}

/* Camera-type pages have the ~7 MB viewfinder buffer live. Opening a
 * bg gif_decoder at the same time produces enough PSRAM fragmentation
 * that the viewfinder's next realloc fails, freezing the camera UI.
 * The gallery check (s_ctx.decoder / s_gallery_open) is separate and
 * only covers gif playback contention. */
static bool bg_camera_page_active(void)
{
    ui_page_t p = ui_extra_get_current_page();
    return (p == UI_PAGE_CAMERA ||
            p == UI_PAGE_INTERVAL_CAM ||
            p == UI_PAGE_VIDEO_MODE);
}

/* Pages where a ~7 MB bg encode can safely run. Camera-type pages are
 * hard-excluded (viewfinder owns 7 MB). MAIN is excluded because camera
 * is one click away. GIFS is INCLUDED because that's where the user
 * naturally sits waiting for their captures to encode — the bg worker
 * calls app_gifs_stop() + app_gifs_flush_cache() before the 7 MB alloc
 * (see bg_worker_task), so the gallery's canvas cache is reclaimed and
 * playback resumes automatically after the async scan. */
static bool bg_encode_safe_page(void)
{
    ui_page_t p = ui_extra_get_current_page();
    /* UI_PAGE_GIFS intentionally omitted. An in-gallery encode takes
     * ~50 s and pins the JPEG decoder + 7 MB of PSRAM + SD I/O, which
     * stalls every gallery nav the user tries during the window
     * (regression: knob presses feel dead for tens of seconds). Users
     * who want stale captures to catch up only need to leave the
     * gallery — any of the other idle pages (ALBUM / USB_DISK /
     * SETTINGS) triggers the bg encoder. Pre-rendering of .p4ms
     * previews, which is fast (~10 s per GIF, interruptible), still
     * runs unconditionally — that's a different bg path. */
    return (p == UI_PAGE_ALBUM ||
            p == UI_PAGE_USB_DISK ||
            p == UI_PAGE_SETTINGS);
}

/* Used once at the top of a bg iteration — full check including a
 * PSRAM-fragmentation back-stop. Opening a decoder pins ~3.5 MB so we
 * want to see a comfortable contiguous block BEFORE we commit. */
static bool bg_should_yield(void)
{
    /* The real constraint is the foreground GIF decoder — not whether
     * the gallery is open. If the active entry is playing from .p4ms
     * (cached-only mode, no decoder), we can freely pre-render other
     * entries in the background; the user will see new ones flip to
     * instant-load as they navigate. */
    if (s_ctx.decoder != NULL) return true;
    if (s_ctx.is_encoding) return true;
    if (app_pimslo_is_encoding()) return true;
    if (app_pimslo_is_capturing()) return true;
    if (app_pimslo_get_queue_depth() > 0) return true;
    if (bg_camera_page_active()) return true;

    /* Recently-navigating guard: if the user just pressed up/down on
     * the gallery (within NAV_QUIET_MS), don't pick up new background
     * work yet. A GIF encode holds the JPEG decoder + 7 MB of PSRAM
     * for ~50 s; starting one while the user is still scrubbing
     * through entries locks gallery playback completely. The in-flight
     * bg render (if any) already got a s_bg_abort_current nudge from
     * app_gifs_next/_prev so it'll unwind quickly on its own. */
    if (s_last_nav_ms != 0 &&
        (esp_log_timestamp() - s_last_nav_ms) < NAV_QUIET_MS) return true;

    size_t contig = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (contig < 5 * 1024 * 1024) return true;

    return false;
}

/* Used inside a running pre-render's per-frame loop. Intentionally does
 * NOT check the PSRAM contig threshold — the bg decoder itself is
 * already holding ~3.5 MB, so that check would always trip mid-render
 * and uselessly abort every attempt. Only checks signals that mean
 * "foreground truly claimed something new." */
static bool bg_should_abort_current(void)
{
    if (s_bg_abort_current) return true;
    if (s_ctx.decoder != NULL) return true;        /* foreground opened a decoder */
    if (s_ctx.is_encoding) return true;
    if (app_pimslo_is_capturing()) return true;
    if (app_pimslo_is_encoding()) return true;
    if (bg_camera_page_active()) return true;      /* viewfinder owns PSRAM */
    return false;
}

/* Session-scoped blacklist of .gif paths that failed bg pre-render at
 * least once. Prevents the worker from pounding the same broken file
 * every 3 seconds forever. Cleared on reboot. */
#define BG_BLACKLIST_MAX 16
static char *s_bg_blacklist[BG_BLACKLIST_MAX] = {0};
static int   s_bg_blacklist_n = 0;

static bool bg_is_blacklisted(const char *path)
{
    for (int i = 0; i < s_bg_blacklist_n; i++) {
        if (s_bg_blacklist[i] && strcmp(s_bg_blacklist[i], path) == 0) return true;
    }
    return false;
}

static void bg_blacklist_add(const char *path)
{
    if (bg_is_blacklisted(path)) return;
    if (s_bg_blacklist_n >= BG_BLACKLIST_MAX) return;
    s_bg_blacklist[s_bg_blacklist_n++] = strdup(path);
    ESP_LOGW(TAG, "BG: blacklisting %s after repeated failure", path);
}

/* Search /sdcard/p4mslo_gifs for the first .gif that doesn't have a
 * matching .p4ms and hasn't been blacklisted this session. */
static bool bg_find_unprocessed_gif(char *out, size_t cap)
{
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/%s", BSP_SD_MOUNT_POINT, GIF_FOLDER_NAME);
    DIR *d = opendir(dir);
    if (!d) return false;

    bool found = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!is_gif_file(e->d_name) || e->d_name[0] == '.') continue;
        char gif_path[MAX_PATH_LEN];
        snprintf(gif_path, sizeof(gif_path), "%.200s/%.255s", dir, e->d_name);
        if (small_file_exists(gif_path)) continue;
        if (bg_is_blacklisted(gif_path)) continue;

        snprintf(out, cap, "%s", gif_path);
        found = true;
        break;
    }
    closedir(d);
    return found;
}

/* Search /sdcard/p4mslo_previews for JPEGs whose capture hasn't
 * produced a .gif yet, AND whose source /sdcard/p4mslo/<stem>/pos1.jpg
 * still exists. Returns the capture directory so the encoder can
 * consume it. */
static bool bg_find_jpeg_only(char *out_capture_dir, size_t cap)
{
    DIR *d = opendir(PIMSLO_PREVIEW_DIR);
    if (!d) return false;

    bool found = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *ext = strrchr(e->d_name, '.');
        if (!ext || strcasecmp(ext, ".jpg") != 0 || e->d_name[0] == '.') continue;
        char stem[32];
        if (!extract_pimslo_stem(e->d_name, stem, sizeof(stem))) continue;

        char gif_path[MAX_PATH_LEN];
        snprintf(gif_path, sizeof(gif_path), "%s/%s/%s.gif",
                 BSP_SD_MOUNT_POINT, GIF_FOLDER_NAME, stem);
        struct stat st;
        if (stat(gif_path, &st) == 0) continue;  /* GIF already produced */

        char capture_dir[MAX_PATH_LEN];
        snprintf(capture_dir, sizeof(capture_dir), "/sdcard/p4mslo/%s", stem);
        char pos1[MAX_PATH_LEN + 16];
        snprintf(pos1, sizeof(pos1), "%s/pos1.jpg", capture_dir);
        if (stat(pos1, &st) != 0) continue;  /* no source frames */

        /* Skip dirs we've already failed on this session (e.g. capture
         * only saved pos1.jpg — pimslo needs ≥2 for any GIF). */
        if (bg_is_blacklisted(capture_dir)) continue;

        snprintf(out_capture_dir, cap, "%s", capture_dir);
        found = true;
        break;
    }
    closedir(d);
    return found;
}

/* Decode a .gif into a temporary slot and persist it as .p4ms. Bails
 * early on foreground activity (polls s_bg_abort_current / s_gallery_open
 * between frames). Designed to use roughly the same PSRAM as active
 * playback (~3.5 MB for pixel_indices + growing pending_lzw), so MUST
 * only be called when the foreground decoder is idle. */
static esp_err_t bg_render_p4ms_from_gif(const char *gif_path)
{
    if (!gif_path || s_ctx.canvas_width <= 0) return ESP_ERR_INVALID_STATE;

    gif_decoder_t *dec = NULL;
    uint16_t *tmp_canvas = NULL;
    gif_cache_slot_t local = {0};
    esp_err_t ret = ESP_FAIL;
    size_t canvas_bytes = (size_t)s_ctx.canvas_width * s_ctx.canvas_height * 2;

    local.gif_path = strdup(gif_path);
    if (!local.gif_path) return ESP_ERR_NO_MEM;

    tmp_canvas = heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM);
    if (!tmp_canvas) { ret = ESP_ERR_NO_MEM; goto done; }

    ret = gif_decoder_open(gif_path, &dec);
    if (ret != ESP_OK) goto done;

    s_bg_abort_current = false;
    while (1) {
        /* Pacing delay between frames. Two jobs:
         *   1. Give IDLE1 on Core 1 actual run time (otherwise the
         *      task-watchdog fires every 5 s complaining about IDLE1
         *      being starved — bg_worker at prio 2 is the only
         *      non-idle task on Core 1 and LZW frame decode is a tight
         *      CPU loop that never blocks on I/O).
         *   2. Leave breathing room for foreground gallery nav.
         *      `ui_extra_btn_down` on the gifs page calls
         *      `app_gifs_play_current`, which opens the GIF decoder
         *      inside the LVGL display lock. If bg_worker is in the
         *      middle of its own frame decode (~700 ms for a large
         *      GIF), the foreground's check-for-abort→see-it-set path
         *      takes up to one full frame to reach the yield point,
         *      and knob nav feels sluggish on the test rig.
         * 100 ms costs us almost nothing — pre-rendering a full GIF
         * already takes 5-10 s of decode work, and the user won't
         * notice the extra 700 ms total over a whole render — but it
         * halves the worst-case foreground latency. */
        vTaskDelay(pdMS_TO_TICKS(100));

        /* Preempt if the foreground claimed a decoder or PIMSLO started
         * capturing/encoding. Does NOT check PSRAM threshold — our own
         * decoder is part of what's using it. */
        if (bg_should_abort_current()) {
            ret = ESP_ERR_INVALID_STATE;
            goto done;
        }

        uint32_t hash = 0;
        int delay_cs = 10;
        esp_err_t r = gif_decoder_read_next_frame(dec, &hash, &delay_cs);
        if (r == ESP_ERR_NOT_FOUND) break;
        if (r != ESP_OK) { ret = r; goto done; }

        /* Dedup against already-stored unique frames. */
        int hit = -1;
        for (int i = 0; i < local.n_frames; i++) {
            if (local.frames[i].canvas && local.frames[i].hash == hash) { hit = i; break; }
        }

        if (hit >= 0) {
            gif_decoder_discard_read_frame(dec);
        } else {
            if (local.n_frames >= MAX_FRAMES_PER_GIF) {
                ret = ESP_ERR_NO_MEM;
                goto done;
            }
            r = gif_decoder_decode_read_frame(dec, tmp_canvas,
                                                s_ctx.canvas_width,
                                                s_ctx.canvas_height);
            if (r != ESP_OK) { ret = r; goto done; }
            uint16_t *copy = heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM);
            if (!copy) { ret = ESP_ERR_NO_MEM; goto done; }
            memcpy(copy, tmp_canvas, canvas_bytes);
            hit = local.n_frames++;
            local.frames[hit].canvas = copy;
            local.frames[hit].hash = hash;
            local.frames[hit].delay_cs = delay_cs;
        }

        if (local.playback_count < (int)sizeof(local.playback_order) && hit < 256) {
            local.playback_order[local.playback_count++] = (uint8_t)hit;
        }
    }

    local.complete = true;
    local.from_disk = false;
    ret = save_small_gif(&local);

done:
    if (dec) gif_decoder_close(dec);
    if (tmp_canvas) heap_caps_free(tmp_canvas);
    for (int i = 0; i < MAX_FRAMES_PER_GIF; i++) {
        if (local.frames[i].canvas) heap_caps_free(local.frames[i].canvas);
    }
    free(local.gif_path);
    return ret;
}

static void bg_worker_task(void *arg)
{
    ESP_LOGI(TAG, "BG worker started on core %d", xPortGetCoreID());

    const TickType_t STEP_DELAY = pdMS_TO_TICKS(3000);
    const TickType_t IDLE_DELAY = pdMS_TO_TICKS(15000);
    const TickType_t YIELD_DELAY = pdMS_TO_TICKS(2000);

    while (1) {
        /* Let the system finish booting / settle before the first pass. */
        vTaskDelay(STEP_DELAY);

        if (bg_should_yield()) {
            vTaskDelay(YIELD_DELAY);
            continue;
        }

        char path[MAX_PATH_LEN];

        /* Priority 1: pre-render .p4ms for existing .gif files. */
        if (bg_find_unprocessed_gif(path, sizeof(path))) {
            ESP_LOGI(TAG, "BG: pre-rendering .p4ms for %s", path);
            esp_err_t r = bg_render_p4ms_from_gif(path);
            if (r == ESP_OK) {
                ESP_LOGI(TAG, "BG: pre-render success");
            } else if (r == ESP_ERR_INVALID_STATE) {
                /* Foreground claimed the decoder — this isn't the file's
                 * fault, retry next pass without blacklisting. */
                ESP_LOGI(TAG, "BG: pre-render aborted (foreground claim)");
            } else {
                /* Genuine failure (broken file, OOM, etc) — blacklist so
                 * we don't keep thrashing on the same source forever. */
                ESP_LOGW(TAG, "BG: pre-render failed 0x%x", r);
                bg_blacklist_add(path);
            }
            continue;
        }

        /* Priority 2: encode stale PIMSLO captures. ~7 MB scaled_buf, so
         * only run while the user is on a safe page (GIFS / ALBUM /
         * USB / SETTINGS). Gated on gallery-opened-once. */
        if (s_gallery_ever_opened &&
            bg_encode_safe_page() &&
            bg_find_jpeg_only(path, sizeof(path))) {
            ESP_LOGI(TAG, "BG: encoding PIMSLO from %s", path);
            /* Stop gallery playback + flush its canvas cache (up to
             * ~3.5 MB of PSRAM) before the encoder's 7 MB alloc. The
             * lv_async_call scan at the end of app_gifs_encode_pimslo_from_dir
             * auto-restarts playback on whatever entry the user is
             * parked on. */
            app_gifs_stop();
            app_gifs_flush_cache();
            esp_err_t r = app_gifs_encode_pimslo_from_dir(path, 150, 0.05f);
            if (r != ESP_OK) {
                /* Encode failed (missing pos files, OOM, etc) — blacklist
                 * the capture dir so we don't spin on the same stale
                 * entry forever. The same BG worker blacklist is used for
                 * both pre-render and encode paths. */
                ESP_LOGW(TAG, "BG: encode failed 0x%x — blacklisting %s",
                          r, path);
                bg_blacklist_add(path);
            }
            continue;
        }

        /* Nothing to do — idle longer. */
        vTaskDelay(IDLE_DELAY);
    }
}

void app_gifs_start_background_worker(void)
{
    if (s_bg_worker) return;

    /* Seed the canvas dimensions here so that save_small_gif_from_jpegs() can
     * run before the user ever opens the gallery page. Without this the
     * PIMSLO encode path's inline .p4ms save bails with ESP_ERR_INVALID_STATE
     * on the first photo_btn of a fresh boot. app_gifs_init() will overwrite
     * these with the same values when the gallery is opened later — idempotent. */
    if (s_ctx.canvas_width <= 0)  s_ctx.canvas_width  = BSP_LCD_H_RES;
    if (s_ctx.canvas_height <= 0) s_ctx.canvas_height = BSP_LCD_V_RES;

    /* gif_bg stack: BSS-resident static internal RAM via
     * xTaskCreateStaticPinnedToCore.
     *
     * Was PSRAM (via xTaskCreatePinnedToCoreWithCaps + MALLOC_CAP_
     * SPIRAM). PSRAM stacks have two problems: (1) every push/local-
     * read in the encoder hot loop is ~100-200 ns, blowing pre-pass-2
     * timing 5-7×, (2) more importantly per the v5.5.x heap-debug
     * research, FreeRTOS's stack-canary detector does NOT reliably
     * cover PSRAM stacks — a stack overflow into the adjacent PSRAM
     * heap block silently corrupts the next free-block header and
     * fires later as a tlsf::remove_free_block panic with a garbage
     * MTVAL. That's the open bug we've been chasing in tests 08/09.
     *
     * Moved to BSS now that the encoder's 64 KB pixel_lut is in TCM
     * (commit a206f6e) — that freed up exactly the internal-RAM
     * headroom this 16 KB stack needs. The mock's PROPOSED catalog
     * (test/mocks/p4_budget.c) has already been validating the
     * combination "TCM LUT + gif_bg static BSS + pimslo_gif static
     * BSS" against the dma_int budget for several iterations.
     *
     * If gif_bg's 16 KB BSS later starves dma_int again (e.g. when
     * we add another internal-resident static), revert this and put
     * the canary back in via CONFIG_ESP_SYSTEM_HW_STACK_GUARD which
     * IS reliable for PSRAM stacks (unlike the FreeRTOS canary). */
    static StaticTask_t s_gif_bg_tcb;
    static StackType_t s_gif_bg_stack[16384 / sizeof(StackType_t)];
    s_bg_worker = xTaskCreateStaticPinnedToCore(
        bg_worker_task, "gif_bg",
        sizeof(s_gif_bg_stack) / sizeof(StackType_t),
        NULL, 2, s_gif_bg_stack, &s_gif_bg_tcb, 1);
    if (!s_bg_worker) {
        ESP_LOGE(TAG, "Failed to start BG worker");
    }
}
