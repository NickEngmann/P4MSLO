/**
 * @file app_video_photo_quality.h
 * @brief Photo quality configuration module header
 * 
 * This header provides the API for configuring photo quality settings
 * including JPEG compression, exposure, and image enhancement parameters.
 */

#ifndef APP_VIDEO_PHOTO_QUALITY_H
#define APP_VIDEO_PHOTO_QUALITY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Photo quality preset levels
 */
typedef enum {
    QUALITY_LOW = 0,
    QUALITY_MEDIUM = 1,
    QUALITY_HIGH = 2
} photo_quality_level_t;

/**
 * @brief Photo quality configuration structure
 */
typedef struct {
    uint8_t jpeg_quality;       /* JPEG compression quality (1-100) */
    int8_t sharpness;           /* Image sharpness (-5 to +5) */
    int8_t contrast;            /* Contrast adjustment (-5 to +5) */
    int8_t brightness;          /* Brightness adjustment (-5 to +5) */
    int8_t saturation;          /* Color saturation (-5 to +5) */
} photo_quality_config_t;

/**
 * @brief Initialize photo quality module
 * 
 * Loads default settings from NVS storage.
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t photo_quality_init(void);

/**
 * @brief Shutdown photo quality module
 * 
 * Saves current settings to NVS storage.
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t photo_quality_shutdown(void);

/**
 * @brief Get current photo quality level
 * 
 * @return Current quality level (LOW, MEDIUM, HIGH)
 */
photo_quality_level_t photo_quality_get_level(void);

/**
 * @brief Set photo quality level
 * 
 * @param level Quality level to set
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if level is invalid
 */
esp_err_t photo_quality_set_level(photo_quality_level_t level);

/**
 * @brief Get current JPEG quality setting
 * 
 * @return JPEG quality value (1-100)
 */
uint8_t photo_quality_get_jpeg_quality(void);

/**
 * @brief Set JPEG quality
 * 
 * @param quality JPEG quality value (1-100)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out of range
 */
esp_err_t photo_quality_set_jpeg_quality(uint8_t quality);

/**
 * @brief Get current sharpness setting
 * 
 * @return Sharpness value (-5 to +5)
 */
int8_t photo_quality_get_sharpness(void);

/**
 * @brief Set sharpness
 * 
 * @param value Sharpness value (-5 to +5)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out of range
 */
esp_err_t photo_quality_set_sharpness(int8_t value);

/**
 * @brief Get current contrast setting
 * 
 * @return Contrast value (-5 to +5)
 */
int8_t photo_quality_get_contrast(void);

/**
 * @brief Set contrast
 * 
 * @param value Contrast value (-5 to +5)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out of range
 */
esp_err_t photo_quality_set_contrast(int8_t value);

/**
 * @brief Get current brightness setting
 * 
 * @return Brightness value (-5 to +5)
 */
int8_t photo_quality_get_brightness(void);

/**
 * @brief Set brightness
 * 
 * @param value Brightness value (-5 to +5)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out of range
 */
esp_err_t photo_quality_set_brightness(int8_t value);

/**
 * @brief Get current saturation setting
 * 
 * @return Saturation value (-5 to +5)
 */
int8_t photo_quality_get_saturation(void);

/**
 * @brief Set saturation
 * 
 * @param value Saturation value (-5 to +5)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out of range
 */
esp_err_t photo_quality_set_saturation(int8_t value);

/**
 * @brief Get full photo quality configuration
 * 
 * @param config Pointer to store configuration (must not be NULL)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if config is NULL
 */
esp_err_t photo_quality_get_config(photo_quality_config_t *config);

/**
 * @brief Set photo quality configuration
 * 
 * @param config Configuration to apply (must not be NULL)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t photo_quality_set_config(const photo_quality_config_t *config);

/**
 * @brief Reset to default settings
 * 
 * @return ESP_OK on success
 */
esp_err_t photo_quality_reset_defaults(void);

/**
 * @brief Get string representation of quality level
 * 
 * @param level Quality level
 * @return String representation ("Low", "Medium", or "High")
 */
const char* quality_to_string(photo_quality_level_t level);

#ifdef __cplusplus
}
#endif

#endif /* APP_VIDEO_PHOTO_QUALITY_H */
