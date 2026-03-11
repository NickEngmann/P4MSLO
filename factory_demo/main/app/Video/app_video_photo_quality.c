/**
 * @file app_video_photo_quality.c
 * @brief Photo quality configuration module for camera application
 * 
 * This module provides functionality to configure photo quality settings
 * including JPEG compression levels and resolution presets.
 */

#include "app_video_photo.h"
#include "app_video_photo_quality.h"
#include <string.h>
#include <stdlib.h>

/**
 * @brief Photo quality configuration structure
 */
static struct {
    uint8_t jpeg_quality;        // 0-100 (100 = highest quality)
    uint8_t auto_exposure;       // 0 = manual, 1 = auto
    uint8_t auto_white_balance;  // 0 = manual, 1 = auto
    uint8_t brightness;          // -5 to +5
    uint8_t contrast;            // -5 to +5
    uint8_t saturation;          // -5 to +5
    uint8_t sharpness;           // -5 to +5
    uint8_t noise_reduction;     // 0-10
    uint8_t face_detection;      // 0 = off, 1 = on
    uint8_t timer_enabled;       // 0 = off, 1 = on
    uint32_t timer_seconds;      // timer duration in seconds
    uint8_t burst_mode;          // 0 = single, >0 = burst count
    uint8_t hdr_mode;            // 0 = off, 1 = on
} photo_config = {
    .jpeg_quality = 90,
    .auto_exposure = 1,
    .auto_white_balance = 1,
    .brightness = 0,
    .contrast = 0,
    .saturation = 0,
    .sharpness = 0,
    .noise_reduction = 5,
    .face_detection = 0,
    .timer_enabled = 0,
    .timer_seconds = 3,
    .burst_mode = 0,
    .hdr_mode = 0
};

/**
 * @brief Initialize photo quality settings with defaults
 * 
 * @return ESP_OK on success
 */
esp_err_t photo_quality_init(void) {
    photo_config.jpeg_quality = 90;
    photo_config.auto_exposure = 1;
    photo_config.auto_white_balance = 1;
    photo_config.brightness = 0;
    photo_config.contrast = 0;
    photo_config.saturation = 0;
    photo_config.sharpness = 0;
    photo_config.noise_reduction = 5;
    photo_config.face_detection = 0;
    photo_config.timer_enabled = 0;
    photo_config.timer_seconds = 3;
    photo_config.burst_mode = 0;
    photo_config.hdr_mode = 0;
    return ESP_OK;
}

/**
 * @brief Set JPEG quality (0-100)
 * 
 * @param quality Quality level from 0 (lowest) to 100 (highest)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid quality
 */
esp_err_t photo_quality_set_jpeg_quality(uint8_t quality) {
    if (quality > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    photo_config.jpeg_quality = quality;
    return ESP_OK;
}

/**
 * @brief Get current JPEG quality
 * 
 * @return JPEG quality value (0-100)
 */
uint8_t photo_quality_get_jpeg_quality(void) {
    return photo_config.jpeg_quality;
}

/**
 * @brief Enable/disable auto exposure
 * 
 * @param enabled 1 = auto exposure, 0 = manual
 * @return ESP_OK on success
 */
esp_err_t photo_quality_set_auto_exposure(uint8_t enabled) {
    photo_config.auto_exposure = (enabled != 0) ? 1 : 0;
    return ESP_OK;
}

/**
 * @brief Get auto exposure status
 * 
 * @return 1 if auto exposure enabled, 0 if manual
 */
uint8_t photo_quality_get_auto_exposure(void) {
    return photo_config.auto_exposure;
}

/**
 * @brief Enable/disable auto white balance
 * 
 * @param enabled 1 = auto white balance, 0 = manual
 * @return ESP_OK on success
 */
esp_err_t photo_quality_set_auto_white_balance(uint8_t enabled) {
    photo_config.auto_white_balance = (enabled != 0) ? 1 : 0;
    return ESP_OK;
}

/**
 * @brief Get auto white balance status
 * 
 * @return 1 if auto white balance enabled, 0 if manual
 */
uint8_t photo_quality_get_auto_white_balance(void) {
    return photo_config.auto_white_balance;
}

/**
 * @brief Set brightness adjustment (-5 to +5)
 * 
 * @param level Brightness level from -5 (darker) to +5 (brighter)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid level
 */
esp_err_t photo_quality_set_brightness(int8_t level) {
    if (level < -5 || level > 5) {
        return ESP_ERR_INVALID_ARG;
    }
    photo_config.brightness = (uint8_t)(level + 5); // Store as 0-10 internally
    return ESP_OK;
}

/**
 * @brief Get current brightness level
 * 
 * @return Brightness level (-5 to +5)
 */
int8_t photo_quality_get_brightness(void) {
    return (int8_t)(photo_config.brightness - 5);
}

/**
 * @brief Set contrast adjustment (-5 to +5)
 * 
 * @param level Contrast level from -5 (low contrast) to +5 (high contrast)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid level
 */
esp_err_t photo_quality_set_contrast(int8_t level) {
    if (level < -5 || level > 5) {
        return ESP_ERR_INVALID_ARG;
    }
    photo_config.contrast = (uint8_t)(level + 5);
    return ESP_OK;
}

/**
 * @brief Get current contrast level
 * 
 * @return Contrast level (-5 to +5)
 */
int8_t photo_quality_get_contrast(void) {
    return (int8_t)(photo_config.contrast - 5);
}

/**
 * @brief Set saturation adjustment (-5 to +5)
 * 
 * @param level Saturation level from -5 (grayscale) to +5 (vivid)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid level
 */
esp_err_t photo_quality_set_saturation(int8_t level) {
    if (level < -5 || level > 5) {
        return ESP_ERR_INVALID_ARG;
    }
    photo_config.saturation = (uint8_t)(level + 5);
    return ESP_OK;
}

/**
 * @brief Get current saturation level
 * 
 * @return Saturation level (-5 to +5)
 */
int8_t photo_quality_get_saturation(void) {
    return (int8_t)(photo_config.saturation - 5);
}

/**
 * @brief Set sharpness adjustment (-5 to +5)
 * 
 * @param level Sharpness level from -5 (blurry) to +5 (sharp)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid level
 */
esp_err_t photo_quality_set_sharpness(int8_t level) {
    if (level < -5 || level > 5) {
        return ESP_ERR_INVALID_ARG;
    }
    photo_config.sharpness = (uint8_t)(level + 5);
    return ESP_OK;
}

/**
 * @brief Get current sharpness level
 * 
 * @return Sharpness level (-5 to +5)
 */
int8_t photo_quality_get_sharpness(void) {
    return (int8_t)(photo_config.sharpness - 5);
}

/**
 * @brief Set noise reduction level (0-10)
 * 
 * @param level Noise reduction level from 0 (none) to 10 (maximum)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid level
 */
esp_err_t photo_quality_set_noise_reduction(uint8_t level) {
    if (level > 10) {
        return ESP_ERR_INVALID_ARG;
    }
    photo_config.noise_reduction = level;
    return ESP_OK;
}

/**
 * @brief Get current noise reduction level
 * 
 * @return Noise reduction level (0-10)
 */
uint8_t photo_quality_get_noise_reduction(void) {
    return photo_config.noise_reduction;
}

/**
 * @brief Enable/disable face detection
 * 
 * @param enabled 1 = face detection on, 0 = off
 * @return ESP_OK on success
 */
esp_err_t photo_quality_set_face_detection(uint8_t enabled) {
    photo_config.face_detection = (enabled != 0) ? 1 : 0;
    return ESP_OK;
}

/**
 * @brief Get face detection status
 * 
 * @return 1 if face detection enabled, 0 if disabled
 */
uint8_t photo_quality_get_face_detection(void) {
    return photo_config.face_detection;
}

/**
 * @brief Enable/disable timer
 * 
 * @param enabled 1 = timer on, 0 = off
 * @return ESP_OK on success
 */
esp_err_t photo_quality_set_timer_enabled(uint8_t enabled) {
    photo_config.timer_enabled = (enabled != 0) ? 1 : 0;
    return ESP_OK;
}

/**
 * @brief Get timer enabled status
 * 
 * @return 1 if timer enabled, 0 if disabled
 */
uint8_t photo_quality_get_timer_enabled(void) {
    return photo_config.timer_enabled;
}

/**
 * @brief Set timer duration
 * 
 * @param seconds Timer duration in seconds (1-30)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid duration
 */
esp_err_t photo_quality_set_timer_seconds(uint8_t seconds) {
    if (seconds < 1 || seconds > 30) {
        return ESP_ERR_INVALID_ARG;
    }
    photo_config.timer_seconds = seconds;
    return ESP_OK;
}

/**
 * @brief Get timer duration
 * 
 * @return Timer duration in seconds
 */
uint32_t photo_quality_get_timer_seconds(void) {
    return photo_config.timer_seconds;
}

/**
 * @brief Enable/disable burst mode
 * 
 * @param enabled 1 = burst mode on, 0 = off
 * @return ESP_OK on success
 */
esp_err_t photo_quality_set_burst_mode(uint8_t enabled) {
    photo_config.burst_mode = (enabled != 0) ? 1 : 0;
    return ESP_OK;
}

/**
 * @brief Get burst mode status
 * 
 * @return 1 if burst mode enabled, 0 if disabled
 */
uint8_t photo_quality_get_burst_mode(void) {
    return photo_config.burst_mode;
}

/**
 * @brief Set burst mode count
 * 
 * @param count Number of photos to take in burst (2-10)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid count
 */
esp_err_t photo_quality_set_burst_count(uint8_t count) {
    if (count < 2 || count > 10) {
        return ESP_ERR_INVALID_ARG;
    }
    photo_config.burst_mode = count;
    return ESP_OK;
}

/**
 * @brief Get burst mode count
 * 
 * @return Number of photos in burst mode (0 if not in burst mode)
 */
uint8_t photo_quality_get_burst_count(void) {
    return (photo_config.burst_mode > 1) ? photo_config.burst_mode : 0;
}

/**
 * @brief Enable/disable HDR mode
 * 
 * @param enabled 1 = HDR on, 0 = off
 * @return ESP_OK on success
 */
esp_err_t photo_quality_set_hdr_mode(uint8_t enabled) {
    photo_config.hdr_mode = (enabled != 0) ? 1 : 0;
    return ESP_OK;
}

/**
 * @brief Get HDR mode status
 * 
 * @return 1 if HDR enabled, 0 if disabled
 */
uint8_t photo_quality_get_hdr_mode(void) {
    return photo_config.hdr_mode;
}

/**
 * @brief Get all photo quality settings as a structure
 * 
 * @param settings Pointer to settings structure to fill
 * @return ESP_OK on success
 */
esp_err_t photo_quality_get_all_settings(photo_quality_settings_t *settings) {
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    settings->jpeg_quality = photo_config.jpeg_quality;
    settings->auto_exposure = photo_config.auto_exposure;
    settings->auto_white_balance = photo_config.auto_white_balance;
    settings->brightness = (int8_t)(photo_config.brightness - 5);
    settings->contrast = (int8_t)(photo_config.contrast - 5);
    settings->saturation = (int8_t)(photo_config.saturation - 5);
    settings->sharpness = (int8_t)(photo_config.sharpness - 5);
    settings->noise_reduction = photo_config.noise_reduction;
    settings->face_detection = photo_config.face_detection;
    settings->timer_enabled = photo_config.timer_enabled;
    settings->timer_seconds = photo_config.timer_seconds;
    settings->burst_mode = (photo_config.burst_mode > 1) ? photo_config.burst_mode : 0;
    settings->hdr_mode = photo_config.hdr_mode;
    
    return ESP_OK;
}

/**
 * @brief Reset all settings to defaults
 * 
 * @return ESP_OK on success
 */
esp_err_t photo_quality_reset_to_defaults(void) {
    return photo_quality_init();
}

/**
 * @brief Apply quality settings to camera driver
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t photo_quality_apply_to_camera(void) {
    // This function would interface with the actual camera driver
    // For now, it just validates the settings
    if (photo_config.jpeg_quality > 100 || photo_config.noise_reduction > 10) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
