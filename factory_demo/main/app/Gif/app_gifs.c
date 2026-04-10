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

static const char *TAG = "app_gifs";

#define MAX_PATH_LEN  512
#define MAX_GIF_FILES 64

/* Photo source directory (same as album) */
#define PIC_FOLDER_NAME "esp32_p4_pic_save"

typedef struct {
    lv_obj_t *canvas;
    uint16_t *canvas_buffer;
    int canvas_width, canvas_height;

    char **filenames;
    int count;
    int current_index;

    gif_decoder_t *decoder;
    lv_timer_t *play_timer;
    bool is_playing;
    bool is_encoding;

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

static void playback_timer_cb(lv_timer_t *timer)
{
    if (!s_ctx.is_playing || !s_ctx.decoder || !s_ctx.canvas_buffer) {
        return;
    }

    int delay_cs = 10;
    esp_err_t ret = gif_decoder_next_frame(s_ctx.decoder, s_ctx.canvas_buffer, &delay_cs);

    if (ret == ESP_ERR_NOT_FOUND) {
        /* Loop: reset to first frame */
        gif_decoder_reset(s_ctx.decoder);
        ret = gif_decoder_next_frame(s_ctx.decoder, s_ctx.canvas_buffer, &delay_cs);
        if (ret != ESP_OK) {
            app_gifs_stop();
            return;
        }
    } else if (ret != ESP_OK) {
        app_gifs_stop();
        return;
    }

    /* Update canvas */
    bsp_display_lock(0);
    lv_canvas_set_buffer(s_ctx.canvas, s_ctx.canvas_buffer,
                         s_ctx.canvas_width, s_ctx.canvas_height,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_invalidate(s_ctx.canvas);
    bsp_display_unlock();

    /* Adjust timer period for next frame */
    if (delay_cs > 0) {
        lv_timer_set_period(timer, delay_cs * 10);  /* centiseconds → milliseconds */
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

    ESP_LOGI(TAG, "Creating GIF from %d JPEGs", jpeg_count);

    /* Create encoder */
    gif_encoder_config_t cfg = {
        .frame_delay_cs = p->frame_delay_ms / 10,
        .loop_count = 0,
        .target_width = BSP_LCD_H_RES,
        .target_height = BSP_LCD_V_RES,
    };

    gif_encoder_t *enc = NULL;
    ret = gif_encoder_create(&cfg, &enc);
    if (ret != ESP_OK) goto cleanup;

    gif_encoder_set_progress_cb(enc, encode_progress_cb, NULL);

    /* Pass 1: build palette */
    for (int i = 0; i < jpeg_count; i++) {
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

    for (int i = 0; i < jpeg_count; i++) {
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

    return ESP_OK;
}

void app_gifs_deinit(void)
{
    app_gifs_stop();

    if (s_ctx.filenames) {
        for (int i = 0; i < s_ctx.count; i++) {
            if (s_ctx.filenames[i]) free(s_ctx.filenames[i]);
        }
        free(s_ctx.filenames);
        s_ctx.filenames = NULL;
    }

    if (s_ctx.canvas_buffer) {
        heap_caps_free(s_ctx.canvas_buffer);
        s_ctx.canvas_buffer = NULL;
    }

    s_ctx.count = 0;
}

esp_err_t app_gifs_scan(void)
{
    /* Free old filenames */
    if (s_ctx.filenames) {
        for (int i = 0; i < s_ctx.count; i++) {
            if (s_ctx.filenames[i]) free(s_ctx.filenames[i]);
        }
        free(s_ctx.filenames);
        s_ctx.filenames = NULL;
        s_ctx.count = 0;
    }

    ensure_gif_dir();

    char gif_dir[MAX_PATH_LEN];
    snprintf(gif_dir, sizeof(gif_dir), "%s/%s", BSP_SD_MOUNT_POINT, GIF_FOLDER_NAME);

    DIR *dir = opendir(gif_dir);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s", gif_dir);
        return ESP_OK;  /* Not an error, just no GIFs */
    }

    s_ctx.filenames = heap_caps_calloc(MAX_GIF_FILES, sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!s_ctx.filenames) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_ctx.count < MAX_GIF_FILES) {
        if (!is_gif_file(entry->d_name)) continue;
        if (entry->d_name[0] == '.') continue;

        s_ctx.filenames[s_ctx.count] = heap_caps_malloc(MAX_PATH_LEN, MALLOC_CAP_SPIRAM);
        snprintf(s_ctx.filenames[s_ctx.count], MAX_PATH_LEN, "%.200s/%.255s", gif_dir, entry->d_name);
        s_ctx.count++;
    }
    closedir(dir);

    s_ctx.current_index = 0;
    ESP_LOGI(TAG, "Found %d GIF files", s_ctx.count);
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

esp_err_t app_gifs_play_current(void)
{
    if (s_ctx.count == 0 || !s_ctx.canvas_buffer) return ESP_FAIL;

    app_gifs_stop();

    esp_err_t ret = gif_decoder_open(s_ctx.filenames[s_ctx.current_index], &s_ctx.decoder);
    if (ret != ESP_OK) return ret;

    s_ctx.is_playing = true;

    /* Create LVGL timer for frame-paced playback */
    s_ctx.play_timer = lv_timer_create(playback_timer_cb, 100, NULL);
    if (!s_ctx.play_timer) {
        app_gifs_stop();
        return ESP_FAIL;
    }

    /* Decode and display the first frame immediately */
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

    if (s_ctx.decoder) {
        gif_decoder_close(s_ctx.decoder);
        s_ctx.decoder = NULL;
    }
}

bool app_gifs_is_playing(void) { return s_ctx.is_playing; }

esp_err_t app_gifs_create_from_album(int frame_delay_ms)
{
    if (s_ctx.is_encoding) {
        ESP_LOGW(TAG, "Encoding already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    encode_task_params_t *params = malloc(sizeof(encode_task_params_t));
    if (!params) return ESP_ERR_NO_MEM;
    params->frame_delay_ms = (frame_delay_ms > 0) ? frame_delay_ms : 500;

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
