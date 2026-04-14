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
#include "driver/jpeg_decode.h"

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

static const char *TAG = "app_gifs";

#define MAX_PATH_LEN  512
#define MAX_GIF_FILES 64

/* Photo source directory (same as album) */
#define PIC_FOLDER_NAME "esp32_p4_pic_save"

typedef struct {
    lv_obj_t *canvas;
    uint16_t *canvas_buffer;       /* 240x240 for display */
    int canvas_width, canvas_height;

    uint16_t *decode_buffer;       /* GIF native resolution for decoder output */
    int decode_width, decode_height;

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

/* Scale a GIF frame from native resolution to 240x240 display using nearest-neighbor */
static void scale_to_display(const uint16_t *src, int src_w, int src_h,
                             uint16_t *dst, int dst_w, int dst_h)
{
    for (int y = 0; y < dst_h; y++) {
        int sy = y * src_h / dst_h;
        if (sy >= src_h) sy = src_h - 1;
        for (int x = 0; x < dst_w; x++) {
            int sx = x * src_w / dst_w;
            if (sx >= src_w) sx = src_w - 1;
            dst[y * dst_w + x] = src[sy * src_w + sx];
        }
    }
}

/* Convert RGB888 palette color to RGB565 with byte swap for LVGL */
static inline uint16_t rgb888_to_rgb565_swapped(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t px = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (px >> 8) | (px << 8);  /* Byte swap for LV_COLOR_16_SWAP */
}

static void playback_timer_cb(lv_timer_t *timer)
{
    if (!s_ctx.is_playing || !s_ctx.decoder || !s_ctx.decode_buffer) {
        return;
    }

    int delay_cs = 10;
    esp_err_t ret = gif_decoder_next_frame(s_ctx.decoder, s_ctx.decode_buffer, &delay_cs);

    if (ret == ESP_ERR_NOT_FOUND) {
        /* Loop: reset to first frame */
        gif_decoder_reset(s_ctx.decoder);
        ret = gif_decoder_next_frame(s_ctx.decoder, s_ctx.decode_buffer, &delay_cs);
        if (ret != ESP_OK) {
            app_gifs_stop();
            return;
        }
    } else if (ret != ESP_OK) {
        app_gifs_stop();
        return;
    }

    /* Scale GIF frame to display size */
    scale_to_display(s_ctx.decode_buffer, s_ctx.decode_width, s_ctx.decode_height,
                     s_ctx.canvas_buffer, s_ctx.canvas_width, s_ctx.canvas_height);

    /* Update canvas */
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

    /* Free camera buffers to make PSRAM available for JPEG decoding */
    app_video_stream_free_buffers();
    ESP_LOGI(TAG, "Free PSRAM after releasing camera buffers: %zu",
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

    /* Restore camera buffers */
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

    if (s_ctx.decode_buffer) {
        heap_caps_free(s_ctx.decode_buffer);
        s_ctx.decode_buffer = NULL;
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
        return ESP_OK;
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
    if (s_ctx.count == 0 || !s_ctx.canvas_buffer) {
        ESP_LOGE(TAG, "Cannot play: count=%d canvas=%p", s_ctx.count, s_ctx.canvas_buffer);
        return ESP_FAIL;
    }

    app_gifs_stop();

    ESP_LOGI(TAG, "Playing: %s", s_ctx.filenames[s_ctx.current_index]);

    esp_err_t ret = gif_decoder_open(s_ctx.filenames[s_ctx.current_index], &s_ctx.decoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open GIF: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Allocate decode buffer matching GIF dimensions */
    s_ctx.decode_width = gif_decoder_get_width(s_ctx.decoder);
    s_ctx.decode_height = gif_decoder_get_height(s_ctx.decoder);

    if (s_ctx.decode_buffer) {
        heap_caps_free(s_ctx.decode_buffer);
        s_ctx.decode_buffer = NULL;
    }

    size_t decode_buf_size = s_ctx.decode_width * s_ctx.decode_height * 2;
    s_ctx.decode_buffer = heap_caps_malloc(decode_buf_size, MALLOC_CAP_SPIRAM);
    if (!s_ctx.decode_buffer) {
        ESP_LOGE(TAG, "Failed to allocate %zu byte decode buffer", decode_buf_size);
        gif_decoder_close(s_ctx.decoder);
        s_ctx.decoder = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "GIF: %dx%d, decode buffer: %zu bytes",
             s_ctx.decode_width, s_ctx.decode_height, decode_buf_size);

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

    if (s_ctx.decode_buffer) {
        heap_caps_free(s_ctx.decode_buffer);
        s_ctx.decode_buffer = NULL;
    }
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

static void pimslo_encode_task(void *param)
{
    pimslo_task_params_t *p = (pimslo_task_params_t *)param;
    esp_err_t ret;

    /* Read all 4 JPEGs into memory */
    uint8_t *jpeg_data[4] = {NULL};
    size_t jpeg_size[4] = {0};

    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/pimslo/pos%d.jpg", i + 1);
        FILE *f = fopen(path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Cannot open %s", path);
            goto cleanup;
        }
        fseek(f, 0, SEEK_END);
        jpeg_size[i] = ftell(f);
        fseek(f, 0, SEEK_SET);
        jpeg_data[i] = heap_caps_malloc(jpeg_size[i], MALLOC_CAP_SPIRAM);
        if (!jpeg_data[i]) {
            fclose(f);
            ESP_LOGE(TAG, "OOM for %s (%zu bytes)", path, jpeg_size[i]);
            goto cleanup;
        }
        fread(jpeg_data[i], 1, jpeg_size[i], f);
        fclose(f);
        ESP_LOGI(TAG, "Loaded %s: %zu bytes", path, jpeg_size[i]);
    }

    /* Get source dimensions from first JPEG */
    jpeg_decode_picture_info_t info;
    ret = jpeg_decoder_get_info(jpeg_data[0], jpeg_size[0], &info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read JPEG header");
        goto cleanup;
    }

    int src_w = info.width, src_h = info.height;
    float strength = p->parallax;
    int total_crop = (int)(src_w * strength);
    int crop_w = src_w - total_crop;

    ESP_LOGI(TAG, "PIMSLO: %dx%d source, parallax=%.2f, crop_w=%d",
             src_w, src_h, strength, crop_w);

    /* Calculate parallax crop rects for each position */
    gif_crop_rect_t crops[4];
    for (int i = 0; i < 4; i++) {
        float crop_ratio = (float)i / 3.0f;
        crops[i].x = (int)(crop_ratio * total_crop);
        crops[i].y = 0;
        crops[i].w = crop_w;
        crops[i].h = src_h;
        ESP_LOGI(TAG, "  Pos %d: crop(%d, 0, %d, %d)", i+1, crops[i].x, crop_w, src_h);
    }

    /* Free camera buffers for PSRAM */
    app_video_stream_free_buffers();

    /* Free the JPEG data buffers — they'll be re-read from SD for each pass */
    for (int i = 0; i < 4; i++) {
        heap_caps_free(jpeg_data[i]);
        jpeg_data[i] = NULL;
    }

    /* Create encoder — full resolution, no downscaling */
    gif_encoder_config_t cfg = {
        .frame_delay_cs = p->frame_delay_ms / 10,
        .loop_count = 0,
        .target_width = crop_w,
        .target_height = src_h,
    };
    ESP_LOGI(TAG, "PIMSLO GIF: %dx%d (full resolution)", crop_w, src_h);

    gif_encoder_t *enc = NULL;
    ret = gif_encoder_create(&cfg, &enc);
    if (ret != ESP_OK) goto cleanup_buffers;

    /* Pass 1: Build palette from all 4 unique frames (re-read each from SD) */
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/pimslo/pos%d.jpg", i + 1);
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

    /* Pass 2: Encode oscillating 7-frame sequence (1→2→3→4→3→2→1) */
    ensure_gif_dir();
    char output_path[MAX_PATH_LEN];
    snprintf(output_path, sizeof(output_path), "%s/%s/pimslo_%ld.gif",
             BSP_SD_MOUNT_POINT, GIF_FOLDER_NAME, (long)esp_log_timestamp());

    ret = gif_encoder_pass2_begin(enc, output_path);
    if (ret != ESP_OK) goto cleanup_enc;

    int frame_order[] = {0, 1, 2, 3, 2, 1, 0};
    for (int f = 0; f < 7; f++) {
        int idx = frame_order[f];
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/pimslo/pos%d.jpg", idx + 1);
        FILE *ff = fopen(path, "rb");
        if (!ff) { ESP_LOGW(TAG, "Cannot reopen %s", path); continue; }
        fseek(ff, 0, SEEK_END);
        size_t sz = ftell(ff);
        fseek(ff, 0, SEEK_SET);
        uint8_t *data = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (!data) { fclose(ff); continue; }
        fread(data, 1, sz, ff);
        fclose(ff);
        ret = gif_encoder_pass2_add_frame_from_buffer(enc, data, sz, &crops[idx]);
        heap_caps_free(data);
        if (ret != ESP_OK)
            ESP_LOGW(TAG, "Pass 2 failed for frame %d (pos %d)", f, idx+1);
    }

    ret = gif_encoder_pass2_finish(enc);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PIMSLO GIF saved to %s", output_path);
    }

cleanup_enc:
    gif_encoder_destroy(enc);
cleanup_buffers:
    app_video_stream_realloc_buffers();
cleanup:
    for (int i = 0; i < 4; i++) {
        if (jpeg_data[i]) heap_caps_free(jpeg_data[i]);
    }
    free(p);
    s_ctx.is_encoding = false;
    app_gifs_scan();
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
