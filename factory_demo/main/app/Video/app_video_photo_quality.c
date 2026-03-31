/**
 * @file app_video_photo_quality.c
 * @brief Photo quality settings implementation
 *
 * This module provides configurable JPEG quality settings for photo capture,
 * with save/load persistence to NVS storage.
 */

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "app_video_photo_quality.h"

static const char *TAG = "photo_quality";
static const char *QUALITY_NS = "photo_quality";

/* Static settings storage */
static photo_quality_settings_t g_settings = {
    .jpeg_quality = PHOTO_QUALITY_DEFAULT,
    .brightness = 0,
    .contrast = 0,
    .saturation = 0
};

/* Forward declarations for NVS operations */
static esp_err_t save_settings_to_nvs(void);
static esp_err_t load_settings_from_nvs(void);

/**
 * @brief Initialize photo quality settings module
 *
 * Loads settings from NVS if available, otherwise uses defaults.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_init(void)
{
    esp_err_t ret = load_settings_from_nvs();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Loaded settings from NVS: JPEG quality=%d", g_settings.jpeg_quality);
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved settings found, using defaults: JPEG quality=%d",
                 g_settings.jpeg_quality);
    } else {
        ESP_LOGE(TAG, "Failed to load settings from NVS: 0x%x", ret);
        /* Initialize with defaults on error */
        g_settings.jpeg_quality = PHOTO_QUALITY_DEFAULT;
        g_settings.brightness = 0;
        g_settings.contrast = 0;
        g_settings.saturation = 0;
    }

    return ESP_OK;
}

/**
 * @brief Deinitialize photo quality settings module
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_deinit(void)
{
    /* Save current settings before deinit */
    esp_err_t ret = save_settings_to_nvs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save settings before deinit: 0x%x", ret);
    }

    memset(&g_settings, 0, sizeof(g_settings));
    return ESP_OK;
}

/**
 * @brief Get current photo quality settings
 *
 * @param settings Pointer to store settings (must be valid)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_get_settings(photo_quality_settings_t *settings)
{
    if (settings == NULL) {
        ESP_LOGE(TAG, "Settings pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(settings, &g_settings, sizeof(photo_quality_settings_t));
    return ESP_OK;
}

/**
 * @brief Set JPEG quality value
 *
 * @param quality JPEG quality value (1-100)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_set_jpeg_quality(uint8_t quality)
{
    if (quality < PHOTO_QUALITY_MIN || quality > PHOTO_QUALITY_MAX) {
        ESP_LOGE(TAG, "Invalid JPEG quality: %d (must be %d-%d)",
                 quality, PHOTO_QUALITY_MIN, PHOTO_QUALITY_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    g_settings.jpeg_quality = quality;
    ESP_LOGI(TAG, "JPEG quality set to: %d%%", quality);

    return ESP_OK;
}

/**
 * @brief Set brightness adjustment
 *
 * @param brightness Brightness value (-50 to +50)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_set_brightness(int8_t brightness)
{
    if (brightness < -50 || brightness > 50) {
        ESP_LOGE(TAG, "Invalid brightness: %d (must be -50 to +50)", brightness);
        return ESP_ERR_INVALID_ARG;
    }

    g_settings.brightness = (uint8_t)(brightness < 0 ? 0 : brightness);
    ESP_LOGI(TAG, "Brightness set to: %d", g_settings.brightness);

    return ESP_OK;
}

/**
 * @brief Set contrast adjustment
 *
 * @param contrast Contrast value (-50 to +50)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_set_contrast(int8_t contrast)
{
    if (contrast < -50 || contrast > 50) {
        ESP_LOGE(TAG, "Invalid contrast: %d (must be -50 to +50)", contrast);
        return ESP_ERR_INVALID_ARG;
    }

    g_settings.contrast = (uint8_t)(contrast < 0 ? 0 : contrast);
    ESP_LOGI(TAG, "Contrast set to: %d", g_settings.contrast);

    return ESP_OK;
}

/**
 * @brief Set saturation adjustment
 *
 * @param saturation Saturation value (-50 to +50)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_set_saturation(int8_t saturation)
{
    if (saturation < -50 || saturation > 50) {
        ESP_LOGE(TAG, "Invalid saturation: %d (must be -50 to +50)", saturation);
        return ESP_ERR_INVALID_ARG;
    }

    g_settings.saturation = (uint8_t)(saturation < 0 ? 0 : saturation);
    ESP_LOGI(TAG, "Saturation set to: %d", g_settings.saturation);

    return ESP_OK;
}

/**
 * @brief Save current settings to NVS
 *
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t save_settings_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(QUALITY_NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: 0x%x", ret);
        return ret;
    }

    ret = nvs_set_blob(handle, "photo_settings", &g_settings, sizeof(g_settings));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save settings to NVS: 0x%x", ret);
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: 0x%x", ret);
    }

    nvs_close(handle);
    return ret;
}

/**
 * @brief Load settings from NVS
 *
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if no saved settings
 */
static esp_err_t load_settings_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(QUALITY_NS, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: 0x%x", ret);
        return ret;
    }

    size_t required_size = sizeof(g_settings);
    ret = nvs_get_blob(handle, "photo_settings", &g_settings, &required_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load settings from NVS: 0x%x", ret);
        nvs_close(handle);
        return ret;
    }

    if (required_size != sizeof(g_settings)) {
        ESP_LOGW(TAG, "Settings size mismatch: expected %zu, got %zu",
                 sizeof(g_settings), required_size);
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Settings loaded successfully");
    nvs_close(handle);
    return ESP_OK;
}

/**
 * @brief Save current settings to NVS
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_save(void)
{
    return save_settings_to_nvs();
}

/**
 * @brief Load settings from NVS
 *
 * @return ESP_OK on success, error code otherwise (settings not found)
 */
esp_err_t app_photo_quality_load(void)
{
    return load_settings_from_nvs();
}

/**
 * @brief Reset to default quality settings
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_reset_defaults(void)
{
    g_settings.jpeg_quality = PHOTO_QUALITY_DEFAULT;
    g_settings.brightness = 0;
    g_settings.contrast = 0;
    g_settings.saturation = 0;

    ESP_LOGI(TAG, "Reset to defaults: JPEG quality=%d%%", g_settings.jpeg_quality);

    /* Save defaults to NVS */
    return save_settings_to_nvs();
}

/**
 * @brief Validate quality settings range
 *
 * @param settings Settings to validate
 * @return true if valid, false otherwise
 */
bool app_photo_quality_validate_settings(const photo_quality_settings_t *settings)
{
    if (settings == NULL) {
        return false;
    }

    if (settings->jpeg_quality < PHOTO_QUALITY_MIN ||
        settings->jpeg_quality > PHOTO_QUALITY_MAX) {
        return false;
    }

    /* Check brightness, contrast, saturation (stored as unsigned, 0-50 range) */
    if (settings->brightness > 50 ||
        settings->contrast > 50 ||
        settings->saturation > 50) {
        return false;
    }

    return true;
}
