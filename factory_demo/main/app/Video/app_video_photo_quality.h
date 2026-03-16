/**
 * @file app_video_photo_quality.h
 * @brief Photo quality settings API
 *
 * This module provides configurable JPEG quality settings for photo capture,
 * with save/load persistence to NVS storage.
 */

#ifndef APP_VIDEO_PHOTO_QUALITY_H
#define APP_VIDEO_PHOTO_QUALITY_H

#include <esp_err.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief JPEG quality range
 */
#define PHOTO_QUALITY_MIN   10      // Minimum quality (10%)
#define PHOTO_QUALITY_MAX   100     // Maximum quality (100%)
#define PHOTO_QUALITY_DEFAULT 90    // Default quality setting

/**
 * @brief Photo quality settings structure
 */
typedef struct {
    uint8_t jpeg_quality;          // JPEG compression quality (1-100)
    uint8_t brightness;            // Image brightness adjustment (-50 to +50)
    uint8_t contrast;              // Image contrast adjustment (-50 to +50)
    uint8_t saturation;            // Color saturation adjustment (-50 to +50)
} photo_quality_settings_t;

/**
 * @brief Initialize photo quality settings module
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_init(void);

/**
 * @brief Deinitialize photo quality settings module
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_deinit(void);

/**
 * @brief Get current photo quality settings
 *
 * @param settings Pointer to store settings (must be valid)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_get_settings(photo_quality_settings_t *settings);

/**
 * @brief Set JPEG quality value
 *
 * @param quality JPEG quality value (1-100)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_set_jpeg_quality(uint8_t quality);

/**
 * @brief Set brightness adjustment
 *
 * @param brightness Brightness value (-50 to +50)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_set_brightness(int8_t brightness);

/**
 * @brief Set contrast adjustment
 *
 * @param contrast Contrast value (-50 to +50)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_set_contrast(int8_t contrast);

/**
 * @brief Set saturation adjustment
 *
 * @param saturation Saturation value (-50 to +50)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_set_saturation(int8_t saturation);

/**
 * @brief Save current settings to NVS
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_save(void);

/**
 * @brief Load settings from NVS
 *
 * @return ESP_OK on success, error code otherwise (settings not found)
 */
esp_err_t app_photo_quality_load(void);

/**
 * @brief Reset to default quality settings
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_photo_quality_reset_defaults(void);

/**
 * @brief Validate quality settings range
 *
 * @param settings Settings to validate
 * @return true if valid, false otherwise
 */
bool app_photo_quality_validate_settings(const photo_quality_settings_t *settings);

#ifdef __cplusplus
}
#endif

#endif /* APP_VIDEO_PHOTO_QUALITY_H */
