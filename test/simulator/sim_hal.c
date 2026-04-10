/**
 * @file sim_hal.c
 * @brief Hardware abstraction stubs for the LVGL simulator
 *
 * Implements all app_* functions called by ui_extra.c as no-op stubs
 * that track state in a sim_hw_state_t struct for debug visibility.
 */

#include "sim_hal.h"
#include "esp_err.h"
#include "ui_extra.h"

#include <string.h>

/*-------------------------------------------------*/
/* Simulated state singleton                        */
/*-------------------------------------------------*/

static sim_hw_state_t s_hw = {0};

sim_hw_state_t *sim_hal_get_state(void) {
    return &s_hw;
}

void sim_hal_reset(void) {
    memset(&s_hw, 0, sizeof(s_hw));
    s_hw.album_image_count  = 5;
    s_hw.album_can_store    = true;
    s_hw.photo_resolution   = 1; /* 720P */
    s_hw.contrast           = 53;
    s_hw.saturation         = 63;
    s_hw.brightness         = 54;
    s_hw.hue                = 2;
}

/*-------------------------------------------------*/
/* app_storage.h stubs                              */
/*-------------------------------------------------*/

esp_err_t app_storage_init(void) {
    return ESP_OK;
}

esp_err_t app_storage_save_picture(const uint8_t *data, size_t len) {
    (void)data; (void)len;
    printf("[SIM] app_storage_save_picture(%zu bytes)\n", len);
    return ESP_OK;
}

esp_err_t app_storage_save_settings(settings_info_t *settings, uint16_t interval_time, uint16_t magnification) {
    (void)settings;
    s_hw.settings_saved = true;
    printf("[SIM] app_storage_save_settings(interval=%u, magnification=%u)\n", interval_time, magnification);
    return ESP_OK;
}

esp_err_t app_storage_load_settings(settings_info_t *settings, uint16_t *interval_time, uint16_t *magnification) {
    if (settings) {
        settings->gyroscope  = "Off";
        settings->od         = "On";
        settings->resolution = "1080P";
        settings->flash      = "On";
    }
    if (interval_time)  *interval_time  = 30;
    if (magnification)  *magnification  = 1;
    printf("[SIM] app_storage_load_settings\n");
    return ESP_OK;
}

esp_err_t app_storage_save_interval_state(bool is_active, uint32_t next_wake_time) {
    s_hw.interval_active = is_active;
    (void)next_wake_time;
    return ESP_OK;
}

esp_err_t app_storage_get_interval_state(bool *is_active, uint32_t *next_wake_time) {
    if (is_active) *is_active = s_hw.interval_active;
    if (next_wake_time) *next_wake_time = 0;
    return ESP_OK;
}

esp_err_t app_storage_save_camera_settings(uint32_t contrast, uint32_t saturation,
                                           uint32_t brightness, uint32_t hue) {
    s_hw.contrast   = contrast;
    s_hw.saturation = saturation;
    s_hw.brightness = brightness;
    s_hw.hue        = hue;
    printf("[SIM] app_storage_save_camera_settings(C=%u S=%u B=%u H=%u)\n",
           contrast, saturation, brightness, hue);
    return ESP_OK;
}

esp_err_t app_storage_load_camera_settings(uint32_t *contrast, uint32_t *saturation,
                                           uint32_t *brightness, uint32_t *hue) {
    if (contrast)   *contrast   = s_hw.contrast;
    if (saturation) *saturation = s_hw.saturation;
    if (brightness) *brightness = s_hw.brightness;
    if (hue)        *hue        = s_hw.hue;
    return ESP_OK;
}

esp_err_t app_storage_save_photo_count(uint16_t count) {
    s_hw.photo_count = count;
    return ESP_OK;
}

esp_err_t app_storage_get_photo_count(uint16_t *count) {
    if (count) *count = (uint16_t)s_hw.photo_count;
    return ESP_OK;
}

esp_err_t app_storage_save_gyroscope_setting(bool enabled) {
    s_hw.gyroscope_enabled = enabled;
    printf("[SIM] gyroscope=%s\n", enabled ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t app_storage_load_gyroscope_setting(bool *enabled) {
    if (enabled) *enabled = s_hw.gyroscope_enabled;
    return ESP_OK;
}

/*-------------------------------------------------*/
/* app_album.h stubs                                */
/*-------------------------------------------------*/

esp_err_t app_album_init(void *parent) {
    (void)parent;
    return ESP_OK;
}

esp_err_t app_album_next_image(void) {
    if (s_hw.album_image_count > 0) {
        s_hw.album_current_index = (s_hw.album_current_index + 1) % s_hw.album_image_count;
    }
    printf("[SIM] album_next -> index=%d\n", s_hw.album_current_index);
    return ESP_OK;
}

esp_err_t app_album_prev_image(void) {
    if (s_hw.album_image_count > 0) {
        s_hw.album_current_index = (s_hw.album_current_index - 1 + s_hw.album_image_count) % s_hw.album_image_count;
    }
    printf("[SIM] album_prev -> index=%d\n", s_hw.album_current_index);
    return ESP_OK;
}

esp_err_t app_album_refresh(void) {
    printf("[SIM] album_refresh (count=%d)\n", s_hw.album_image_count);
    return ESP_OK;
}

void app_album_deinit(void) {
    printf("[SIM] album_deinit\n");
}

int app_album_get_image_count(void) {
    return s_hw.album_image_count;
}

int app_album_get_current_index(void) {
    return s_hw.album_current_index;
}

esp_err_t app_album_delete_current_image(void) {
    if (s_hw.album_image_count > 0) {
        s_hw.album_image_count--;
        if (s_hw.album_current_index >= s_hw.album_image_count && s_hw.album_image_count > 0) {
            s_hw.album_current_index = s_hw.album_image_count - 1;
        }
    }
    printf("[SIM] album_delete -> count=%d\n", s_hw.album_image_count);
    return ESP_OK;
}

bool app_album_can_store_new_image(void) {
    return s_hw.album_can_store;
}

void app_album_photo_saved(void) {
    printf("[SIM] album_photo_saved\n");
}

bool app_video_stream_can_store_new_mp4(float estimated_size_mb) {
    (void)estimated_size_mb;
    return s_hw.album_can_store;
}

void app_album_enable_coco_od(bool enable) {
    s_hw.coco_od_enabled = enable;
    printf("[SIM] coco_od=%s\n", enable ? "ON" : "OFF");
}

/*-------------------------------------------------*/
/* app_video_stream.h stubs                         */
/*-------------------------------------------------*/

esp_err_t app_video_stream_take_photo(void) {
    s_hw.photo_count++;
    printf("[SIM] TAKE PHOTO (total=%d)\n", s_hw.photo_count);
    return ESP_OK;
}

esp_err_t app_video_stream_stop_take_photo(void) {
    return ESP_OK;
}

esp_err_t app_video_stream_take_video(void) {
    s_hw.recording_video = true;
    printf("[SIM] START VIDEO RECORDING\n");
    return ESP_OK;
}

esp_err_t app_video_stream_stop_take_video(void) {
    s_hw.recording_video = false;
    printf("[SIM] STOP VIDEO RECORDING\n");
    return ESP_OK;
}

esp_err_t app_video_stream_set_flash_light(bool is_on) {
    s_hw.flash_on = is_on;
    printf("[SIM] flash=%s\n", is_on ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t app_video_stream_start_interval_photo(uint16_t interval_minutes) {
    s_hw.interval_active  = true;
    s_hw.interval_minutes = interval_minutes;
    printf("[SIM] interval_start(%u min)\n", interval_minutes);
    return ESP_OK;
}

esp_err_t app_video_stream_stop_interval_photo(void) {
    s_hw.interval_active = false;
    printf("[SIM] interval_stop\n");
    return ESP_OK;
}

esp_err_t app_video_stream_check_interval_wakeup(void) {
    return ESP_FAIL;
}

int app_video_stream_get_photo_resolution(void) {
    return s_hw.photo_resolution;
}

esp_err_t app_video_stream_set_photo_resolution_by_string(const char *resolution_str) {
    if (!resolution_str) return ESP_FAIL;
    if (strcmp(resolution_str, "480P") == 0)       s_hw.photo_resolution = 0;
    else if (strcmp(resolution_str, "720P") == 0)   s_hw.photo_resolution = 1;
    else if (strcmp(resolution_str, "1080P") == 0)  s_hw.photo_resolution = 2;
    printf("[SIM] resolution=%s\n", resolution_str);
    return ESP_OK;
}

void app_video_stream_get_scaled_camera_buf(uint8_t **buf, uint32_t *size) {
    static uint8_t dummy[4] = {0};
    if (buf)  *buf  = dummy;
    if (size) *size = sizeof(dummy);
}

void app_video_stream_get_jpg_buf(uint8_t **buf, uint32_t *size) {
    static uint8_t dummy[4] = {0};
    if (buf)  *buf  = dummy;
    if (size) *size = sizeof(dummy);
}

void app_video_stream_get_shared_photo_buf(uint8_t **buf, uint32_t *size) {
    static uint8_t dummy[4] = {0};
    if (buf)  *buf  = dummy;
    if (size) *size = sizeof(dummy);
}

bool app_video_stream_get_flash_light_state(void) {
    return s_hw.flash_on;
}

bool app_video_stream_get_interval_photo_state(void) {
    return s_hw.interval_active;
}

uint16_t app_video_stream_get_current_interval_minutes(void) {
    return s_hw.interval_minutes;
}

int app_video_stream_get_video_fd(void) {
    return -1;
}

/*-------------------------------------------------*/
/* app_isp.h stubs                                  */
/*-------------------------------------------------*/

esp_err_t app_isp_init(int cam_fd) {
    (void)cam_fd;
    return ESP_OK;
}

esp_err_t app_isp_set_contrast(uint32_t percent) {
    s_hw.contrast = percent;
    return ESP_OK;
}

esp_err_t app_isp_set_saturation(uint32_t percent) {
    s_hw.saturation = percent;
    return ESP_OK;
}

esp_err_t app_isp_set_brightness(uint32_t percent) {
    s_hw.brightness = percent;
    return ESP_OK;
}

esp_err_t app_isp_set_hue(uint32_t percent) {
    s_hw.hue = percent;
    return ESP_OK;
}
