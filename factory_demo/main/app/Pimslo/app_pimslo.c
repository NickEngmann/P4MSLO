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
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "pimslo";

#define PIMSLO_QUEUE_DEPTH   8
#define PIMSLO_BASE_DIR      "/sdcard/p4mslo"
#define NVS_NAMESPACE        "storage"
#define NVS_KEY_CAPTURE_NUM  "pimslo_num"
#define NVS_KEY_FAST_MODE    "pimslo_fast"
#define PIMSLO_EXPOSURE_REF_CAM   1  /* camera #2 = most reliable in testing */

/* GIF encode job — just the capture number */
typedef struct {
    uint16_t capture_num;
} pimslo_gif_job_t;

static SemaphoreHandle_t s_capture_sem = NULL;
static QueueHandle_t     s_gif_queue   = NULL;
static uint16_t          s_next_capture_num = 1;
static bool              s_encoding = false;
static volatile bool     s_capturing = false;
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
    uint8_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_FAST_MODE, &v);
        nvs_close(h);
    }
    spi_camera_set_fast_mode(v != 0);
    ESP_LOGI(TAG, "Fast capture mode: %s", v ? "ON" : "off");
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

        /* Save the P4 photo we just took as the gallery preview for this
         * capture number. Safe to do here (instead of in the video-stream
         * frame callback) because the photo's `take_and_save_photo` call
         * already flushed to SD before the caller gave us this semaphore,
         * and this task has an 8KB stack for the file-copy buffer. */
        app_pimslo_save_preview_from_latest_photo(num);

        /* Create capture directory */
        char dir_path[64];
        mkdir(PIMSLO_BASE_DIR, 0755);
        snprintf(dir_path, sizeof(dir_path), "%s/P4M%04d", PIMSLO_BASE_DIR, num);
        mkdir(dir_path, 0755);

        ESP_LOGI(TAG, "Capture %03d: triggering SPI cameras...", num);

        /* Initialize SPI (idempotent) and capture from all cameras */
        spi_camera_init();

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

        esp_err_t ret = spi_camera_capture_all(jpeg_bufs, jpeg_sizes, &capture_ms);

        /* Save JPEGs to the capture directory */
        int saved = 0;
        for (int i = 0; i < 4; i++) {
            if (jpeg_bufs[i] && jpeg_sizes[i] > 0) {
                char path[80];
                snprintf(path, sizeof(path), "%s/pos%d.jpg", dir_path, i + 1);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fwrite(jpeg_bufs[i], 1, jpeg_sizes[i], f);
                    fclose(f);
                    saved++;
                    ESP_LOGI(TAG, "  pos%d: %zu bytes", i + 1, jpeg_sizes[i]);
                }
                free(jpeg_bufs[i]);
            }
        }

        ESP_LOGI(TAG, "Capture %03d: %d/4 cameras in %lums",
                 num, saved, (unsigned long)capture_ms);

        /* Enqueue GIF encode job if we got enough cameras. Critical detail:
         * if we're handing off to the GIF task we DO NOT realloc viewfinder
         * buffers here. The GIF task needs to load 4×~600KB JPEGs before it
         * can free the viewfinder itself — but on CAMERA page there isn't
         * enough PSRAM to hold both simultaneously, so reallocating here
         * would starve the GIF task. The GIF task's own free/realloc pair
         * closes the cycle: buffers stay freed through capture + encode,
         * then come back once the GIF is done. */
        bool handed_off_to_gif = false;
        if (saved >= 2) {
            pimslo_gif_job_t job = { .capture_num = num };
            if (xQueueSend(s_gif_queue, &job, 0) != pdTRUE) {
                ESP_LOGW(TAG, "GIF queue full — dropping P4M%04d", num);
            } else {
                ESP_LOGI(TAG, "Queued GIF encode for P4M%04d (queue: %d)",
                         num, (int)uxQueueMessagesWaiting(s_gif_queue));
                handed_off_to_gif = true;
            }
        } else {
            ESP_LOGW(TAG, "Capture %03d: only %d cameras — skipping GIF", num, saved);
        }

        /* Restore viewfinder ONLY if we aren't passing PSRAM responsibility
         * to the GIF task. Keeps the viewfinder frozen end-to-end through a
         * successful PIMSLO cycle, but unfreezes it on a capture failure. */
        if (!handed_off_to_gif) {
            app_video_stream_realloc_buffers();
        }

        s_capturing = false;
    }
}

/* ---- GIF Encode Queue Task (Core 1, persistent) ---- */

static void pimslo_encode_queue_task(void *param)
{
    ESP_LOGI(TAG, "Encode queue task started (Core %d)", xPortGetCoreID());

    pimslo_gif_job_t job;

    while (1) {
        /* Block until a job is available */
        xQueueReceive(s_gif_queue, &job, portMAX_DELAY);

        /* Wait if album GIF encoding is in progress (shared PSRAM) */
        while (app_gifs_is_encoding()) {
            ESP_LOGI(TAG, "Waiting for album GIF encode to finish...");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        s_encoding = true;

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

    s_gif_queue = xQueueCreate(PIMSLO_QUEUE_DEPTH, sizeof(pimslo_gif_job_t));
    if (!s_gif_queue) return ESP_ERR_NO_MEM;

    /* SPI capture task on Core 0 (I/O bound — doesn't compete with GIF encode) */
    BaseType_t ret = xTaskCreatePinnedToCore(
        pimslo_capture_task, "pimslo_cap", 8192, NULL, 5, NULL, 0);
    if (ret != pdPASS) return ESP_FAIL;

    /* GIF encode queue task on Core 1 (CPU bound — dithering + LZW) */
    ret = xTaskCreatePinnedToCore(
        pimslo_encode_queue_task, "pimslo_gif", 16384, NULL, 4, NULL, 1);
    if (ret != pdPASS) return ESP_FAIL;

    s_initialized = true;
    ESP_LOGI(TAG, "PIMSLO subsystem initialized (queue depth=%d)", PIMSLO_QUEUE_DEPTH);
    return ESP_OK;
}

void app_pimslo_request_capture(void)
{
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

bool app_pimslo_is_capturing(void)
{
    return s_capturing;
}

#define P4_PHOTO_DIR "/sdcard/esp32_p4_pic_save"

esp_err_t app_pimslo_save_preview_from_latest_photo(uint16_t num)
{
    /* Scan the P4 photo directory for the highest-numbered pic_NNNN.jpg. */
    DIR *dir = opendir(P4_PHOTO_DIR);
    if (!dir) return ESP_ERR_NOT_FOUND;

    uint32_t best_num = 0;
    char best_name[32] = {0};
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        uint32_t n = 0;
        if (sscanf(entry->d_name, "pic_%lu.jpg", &n) == 1 && n > best_num) {
            best_num = n;
            strncpy(best_name, entry->d_name, sizeof(best_name) - 1);
        }
    }
    closedir(dir);

    if (best_num == 0) {
        ESP_LOGW(TAG, "No P4 photo found to copy as preview");
        return ESP_ERR_NOT_FOUND;
    }

    mkdir(PIMSLO_PREVIEW_DIR, 0755);

    char src[80], dst[80];
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
