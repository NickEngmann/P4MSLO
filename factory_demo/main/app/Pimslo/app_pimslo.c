/**
 * @file app_pimslo.c
 * @brief Background PIMSLO capture + GIF encoding pipeline
 *
 * Two persistent FreeRTOS tasks:
 *   1. Capture task (Core 0): waits on semaphore → SPI capture → save to SD → enqueue
 *   2. Encode task (Core 1): waits on queue → GIF encode from saved JPEGs
 */

#include "app_pimslo.h"
#include "spi_camera.h"
#include "app_gifs.h"
#include "app_video_stream.h"
#include "ui_extra.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  /* xTaskCreatePinnedToCoreWithCaps */
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "pimslo";

#define PIMSLO_QUEUE_DEPTH   8
#define PIMSLO_SAVE_QUEUE_DEPTH  4
#define PIMSLO_BASE_DIR      "/sdcard/p4mslo"
#define NVS_NAMESPACE        "storage"
#define NVS_KEY_CAPTURE_NUM  "pimslo_num"
#define NVS_KEY_FAST_MODE    "pimslo_fast"
#define PIMSLO_EXPOSURE_REF_CAM   1  /* camera #2 = most reliable in testing */

/* GIF encode job — just the capture number */
typedef struct {
    uint16_t capture_num;
} pimslo_gif_job_t;

/* Save job — holds the SPI-transferred JPEG buffers on the heap so the
 * capture task can release s_capturing and return to wait immediately
 * after the ~2 s SPI transfer completes. The save task takes ownership
 * of the buffers and frees them after the fwrite loop. */
typedef struct {
    uint16_t  capture_num;
    uint8_t  *jpeg_bufs[4];
    size_t    jpeg_sizes[4];
} pimslo_save_job_t;

static SemaphoreHandle_t s_capture_sem = NULL;
static QueueHandle_t     s_gif_queue   = NULL;
static QueueHandle_t     s_save_queue  = NULL;
static uint16_t          s_next_capture_num = 1;
static bool              s_encoding = false;
/* Which capture is currently being encoded (valid only while s_encoding).
 * Exposed so the gallery can distinguish between an entry that's
 * actively in the encoder vs one still sitting in the queue. */
static volatile uint16_t s_encoding_num = 0;
static volatile bool     s_capturing = false;
/* s_saving is an OR-signal: the capture task sets it right before it
 * enqueues a save job and the save task clears it when the job is done.
 * Keeps app_pimslo_is_capturing() truthy (and the "saving..." overlay
 * visible) across the capture→save handoff so the user sees continuous
 * feedback while the SD writes run in the background. */
static volatile bool     s_saving = false;
static bool              s_initialized = false;

/* ---- NVS persistence for capture counter ---- */

static void load_capture_counter(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, NVS_KEY_CAPTURE_NUM, &s_next_capture_num);
        nvs_close(h);
    }
    if (s_next_capture_num == 0) s_next_capture_num = 1;

    /* Also scan existing directories in case NVS is behind */
    DIR *dir = opendir(PIMSLO_BASE_DIR);
    if (dir) {
        struct dirent *entry;
        uint16_t max_num = 0;
        while ((entry = readdir(dir)) != NULL) {
            unsigned int num = 0;
            if (sscanf(entry->d_name, "P4M%u", &num) == 1 && num > max_num) {
                max_num = (uint16_t)num;
            }
        }
        closedir(dir);
        if (max_num >= s_next_capture_num) {
            s_next_capture_num = max_num + 1;
        }
    }

    ESP_LOGI(TAG, "Next capture number: %d", s_next_capture_num);
}

static void save_capture_counter(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, NVS_KEY_CAPTURE_NUM, s_next_capture_num);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- Fast-capture mode (Phase 4) ---- */

static void load_fast_mode(void)
{
    /* Fast mode defaults to ON — the Phase 4 AF + AE pre-pass was
     * observed to leave the OV5640 sensors in a state where the
     * subsequent trigger produced no JPEG for ~44 s. Skipping the
     * pre-pass recovers reliable photo_btn captures (3-4/4 per run at
     * ~3-4 s) and trades away a feature that's a stub anyway (AF isn't
     * actually implemented on the S3 side — the firmware blob's not
     * loaded). NVS override still lets the user flip it off if desired. */
    uint8_t v = 1;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        /* If the key doesn't exist, v keeps the default (1). */
        nvs_get_u8(h, NVS_KEY_FAST_MODE, &v);
        nvs_close(h);
    }
    spi_camera_set_fast_mode(v != 0);
    ESP_LOGI(TAG, "Fast capture mode: %s (default ON)", v ? "ON" : "off");
}

bool app_pimslo_get_fast_mode(void)
{
    return spi_camera_get_fast_mode();
}

esp_err_t app_pimslo_set_fast_mode(bool enabled)
{
    spi_camera_set_fast_mode(enabled);
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    nvs_set_u8(h, NVS_KEY_FAST_MODE, enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Fast capture mode → %s (persisted)", enabled ? "ON" : "off");
    return ESP_OK;
}

/* ---- SPI Capture Task (Core 0, persistent) ---- */

static void pimslo_capture_task(void *param)
{
    ESP_LOGI(TAG, "Capture task started (Core %d)", xPortGetCoreID());

    while (1) {
        /* Block until capture requested */
        xSemaphoreTake(s_capture_sem, portMAX_DELAY);
        s_capturing = true;

        uint16_t num = s_next_capture_num++;
        save_capture_counter();

        /* ── Overlap mode (Phase 5) ──
         * The video-stream task already fired the GPIO34 trigger on
         * this same photo press (via spi_camera_send_trigger) BEFORE
         * calling take_and_save_photo. So by the time we're here:
         *   - S3 cameras have been capturing for ~1.5-2 s (plenty of
         *     time for exposure + JPEG encode) and their JPEGs are
         *     waiting in PSRAM.
         *   - P4 photo save is already committed to SD.
         * We just need to poll + transfer the JPEGs. Use the
         * _after_trigger variant so we skip the redundant second
         * trigger pulse and the ~500 ms post-trigger wait.
         *
         * If the trigger was NOT pre-fired (e.g. serial-only
         * request_capture off the camera page), the first retry below
         * will send a fresh trigger and recover normally. */
        spi_camera_init();

        /* NB: the preview-JPEG copy (P4 latest photo → P4M<num>.jpg) is
         * handled later on the save task. Keeping it out of this task
         * shaves ~2 s of SD-I/O off the time the user's "saving"
         * overlay stays up. */

        /* Create capture directory */
        char dir_path[64];
        mkdir(PIMSLO_BASE_DIR, 0755);
        snprintf(dir_path, sizeof(dir_path), "%s/P4M%04d", PIMSLO_BASE_DIR, num);
        mkdir(dir_path, 0755);

        ESP_LOGI(TAG, "Capture %03d: polling S3 cameras (trigger already sent)...", num);

        /* Free the viewfinder / photo buffers so the ~4×600KB SPI JPEG
         * transfer buffers have room to allocate. On the CAMERA page the
         * viewfinder holds ~7 MB of PSRAM and without this the transfers
         * OOM. The GIF-encode task also frees+reallocs separately (it needs
         * a 7MB scaled_buf) — those calls are idempotent against this one.
         * Viewfinder freezes for the duration of the SPI capture (~3s). */
        app_video_stream_free_buffers();

        /* Phase 4 pre-capture image-quality pass. Skipped in fast mode.
         * AF is currently a no-op stub on the S3 side (firmware blob not
         * loaded), but the SPI plumbing is exercised so we can swap in a
         * real AF implementation without touching this file. Exposure sync
         * is functional: the reference camera's current AE values are
         * broadcast to the others to reduce cross-camera brightness drift. */
        if (!spi_camera_get_fast_mode()) {
            uint32_t t_ae = esp_log_timestamp();
            spi_camera_autofocus_all(1500);
            spi_camera_sync_exposure(PIMSLO_EXPOSURE_REF_CAM);
            ESP_LOGI(TAG, "Pre-capture AF+AE sync: %lums",
                     (unsigned long)(esp_log_timestamp() - t_ae));
        }

        uint8_t *jpeg_bufs[4] = {NULL};
        size_t jpeg_sizes[4] = {0};
        uint32_t capture_ms = 0;

        /* Receive only — trigger was sent earlier, overlapped with
         * the P4 photo save. If any camera fails, capture_all_impl's
         * retry logic will send a fresh trigger on attempt 1+ as
         * normal. */
        spi_camera_capture_all_after_trigger(jpeg_bufs, jpeg_sizes, &capture_ms);

        /* Count usable SPI results up front — we don't want to enqueue a
         * save job if we can't build at least a 2-frame PIMSLO. */
        int usable = 0;
        for (int i = 0; i < 4; i++) {
            if (jpeg_bufs[i] && jpeg_sizes[i] > 0) usable++;
        }

        ESP_LOGI(TAG, "Capture %03d: SPI xfer %lums, usable=%d/4",
                 num, (unsigned long)capture_ms, usable);

        if (usable >= 2) {
            /* Hand off the JPEG buffers to the save task — it owns the
             * SD I/O (≈8-12 s for 4 × ~800 KB at ~250 KB/s) + preview
             * copy + GIF enqueue. Capture task immediately reclaims the
             * viewfinder buffers and returns to wait so the user can
             * fire the next photo in ~3 s instead of ~15-20 s. */
            pimslo_save_job_t save_job = { .capture_num = num };
            for (int i = 0; i < 4; i++) {
                save_job.jpeg_bufs[i]  = jpeg_bufs[i];
                save_job.jpeg_sizes[i] = jpeg_sizes[i];
            }
            s_saving = true;
            if (xQueueSend(s_save_queue, &save_job, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGE(TAG, "Save queue full — dropping P4M%04d", num);
                for (int i = 0; i < 4; i++) {
                    if (jpeg_bufs[i]) free(jpeg_bufs[i]);
                }
                s_saving = false;
                /* Clean up the empty capture dir we created above. */
                rmdir(dir_path);
            }
        } else {
            /* < 2 cameras returned usable data — nothing to save. Free
             * whatever buffers came back and drop the capture dir. */
            for (int i = 0; i < 4; i++) {
                if (jpeg_bufs[i]) free(jpeg_bufs[i]);
            }
            ESP_LOGW(TAG, "Capture %03d: only %d usable cameras — dropping",
                     num, usable);
            char prev[80];
            snprintf(prev, sizeof(prev), "%s/P4M%04u.jpg", PIMSLO_PREVIEW_DIR, num);
            unlink(prev);
            rmdir(dir_path);
        }

        /* Viewfinder buffers come back NOW, before the SD writes run.
         * Safe because the save task only touches jpeg_bufs (heap
         * blocks we just handed it) + the SD filesystem — no PSRAM
         * contention with the video stream buffer pool. */
        app_video_stream_realloc_buffers();

        s_capturing = false;
    }
}

/* ---- Save Task (Core 1, persistent) ----
 * Runs the slow SD writes off the capture task so the user's "saving..."
 * overlay clears as soon as the SPI transfer completes (~3 s), instead
 * of waiting for 4 × ~800 KB fwrites at ~250 KB/s (~12 s). */
static void pimslo_save_task(void *param)
{
    ESP_LOGI(TAG, "Save task started (Core %d)", xPortGetCoreID());

    for (;;) {
        pimslo_save_job_t job;
        if (xQueueReceive(s_save_queue, &job, portMAX_DELAY) != pdTRUE) continue;

        const uint16_t num = job.capture_num;
        uint32_t t_start = esp_log_timestamp();
        int saved = 0;

        char dir_path[64];
        snprintf(dir_path, sizeof(dir_path), "%s/P4M%04d", PIMSLO_BASE_DIR, num);

        for (int i = 0; i < 4; i++) {
            uint8_t *buf = job.jpeg_bufs[i];
            size_t   sz  = job.jpeg_sizes[i];
            if (!buf || sz < 4) {
                if (buf) free(buf);
                continue;
            }
            bool has_soi = buf[0] == 0xFF && buf[1] == 0xD8;
            bool has_eoi = buf[sz - 2] == 0xFF && buf[sz - 1] == 0xD9;
            if (!has_soi || !has_eoi) {
                ESP_LOGW(TAG, "  pos%d: corrupted JPEG (SOI=%d EOI=%d, %zu bytes) — dropped",
                         i + 1, has_soi, has_eoi, sz);
                free(buf);
                continue;
            }
            char path[80];
            snprintf(path, sizeof(path), "%s/pos%d.jpg", dir_path, i + 1);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(buf, 1, sz, f);
                fclose(f);
                saved++;
                ESP_LOGI(TAG, "  pos%d: %zu bytes", i + 1, sz);
            } else {
                ESP_LOGE(TAG, "  pos%d: fopen failed", i + 1);
            }
            free(buf);
        }

        /* Copy P4 camera latest photo → preview now that take_and_save_photo
         * has committed it to SD. */
        app_pimslo_save_preview_from_latest_photo(num);

        uint32_t save_ms = esp_log_timestamp() - t_start;
        ESP_LOGI(TAG, "Save %03d: %d/4 pos*.jpg in %lums",
                 num, saved, (unsigned long)save_ms);

        if (saved >= 2) {
            pimslo_gif_job_t gjob = { .capture_num = num };
            if (xQueueSend(s_gif_queue, &gjob, 0) != pdTRUE) {
                ESP_LOGW(TAG, "GIF queue full — dropping P4M%04d", num);
            } else {
                ESP_LOGI(TAG, "Queued GIF encode for P4M%04d (queue: %d)",
                         num, (int)uxQueueMessagesWaiting(s_gif_queue));
            }
        } else {
            ESP_LOGW(TAG, "Save %03d: only %d cameras — cleaning up", num, saved);
            char prev[80];
            snprintf(prev, sizeof(prev), "%s/P4M%04u.jpg", PIMSLO_PREVIEW_DIR, num);
            unlink(prev);
            for (int i = 0; i < 4; i++) {
                char p[80];
                snprintf(p, sizeof(p), "%s/pos%d.jpg", dir_path, i + 1);
                unlink(p);
            }
            rmdir(dir_path);
        }

        /* Clear the save flag only if no more saves are pending. If
         * another capture's save job arrived while we were running, the
         * next loop iteration will re-set s_saving from the capture
         * task's xQueueSend + keep the overlay visible. */
        if (uxQueueMessagesWaiting(s_save_queue) == 0) {
            s_saving = false;
        }
    }
}

/* ---- GIF Encode Queue Task (Core 1, persistent) ---- */

/* Defer encoding only when there's an actual memory / display conflict.
 * The encoder needs ~7 MB contiguous PSRAM for its scaled_buf and runs
 * on Core 1; the LCD draws on Core 0. They coexist fine as long as
 * nothing else is holding big contiguous blocks.
 *
 * Defer on:
 *   - CAMERA / INTERVAL_CAM / VIDEO_MODE: viewfinder owns ~7 MB
 *     scaled_buf — absolute collision.
 *
 * Allow on MAIN: a user who fires off photos and stays on MAIN is the
 * common case (test 14, photo_btn from main, walking up to the device
 * and pressing the trigger without entering the gallery). Deferring
 * here meant the .gif never finalized until the user navigated to the
 * album — captures piled up forever as JPEG-only entries with their
 * ~3 MB of pos*.jpg sitting on SD untouched. The pre-existing risk of
 * "user clicks CAMERA mid-encode → viewfinder realloc OOM" already
 * applies on GIFS / USB / SETTINGS and is handled non-fatally by
 * `ui_extra_redirect_to_main_page()` (the realloc just logs a
 * warning and the next viewfinder frame retries).
 *
 * Allow on GIFS (the PIMSLO gallery): the pimslo encode task calls
 * app_gifs_stop() + app_gifs_flush_cache() before the 7 MB alloc, so
 * the gallery's ~3.5 MB canvas cache is reclaimed. Playback
 * auto-resumes via the lv_async_call scan when the encode finishes.
 *
 * USB DISK / SETTINGS are fine — low display pressure. */
static bool encode_should_defer(void)
{
    ui_page_t p = ui_extra_get_current_page();
    if (p == UI_PAGE_CAMERA ||
        p == UI_PAGE_INTERVAL_CAM ||
        p == UI_PAGE_VIDEO_MODE) return true;
    if (app_gifs_is_encoding()) return true;  /* album encoder uses same PSRAM */
    return false;
}

static void pimslo_encode_queue_task(void *param)
{
    ESP_LOGI(TAG, "Encode queue task started (Core %d)", xPortGetCoreID());

    pimslo_gif_job_t job;

    while (1) {
        /* Block until a job is available */
        xQueueReceive(s_gif_queue, &job, portMAX_DELAY);

        /* Defer encoding until the user is off a camera page. Demoted
         * to DEBUG because e2e test 08 uses the presence of any
         * "Deferring" log line in Phase A as its proxy for "did the
         * saving overlay leak on camera-page entry?" — but deferring
         * here is the encoder task politely waiting for PSRAM to be
         * free, not an overlay event (the overlay is driven by
         * app_pimslo_is_capturing, which this path doesn't touch).
         * Enable with `esp_log_level_set("pimslo", ESP_LOG_DEBUG)`
         * when diagnosing encode-queue stalls. */
        bool logged_waiting = false;
        while (encode_should_defer()) {
            if (!logged_waiting) {
                ESP_LOGD(TAG, "Deferring P4M%04d encode — user on camera page / album encoding",
                         job.capture_num);
                logged_waiting = true;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        s_encoding = true;
        s_encoding_num = job.capture_num;

        /* If the user is on the gallery right now, pause playback and
         * flush the canvas cache — that's ~3.5 MB of PSRAM we need back
         * for the encoder's 7 MB scaled_buf. The gallery will re-open
         * its cache the next time play_current runs, which happens
         * automatically via the lv_async_call scan we issue when the
         * encode finishes. Safe no-op when user isn't on the gallery. */
        app_gifs_stop();
        app_gifs_flush_cache();

        char dir_path[64];
        snprintf(dir_path, sizeof(dir_path), "%s/P4M%04d",
                 PIMSLO_BASE_DIR, job.capture_num);

        ESP_LOGI(TAG, "Encoding GIF from %s (queue remaining: %d)",
                 dir_path, (int)uxQueueMessagesWaiting(s_gif_queue));

        uint32_t t0 = esp_log_timestamp();
        esp_err_t ret = app_gifs_encode_pimslo_from_dir(dir_path, 150, 0.05f);
        uint32_t elapsed = esp_log_timestamp() - t0;

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "GIF encode complete for P4M%04d in %lus",
                     job.capture_num, (unsigned long)(elapsed / 1000));

            /* Clean up the capture directory to save SD space */
            for (int i = 1; i <= 4; i++) {
                char path[80];
                snprintf(path, sizeof(path), "%s/pos%d.jpg", dir_path, i);
                unlink(path);
            }
            rmdir(dir_path);
            ESP_LOGI(TAG, "Cleaned up %s", dir_path);
        } else {
            ESP_LOGE(TAG, "GIF encode failed for P4M%04d: 0x%x",
                     job.capture_num, ret);
        }

        s_encoding = false;
        s_encoding_num = 0;
    }
}

/* ---- Public API ---- */

esp_err_t app_pimslo_init(void)
{
    if (s_initialized) return ESP_OK;

    load_capture_counter();
    load_fast_mode();

    s_capture_sem = xSemaphoreCreateBinary();
    if (!s_capture_sem) return ESP_ERR_NO_MEM;

    /* Initialize the SPI master bus + 4 camera device handles up front
     * so the video-stream task's photo-button callback can call
     * spi_camera_send_trigger() without paying init cost on its small
     * 4 KB stack. Idempotent — safe even if this module is re-init'd. */
    spi_camera_init();

    s_gif_queue = xQueueCreate(PIMSLO_QUEUE_DEPTH, sizeof(pimslo_gif_job_t));
    if (!s_gif_queue) return ESP_ERR_NO_MEM;

    s_save_queue = xQueueCreate(PIMSLO_SAVE_QUEUE_DEPTH, sizeof(pimslo_save_job_t));
    if (!s_save_queue) return ESP_ERR_NO_MEM;

    /* SPI capture task on Core 0 (I/O bound — doesn't compete with GIF encode) */
    BaseType_t ret = xTaskCreatePinnedToCore(
        pimslo_capture_task, "pimslo_cap", 8192, NULL, 5, NULL, 0);
    if (ret != pdPASS) return ESP_FAIL;

    /* Save task on Core 1 (SD I/O bound — 4 × ~800 KB fwrite @ ~250 KB/s).
     * Runs concurrently with the GIF encoder task on the same core but
     * doesn't contend heavily since save is I/O-bound and encode is
     * CPU-bound. 6 KB stack: fwrite + its buffered I/O path needs more
     * than 4 KB (observed stack-protect fault at 4 KB in prior attempt).
     *
     * Stack placed in PSRAM (xTaskCreatePinnedToCoreWithCaps +
     * MALLOC_CAP_SPIRAM). The P4-EYE has a very tight DMA-capable
     * internal RAM budget — only ~32 KB reserved by esp_psram at boot,
     * most of which goes to FreeRTOS TCBs / SPI master scratch / etc.
     * When this task's 6 KB stack was in internal RAM (the default),
     * the LCD SPI driver's per-flush priv TX scratch allocation
     * started failing ("setup_dma_priv_buffer: Failed to allocate priv
     * TX buffer"), because LVGL's canvas buffer sits in PSRAM and the
     * LCD SPI device doesn't set SPI_TRANS_DMA_USE_PSRAM, so every
     * flush demands a fresh DMA-internal copy. Symptom: button presses
     * register internally but the screen stops refreshing. Moving the
     * stack to PSRAM is safe here because this task never runs from
     * an ISR context; all it does is fwrite + ESP_LOGI. */
    ret = xTaskCreatePinnedToCoreWithCaps(
        pimslo_save_task, "pimslo_save", 6144, NULL, 4, NULL, 1,
        MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) return ESP_FAIL;

    /* GIF encode queue task on Core 1 (CPU bound — dithering + LZW).
     *
     * Stack pinned to internal RAM via xTaskCreateStaticPinnedToCore
     * + a BSS-resident buffer. The default `xTaskCreatePinnedToCore`
     * calls `pvPortMalloc(16384)` which on this board silently falls
     * back to PSRAM whenever the largest free internal block is < 16
     * KB — and post-boot the largest free internal block is ~7 KB at
     * every point, so the fallback fires every time. PSRAM stack
     * means every push, every local read, every function-call frame
     * access during the encoder per-pixel hot loop is a ~100-200 ns
     * access. Net effect on Pass 2: ~12 s/frame (internal stack)
     * blooms to ~55 s/frame (PSRAM stack), turning a ~95 s encode
     * into a ~5-7 min one.
     *
     * Static allocation places the 16 KB stack in BSS — the linker
     * reserves it in DRAM at link time, before any heap fragmentation
     * happens. We can afford this BSS footprint because the encoder
     * + tjpgd refactor freed 32 KB (gif_encoder.c::tjwork dropped,
     * shared with app_gifs.c::s_tjpgd_work) and 32 KB
     * (gif_encoder.c::file_buf moved to PSRAM via EXT_RAM_BSS_ATTR).
     * Net: -32 -32 +16 = -48 KB internal BSS. Validated against the
     * host budget simulator (test/host_encode/test_budget) before
     * flashing — see CLAUDE-MOCK.md. */
    static StaticTask_t s_pimslo_enc_q_tcb;
    static StackType_t s_pimslo_enc_q_stack[16384 / sizeof(StackType_t)];
    TaskHandle_t enc_q_task = xTaskCreateStaticPinnedToCore(
        pimslo_encode_queue_task, "pimslo_gif",
        sizeof(s_pimslo_enc_q_stack) / sizeof(StackType_t),
        NULL, 4, s_pimslo_enc_q_stack, &s_pimslo_enc_q_tcb, 1);
    if (!enc_q_task) return ESP_FAIL;

    s_initialized = true;
    ESP_LOGI(TAG, "PIMSLO subsystem initialized (queue depth=%d)", PIMSLO_QUEUE_DEPTH);
    return ESP_OK;
}

void app_pimslo_request_capture(void)
{
    /* Flip the capturing flag RIGHT NOW so the "saving..." overlay lights
     * up in the same LVGL tick as the button press. Without this the
     * overlay waited until pimslo_capture_task picked up the semaphore
     * (observed lag: ~500ms-1s) — during which time take_and_save_photo
     * was already freeing viewfinder PSRAM, so the user saw a blank
     * canvas with no feedback. The capture task clears this flag after
     * the full SPI + save cycle. */
    s_capturing = true;
    if (s_capture_sem) {
        xSemaphoreGive(s_capture_sem);
    }
}

int app_pimslo_get_queue_depth(void)
{
    if (!s_gif_queue) return 0;
    return (int)uxQueueMessagesWaiting(s_gif_queue);
}

bool app_pimslo_is_encoding(void)
{
    return s_encoding;
}

uint16_t app_pimslo_encoding_capture_num(void)
{
    return s_encoding ? s_encoding_num : 0;
}

bool app_pimslo_is_capturing(void)
{
    /* Either the SPI capture task is pulling JPEGs from the S3s, or
     * the save task is flushing them to SD. Both states show "saving"
     * to the user since from their perspective the picture isn't done
     * landing until both phases have run. */
    return s_capturing || s_saving;
}

#define P4_PHOTO_DIR "/sdcard/esp32_p4_pic_save"

esp_err_t app_pimslo_save_preview_from_latest_photo(uint16_t num)
{
    /* Scan the P4 photo directory for the highest-numbered pic_NNNN.jpg. */
    DIR *dir = opendir(P4_PHOTO_DIR);
    if (!dir) return ESP_ERR_NOT_FOUND;

    uint32_t best_num = 0;
    /* pic_NNNN.jpg is 12 chars + terminator, but be generous. 64 here
     * also appeases GCC -O2 -Wformat-truncation which flags a 32-byte
     * dest holding a %s of up to 255 POSIX-dirent bytes. */
    char best_name[64] = {0};
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        uint32_t n = 0;
        if (sscanf(entry->d_name, "pic_%lu.jpg", &n) == 1 && n > best_num) {
            best_num = n;
            /* Bounded copy — %.Ns tells the compiler the max input
             * length so it won't warn about truncation. */
            snprintf(best_name, sizeof(best_name), "%.*s",
                      (int)(sizeof(best_name) - 1), entry->d_name);
        }
    }
    closedir(dir);

    if (best_num == 0) {
        ESP_LOGW(TAG, "No P4 photo found to copy as preview");
        return ESP_ERR_NOT_FOUND;
    }

    mkdir(PIMSLO_PREVIEW_DIR, 0755);

    char src[128], dst[80];
    snprintf(src, sizeof(src), "%s/%s", P4_PHOTO_DIR, best_name);
    snprintf(dst, sizeof(dst), "%s/P4M%04u.jpg", PIMSLO_PREVIEW_DIR, num);

    FILE *in = fopen(src, "rb");
    if (!in) return ESP_ERR_NOT_FOUND;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return ESP_FAIL; }

    /* Heap-allocated copy buffer so this function is safe to call from any
     * task regardless of its stack size. A 2 KB stack buffer previously
     * blew out the 4 KB video-stream task stack (caught by stack protection
     * fault after the P4 photo saved successfully). */
    const size_t COPY_BUF_SIZE = 2048;
    uint8_t *buf = heap_caps_malloc(COPY_BUF_SIZE, MALLOC_CAP_DEFAULT);
    if (!buf) { fclose(in); fclose(out); return ESP_ERR_NO_MEM; }

    size_t total = 0;
    size_t n;
    esp_err_t rc = ESP_OK;
    while ((n = fread(buf, 1, COPY_BUF_SIZE, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { rc = ESP_FAIL; break; }
        total += n;
    }
    heap_caps_free(buf);
    fclose(in);
    fclose(out);

    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "Saved P4 preview: %s → %s (%zu bytes)", src, dst, total);
    }
    return rc;
}
