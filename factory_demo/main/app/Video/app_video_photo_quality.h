#ifndef APP_VIDEO_PHOTO_QUALITY_H
#define APP_VIDEO_PHOTO_QUALITY_H

#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Photo quality metrics structure
 * 
 * Contains computed quality metrics for a photo frame.
 */
typedef struct {
    uint32_t brightness_mean;      // Average brightness (0-255)
    uint32_t brightness_std;       // Brightness standard deviation
    uint32_t contrast_score;       // Contrast score (0-1000)
    uint32_t saturation_avg;       // Average saturation (0-255)
    uint32_t sharpness_score;      // Sharpness metric (0-1000)
    uint32_t noise_estimate;       // Noise level estimate
    uint32_t exposure_quality;     // Exposure quality (0-100)
    uint32_t color_balance_score;  // Color balance score (0-1000)
    uint32_t overall_quality;      // Combined quality score (0-1000)
} photo_quality_metrics_t;

/**
 * @brief Initialize the photo quality analysis module
 * 
 * Sets up internal buffers and prepares for quality analysis.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_video_photo_quality_init(void);

/**
 * @brief Analyze photo quality from a frame buffer
 * 
 * Performs comprehensive quality analysis on a YUV420 frame buffer.
 * Analyzes brightness, contrast, saturation, sharpness, and noise.
 * 
 * @param frame_buffer YUV420 frame buffer (NV12 format)
 * @param width Frame width in pixels
 * @param height Frame height in pixels
 * @param metrics Output structure for quality metrics
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_video_photo_quality_analyze(const uint8_t *frame_buffer,
                                          uint32_t width,
                                          uint32_t height,
                                          photo_quality_metrics_t *metrics);

/**
 * @brief Save quality metrics to NVS storage
 * 
 * Stores quality metrics persistently for later retrieval.
 * 
 * @param metrics Quality metrics to save
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_video_photo_quality_save_metrics(const photo_quality_metrics_t *metrics);

/**
 * @brief Load quality metrics from NVS storage
 * 
 * Retrieves previously saved quality metrics.
 * 
 * @param metrics Output structure for loaded metrics
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_video_photo_quality_load_metrics(photo_quality_metrics_t *metrics);

/**
 * @brief Get the last analyzed quality metrics
 * 
 * Returns the most recently analyzed quality metrics without reloading from NVS.
 * 
 * @param metrics Output structure for metrics
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_video_photo_quality_get_last_metrics(photo_quality_metrics_t *metrics);

/**
 * @brief Calculate quality improvement suggestion
 * 
 * Analyzes metrics and returns suggestions for improving photo quality.
 * 
 * @param metrics Current quality metrics
 * @param suggestions Output array of suggestion flags
 * @return Number of suggestions returned
 */
int app_video_photo_quality_get_suggestions(const photo_quality_metrics_t *metrics,
                                           uint32_t *suggestions);

/**
 * @brief Quality suggestion flags
 */
#define QUALITY_SUGGESTION_LOW_BRIGHTNESS   (1 << 0)
#define QUALITY_SUGGESTION_LOW_CONTRAST     (1 << 1)
#define QUALITY_SUGGESTION_LOW_SHARPNESS    (1 << 2)
#define QUALITY_SUGGESTION_HIGH_NOISE       (1 << 3)
#define QUALITY_SUGGESTION_POOR_EXPOSURE    (1 << 4)
#define QUALITY_SUGGESTION_COLOR_IMBALANCE  (1 << 5)

#endif /* APP_VIDEO_PHOTO_QUALITY_H */
