#include "app_video_photo_quality.h"
#include "esp_log.h"
#include "app_storage.h"
#include "driver/ppa.h"
#include <math.h>
#include <string.h>

static const char *TAG = "photo_quality";

// Internal state for quality analysis
static photo_quality_metrics_t last_metrics = {0};
static bool metrics_available = false;
static bool initialized = false;

// Quality thresholds for suggestions
#define BRIGHTNESS_LOW_THRESHOLD      60
#define BRIGHTNESS_HIGH_THRESHOLD     200
#define CONTRAST_LOW_THRESHOLD        100
#define SATURATION_LOW_THRESHOLD      50
#define SHARPNESS_LOW_THRESHOLD       200
#define NOISE_HIGH_THRESHOLD          30
#define EXPOSURE_POOR_THRESHOLD       40
#define COLOR_IMBALANCE_THRESHOLD     150

/**
 * @brief Calculate mean of an array
 */
static uint32_t calculate_mean(const uint32_t *values, size_t count) {
    if (count == 0) return 0;
    
    uint64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += values[i];
    }
    return (uint32_t)(sum / count);
}

/**
 * @brief Calculate standard deviation of an array
 */
static uint32_t calculate_std(const uint32_t *values, size_t count, uint32_t mean) {
    if (count < 2) return 0;
    
    uint64_t sum_sq_diff = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t diff = (int32_t)values[i] - (int32_t)mean;
        sum_sq_diff += (uint64_t)diff * diff;
    }
    
    return (uint32_t)sqrt((double)sum_sq_diff / count);
}

/**
 * @brief Calculate contrast score from brightness values
 * 
 * Contrast is measured as the standard deviation of brightness values.
 * Higher std dev = higher contrast.
 */
static uint32_t calculate_contrast(const uint32_t *brightness_values, size_t count) {
    uint32_t mean = calculate_mean(brightness_values, count);
    uint32_t std = calculate_std(brightness_values, count, mean);
    
    // Scale to 0-1000 range
    return (std * 1000) / 255;
}

/**
 * @brief Calculate sharpness using edge detection
 * 
 * Uses Sobel-like edge detection on the Y (luminance) plane.
 * Higher edge density = higher sharpness.
 */
static uint32_t calculate_sharpness(const uint8_t *y_plane, uint32_t width, uint32_t height) {
    if (width < 2 || height < 2) return 0;
    
    int32_t edge_count = 0;
    int32_t total_edges = 0;
    
    // Simple horizontal edge detection
    for (uint32_t y = 1; y < height - 1; y++) {
        for (uint32_t x = 1; x < width - 1; x++) {
            int32_t left = y_plane[y * width + x - 1];
            int32_t center = y_plane[y * width + x];
            int32_t right = y_plane[y * width + x + 1];
            
            int32_t diff = (left > right) ? (left - right) : (right - left);
            if (diff > 10) {
                edge_count++;
            }
            total_edges++;
        }
    }
    
    // Scale to 0-1000 range based on edge density
    uint32_t sharpness = (total_edges > 0) ? (edge_count * 1000 / total_edges) : 0;
    return sharpness;
}

/**
 * @brief Calculate saturation from UV planes
 * 
 * Saturation is the magnitude of chrominance (UV) components.
 */
static uint32_t calculate_saturation(const uint8_t *u_plane, const uint8_t *v_plane,
                                     uint32_t width, uint32_t height) {
    uint64_t sum_saturation = 0;
    uint32_t sample_count = 0;
    
    // Sample every 4th pixel to reduce computation
    for (uint32_t y = 0; y < height; y += 2) {
        for (uint32_t x = 0; x < width; x += 2) {
            uint8_t u = u_plane[y * (width / 2) + x / 2];
            uint8_t v = v_plane[y * (width / 2) + x / 2];
            
            // Calculate saturation as magnitude from neutral (128, 128)
            int32_t u_diff = (int32_t)u - 128;
            int32_t v_diff = (int32_t)v - 128;
            uint32_t saturation = (uint32_t)sqrt((double)u_diff * u_diff + v_diff * v_diff);
            
            sum_saturation += saturation;
            sample_count++;
        }
    }
    
    return (sample_count > 0) ? (uint32_t)(sum_saturation / sample_count) : 0;
}

/**
 * @brief Estimate noise from UV plane variance
 * 
 * Noise manifests as random variation in chrominance values.
 */
static uint32_t estimate_noise(const uint8_t *u_plane, const uint8_t *v_plane,
                               uint32_t width, uint32_t height) {
    uint32_t sample_count = 0;
    uint64_t sum_u = 0, sum_v = 0;
    uint64_t sum_u_sq = 0, sum_v_sq = 0;
    
    for (uint32_t y = 0; y < height; y += 4) {
        for (uint32_t x = 0; x < width; x += 4) {
            uint8_t u = u_plane[y * (width / 2) + x / 2];
            uint8_t v = v_plane[y * (width / 2) + x / 2];
            
            sum_u += u;
            sum_v += v;
            sum_u_sq += u * u;
            sum_v_sq += v * v;
            sample_count++;
        }
    }
    
    if (sample_count == 0) return 0;
    
    double mean_u = (double)sum_u / sample_count;
    double mean_v = (double)sum_v / sample_count;
    
    double var_u = ((double)sum_u_sq / sample_count) - (mean_u * mean_u);
    double var_v = ((double)sum_v_sq / sample_count) - (mean_v * mean_v);
    
    // Noise estimate as combined variance
    uint32_t noise = (uint32_t)sqrt(var_u + var_v) * 10;
    return (noise > 255) ? 255 : noise;
}

/**
 * @brief Calculate exposure quality
 * 
 * Optimal exposure is around 128 (mid-gray). Deviations indicate under/over exposure.
 */
static uint32_t calculate_exposure_quality(const uint32_t *brightness_values, size_t count) {
    uint32_t mean = calculate_mean(brightness_values, count);
    
    // Optimal brightness is 128, calculate deviation
    int32_t deviation = (int32_t)mean - 128;
    if (deviation < 0) deviation = -deviation;
    
    // Scale: 0 deviation = 100 quality, 128 deviation = 0 quality
    uint32_t quality = 100 - ((deviation * 100) / 128);
    return (quality > 100) ? 100 : quality;
}

/**
 * @brief Calculate color balance score
 * 
 * Measures how balanced the RGB components are.
 */
static uint32_t calculate_color_balance(const uint8_t *y_plane, const uint8_t *u_plane,
                                        const uint8_t *v_plane,
                                        uint32_t width, uint32_t height) {
    uint64_t sum_y = 0, sum_u = 0, sum_v = 0;
    uint32_t sample_count = 0;
    
    for (uint32_t y = 0; y < height; y += 4) {
        for (uint32_t x = 0; x < width; x += 4) {
            sum_y += y_plane[y * width + x];
            sum_u += u_plane[y * (width / 2) + x / 2];
            sum_v += v_plane[y * (width / 2) + x / 2];
            sample_count++;
        }
    }
    
    if (sample_count == 0) return 1000; // Perfect balance if no data
    
    double mean_y = (double)sum_y / sample_count;
    double mean_u = (double)sum_u / sample_count;
    double mean_v = (double)sum_v / sample_count;
    
    // Neutral chrominance is (128, 128)
    int32_t u_deviation = (int32_t)mean_u - 128;
    int32_t v_deviation = (int32_t)mean_v - 128;
    
    // Calculate imbalance score (lower is better)
    uint32_t imbalance = (uint32_t)sqrt((double)u_deviation * u_deviation + 
                                        (double)v_deviation * v_deviation);
    
    // Scale to 0-1000 (1000 = perfect balance, 0 = very imbalanced)
    uint32_t balance_score = 1000 - (imbalance * 1000 / 128);
    return (balance_score > 1000) ? 1000 : balance_score;
}

/**
 * @brief Calculate overall quality score
 * 
 * Weighted combination of all quality metrics.
 */
static uint32_t calculate_overall_quality(const photo_quality_metrics_t *metrics) {
    // Weights: brightness(10%), contrast(15%), saturation(10%), sharpness(20%),
    //          noise(15%), exposure(15%), color_balance(15%)
    
    uint32_t brightness_score = (metrics->brightness_mean < 255) ? 
                                (metrics->brightness_mean * 1000 / 255) : 1000;
    
    uint32_t contrast_score = metrics->contrast_score;
    uint32_t saturation_score = (metrics->saturation_avg < 255) ?
                                (metrics->saturation_avg * 1000 / 255) : 1000;
    uint32_t sharpness_score = metrics->sharpness_score;
    uint32_t noise_score = (metrics->noise_estimate < 255) ?
                           (255 - metrics->noise_estimate) * 1000 / 255 : 0;
    uint32_t exposure_score = metrics->exposure_quality;
    uint32_t color_score = metrics->color_balance_score;
    
    uint32_t overall = (brightness_score * 10 + contrast_score * 15 + 
                        saturation_score * 10 + sharpness_score * 20 +
                        noise_score * 15 + exposure_score * 15 + 
                        color_score * 15) / 100;
    
    return overall;
}

esp_err_t app_video_photo_quality_init(void) {
    if (initialized) {
        ESP_LOGI(TAG, "Photo quality module already initialized");
        return ESP_OK;
    }
    
    memset(&last_metrics, 0, sizeof(last_metrics));
    metrics_available = false;
    initialized = true;
    
    ESP_LOGI(TAG, "Photo quality module initialized");
    return ESP_OK;
}

esp_err_t app_video_photo_quality_analyze(const uint8_t *frame_buffer,
                                          uint32_t width,
                                          uint32_t height,
                                          photo_quality_metrics_t *metrics) {
    if (!initialized) {
        ESP_LOGE(TAG, "Photo quality module not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (frame_buffer == NULL || metrics == NULL) {
        ESP_LOGE(TAG, "Invalid input parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (width == 0 || height == 0) {
        ESP_LOGE(TAG, "Invalid frame dimensions");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Extract Y, U, V planes (NV12 format)
    const uint8_t *y_plane = frame_buffer;
    const uint8_t *uv_plane = frame_buffer + (width * height);
    const uint8_t *u_plane = uv_plane;
    const uint8_t *v_plane = uv_plane + (width * height / 4);
    
    // Calculate brightness statistics
    uint32_t *brightness_samples = NULL;
    uint32_t sample_count = 0;
    
    // Sample Y plane for brightness analysis
    uint32_t stride = (width > 64) ? (width / 64) : 1;
    uint32_t height_stride = (height > 64) ? (height / 64) : 1;
    
    brightness_samples = (uint32_t *)malloc(width * height / (stride * height_stride));
    if (brightness_samples == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for brightness samples");
        return ESP_ERR_NO_MEM;
    }
    
    for (uint32_t y = 0; y < height; y += height_stride) {
        for (uint32_t x = 0; x < width; x += stride) {
            brightness_samples[sample_count++] = y_plane[y * width + x];
        }
    }
    
    // Calculate metrics
    metrics->brightness_mean = calculate_mean(brightness_samples, sample_count);
    metrics->brightness_std = calculate_std(brightness_samples, sample_count, metrics->brightness_mean);
    metrics->contrast_score = calculate_contrast(brightness_samples, sample_count);
    metrics->sharpness_score = calculate_sharpness(y_plane, width, height);
    metrics->saturation_avg = calculate_saturation(u_plane, v_plane, width, height);
    metrics->noise_estimate = estimate_noise(u_plane, v_plane, width, height);
    metrics->exposure_quality = calculate_exposure_quality(brightness_samples, sample_count);
    metrics->color_balance_score = calculate_color_balance(y_plane, u_plane, v_plane, width, height);
    metrics->overall_quality = calculate_overall_quality(metrics);
    
    free(brightness_samples);
    
    // Store as last metrics
    memcpy(&last_metrics, metrics, sizeof(photo_quality_metrics_t));
    metrics_available = true;
    
    ESP_LOGI(TAG, "Quality analysis complete: overall=%u, sharpness=%u, noise=%u",
             metrics->overall_quality, metrics->sharpness_score, metrics->noise_estimate);
    
    return ESP_OK;
}

esp_err_t app_video_photo_quality_save_metrics(const photo_quality_metrics_t *metrics) {
    if (!initialized) {
        ESP_LOGE(TAG, "Photo quality module not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (metrics == NULL) {
        ESP_LOGE(TAG, "Invalid metrics parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = app_storage_save_quality_metrics(metrics);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save metrics: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Quality metrics saved to NVS");
    return ESP_OK;
}

esp_err_t app_video_photo_quality_load_metrics(photo_quality_metrics_t *metrics) {
    if (!initialized) {
        ESP_LOGE(TAG, "Photo quality module not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (metrics == NULL) {
        ESP_LOGE(TAG, "Invalid metrics parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = app_storage_load_quality_metrics(metrics);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load metrics: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Quality metrics loaded from NVS");
    memcpy(&last_metrics, metrics, sizeof(photo_quality_metrics_t));
    metrics_available = true;
    
    return ESP_OK;
}

esp_err_t app_video_photo_quality_get_last_metrics(photo_quality_metrics_t *metrics) {
    if (!initialized) {
        ESP_LOGE(TAG, "Photo quality module not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (metrics == NULL) {
        ESP_LOGE(TAG, "Invalid metrics parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!metrics_available) {
        ESP_LOGW(TAG, "No metrics available yet");
        return ESP_ERR_NOT_FOUND;
    }
    
    memcpy(metrics, &last_metrics, sizeof(photo_quality_metrics_t));
    return ESP_OK;
}

int app_video_photo_quality_get_suggestions(const photo_quality_metrics_t *metrics,
                                           uint32_t *suggestions) {
    if (!initialized || metrics == NULL || suggestions == NULL) {
        return 0;
    }
    
    int count = 0;
    
    // Check brightness
    if (metrics->brightness_mean < BRIGHTNESS_LOW_THRESHOLD) {
        *suggestions |= QUALITY_SUGGESTION_LOW_BRIGHTNESS;
        count++;
    } else if (metrics->brightness_mean > BRIGHTNESS_HIGH_THRESHOLD) {
        *suggestions |= QUALITY_SUGGESTION_POOR_EXPOSURE;
        count++;
    }
    
    // Check contrast
    if (metrics->contrast_score < CONTRAST_LOW_THRESHOLD) {
        *suggestions |= QUALITY_SUGGESTION_LOW_CONTRAST;
        count++;
    }
    
    // Check sharpness
    if (metrics->sharpness_score < SHARPNESS_LOW_THRESHOLD) {
        *suggestions |= QUALITY_SUGGESTION_LOW_SHARPNESS;
        count++;
    }
    
    // Check noise
    if (metrics->noise_estimate > NOISE_HIGH_THRESHOLD) {
        *suggestions |= QUALITY_SUGGESTION_HIGH_NOISE;
        count++;
    }
    
    // Check exposure
    if (metrics->exposure_quality < EXPOSURE_POOR_THRESHOLD) {
        *suggestions |= QUALITY_SUGGESTION_POOR_EXPOSURE;
        count++;
    }
    
    // Check color balance
    if (metrics->color_balance_score < (1000 - COLOR_IMBALANCE_THRESHOLD)) {
        *suggestions |= QUALITY_SUGGESTION_COLOR_IMBALANCE;
        count++;
    }
    
    return count;
}
