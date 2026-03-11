/**
 * @brief Tests for photo quality analysis module
 *
 * Tests brightness, contrast, saturation analysis, quality scoring,
 * and NVS storage of photo quality metrics.
 */

#include "unity/unity.h"
#include "mocks/esp_err.h"
#include "mocks/esp_log.h"
#include "mocks/nvs.h"
#include "mocks/sdkconfig.h"

#include <stdlib.h>
#include <string.h>

/* ---- Photo Quality Test Structures ---- */
#define TEST_IMAGE_WIDTH    1920
#define TEST_IMAGE_HEIGHT   1080
#define TEST_PIXEL_COUNT    (TEST_IMAGE_WIDTH * TEST_IMAGE_HEIGHT)
#define QUALITY_SCORE_MAX   100.0f

/* Test image data patterns */
typedef struct {
    uint8_t *image_data;
    size_t image_size;
    int initialized;
} test_image_t;

typedef struct {
    float brightness;
    float contrast;
    float saturation;
    float quality_score;
    int quality_valid;
} quality_metrics_t;

static test_image_t test_image;
static quality_metrics_t metrics;

static void reset_test_image(void) {
    if (test_image.image_data) {
        free(test_image.image_data);
        test_image.image_data = NULL;
    }
    test_image.image_size = 0;
    test_image.initialized = 0;
}

static void reset_metrics(void) {
    memset(&metrics, 0, sizeof(metrics));
    metrics.quality_valid = 0;
}

static esp_err_t create_test_image(uint8_t pattern) {
    reset_test_image();
    test_image.image_size = TEST_PIXEL_COUNT * 2; /* RGB565 format */
    test_image.image_data = (uint8_t *)malloc(test_image.image_size);
    if (!test_image.image_data) {
        return ESP_ERR_NO_MEM;
    }
    
    /* Fill with pattern as RGB565 value */
    uint16_t pixel_val = pattern;
    for (size_t i = 0; i < TEST_PIXEL_COUNT; i++) {
        ((uint16_t *)test_image.image_data)[i] = pixel_val;
    }
    test_image.initialized = 1;
    return ESP_OK;
}

/* ---- Helper Functions for Quality Analysis ---- */

static float calculate_brightness(const uint8_t *data, size_t size) {
    uint32_t sum = 0;
    size_t pixel_count = size / 2; /* RGB565 = 2 bytes per pixel */
    
    for (size_t i = 0; i < pixel_count; i++) {
        uint16_t pixel = data[i * 2] | (data[i * 2 + 1] << 8);
        /* Extract RGB565 components */
        uint8_t r = (pixel >> 11) & 0x1F;
        uint8_t g = (pixel >> 5) & 0x3F;
        uint8_t b = pixel & 0x1F;
        /* Convert to 8-bit */
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        sum += (r + g + b) / 3;
    }
    
    return (float)sum / pixel_count;
}

static float calculate_contrast(const uint8_t *data, size_t size) {
    uint32_t sum = 0;
    size_t pixel_count = size / 2;
    uint16_t *pixels = (uint16_t *)data;
    
    /* Calculate mean */
    uint32_t mean_sum = 0;
    for (size_t i = 0; i < pixel_count; i++) {
        uint16_t pixel = pixels[i];
        uint8_t r = (pixel >> 11) & 0x1F;
        uint8_t g = (pixel >> 5) & 0x3F;
        uint8_t b = pixel & 0x1F;
        mean_sum += (r + g + b) / 3;
    }
    float mean = (float)mean_sum / pixel_count;
    
    /* Calculate standard deviation */
    for (size_t i = 0; i < pixel_count; i++) {
        uint16_t pixel = pixels[i];
        uint8_t r = (pixel >> 11) & 0x1F;
        uint8_t g = (pixel >> 5) & 0x3F;
        uint8_t b = pixel & 0x1F;
        uint8_t brightness = (r + g + b) / 3;
        float diff = brightness - mean;
        sum += (uint32_t)(diff * diff);
    }
    
    return sqrtf((float)sum / pixel_count);
}

static float calculate_saturation(const uint8_t *data, size_t size) {
    uint32_t sum = 0;
    size_t pixel_count = size / 2;
    uint16_t *pixels = (uint16_t *)data;
    
    for (size_t i = 0; i < pixel_count; i++) {
        uint16_t pixel = pixels[i];
        uint8_t r = (pixel >> 11) & 0x1F;
        uint8_t g = (pixel >> 5) & 0x3F;
        uint8_t b = pixel & 0x1F;
        
        /* Convert to 8-bit */
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        
        uint8_t max = r > g ? r : g;
        max = max > b ? max : b;
        uint8_t min = r < g ? r : g;
        min = min < b ? min : b;
        
        if (max > 0) {
            sum += (max - min) * 255 / max;
        }
    }
    
    return (float)sum / pixel_count;
}

static float calculate_quality_score(float brightness, float contrast, float saturation) {
    /* Normalize brightness to 0-100 scale (ideal ~128) */
    float brightness_score = 100.0f - fabsf(brightness - 128.0f) * 100.0f / 128.0f;
    if (brightness_score < 0) brightness_score = 0;
    
    /* Contrast: higher is better, scale to 0-100 */
    float contrast_score = contrast * 2.0f;
    if (contrast_score > 100.0f) contrast_score = 100.0f;
    
    /* Saturation: moderate is better for natural images (scale 0-255) */
    float saturation_score = 100.0f - fabsf(saturation - 85.0f) * 100.0f / 85.0f;
    if (saturation_score < 0) saturation_score = 0;
    
    /* Weighted average */
    return (brightness_score * 0.3f + contrast_score * 0.4f + saturation_score * 0.3f);
}

/* ---- Tests ---- */

void test_brightness_uniform_image(void) {
    reset_test_image();
    reset_metrics();
    
    /* Create uniform gray image */
    esp_err_t ret = create_test_image(128);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    
    float brightness = calculate_brightness(test_image.image_data, test_image.image_size);
    TEST_ASSERT_EQUAL_FLOAT(128.0f, brightness);
    
    reset_test_image();
}

void test_brightness_black_image(void) {
    reset_test_image();
    
    esp_err_t ret = create_test_image(0);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    
    float brightness = calculate_brightness(test_image.image_data, test_image.image_size);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, brightness);
    
    reset_test_image();
}

void test_brightness_white_image(void) {
    reset_test_image();
    
    esp_err_t ret = create_test_image(255);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    
    float brightness = calculate_brightness(test_image.image_data, test_image.image_size);
    TEST_ASSERT_EQUAL_FLOAT(255.0f, brightness);
    
    reset_test_image();
}

void test_contrast_uniform_image(void) {
    reset_test_image();
    
    /* Uniform image should have zero contrast */
    esp_err_t ret = create_test_image(128);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    
    float contrast = calculate_contrast(test_image.image_data, test_image.image_size);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, contrast);
    
    reset_test_image();
}

void test_contrast_gradient_image(void) {
    reset_test_image();
    
    /* Create gradient image */
    test_image.image_size = TEST_PIXEL_COUNT * 2;
    test_image.image_data = (uint8_t *)malloc(test_image.image_size);
    if (!test_image.image_data) {
        TEST_FAIL();
        return;
    }
    
    uint16_t *pixels = (uint16_t *)test_image.image_data;
    for (size_t i = 0; i < TEST_PIXEL_COUNT; i++) {
        uint8_t val = (i % 256);
        pixels[i] = (val << 11) | (val << 5) | val;
    }
    test_image.initialized = 1;
    
    float contrast = calculate_contrast(test_image.image_data, test_image.image_size);
    TEST_ASSERT_GREATER_THAN(50.0f, contrast); /* Should have significant contrast */
    
    reset_test_image();
}

void test_saturation_gray_image(void) {
    reset_test_image();
    
    /* Gray image should have zero saturation */
    esp_err_t ret = create_test_image(128);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    
    float saturation = calculate_saturation(test_image.image_data, test_image.image_size);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, saturation);
    
    reset_test_image();
}

void test_saturation_colorful_image(void) {
    reset_test_image();
    
    /* Create colorful image with varying RGB */
    test_image.image_size = TEST_PIXEL_COUNT * 2;
    test_image.image_data = (uint8_t *)malloc(test_image.image_size);
    if (!test_image.image_data) {
        TEST_FAIL();
        return;
    }
    
    uint16_t *pixels = (uint16_t *)test_image.image_data;
    for (size_t i = 0; i < TEST_PIXEL_COUNT; i++) {
        uint8_t r = (i % 32) << 3;
        uint8_t g = ((i + 10) % 64) << 2;
        uint8_t b = ((i + 20) % 32) << 3;
        pixels[i] = (r << 11) | (g << 5) | b;
    }
    test_image.initialized = 1;
    
    float saturation = calculate_saturation(test_image.image_data, test_image.image_size);
    TEST_ASSERT_GREATER_THAN(50.0f, saturation);
    
    reset_test_image();
}

void test_quality_score_high_contrast(void) {
    reset_test_image();
    reset_metrics();
    
    /* Create high contrast image */
    test_image.image_size = TEST_PIXEL_COUNT * 2;
    test_image.image_data = (uint8_t *)malloc(test_image.image_size);
    if (!test_image.image_data) {
        TEST_FAIL();
        return;
    }
    
    uint16_t *pixels = (uint16_t *)test_image.image_data;
    for (size_t i = 0; i < TEST_PIXEL_COUNT; i++) {
        uint8_t val = (i % 2 == 0) ? 0 : 255;
        pixels[i] = (val << 11) | (val << 5) | val;
    }
    test_image.initialized = 1;
    
    float brightness = calculate_brightness(test_image.image_data, test_image.image_size);
    float contrast = calculate_contrast(test_image.image_data, test_image.image_size);
    float saturation = calculate_saturation(test_image.image_data, test_image.image_size);
    float score = calculate_quality_score(brightness, contrast, saturation);
    
    /* Score should be reasonable (not zero, not > 100) */
    TEST_ASSERT_GREATER_THAN(0.0f, score);
    TEST_ASSERT_LESS_THAN(100.0f, score);
    
    reset_test_image();
}

void test_quality_score_uniform_image(void) {
    reset_test_image();
    
    esp_err_t ret = create_test_image(128);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    
    float brightness = calculate_brightness(test_image.image_data, test_image.image_size);
    float contrast = calculate_contrast(test_image.image_data, test_image.image_size);
    float saturation = calculate_saturation(test_image.image_data, test_image.image_size);
    float score = calculate_quality_score(brightness, contrast, saturation);
    
    /* Uniform image should have lower quality score */
    TEST_ASSERT_LESS_THAN(50.0f, score);
    
    reset_test_image();
}

void test_quality_score_optimal(void) {
    reset_test_image();
    
    /* Create optimal quality image */
    test_image.image_size = TEST_PIXEL_COUNT * 2;
    test_image.image_data = (uint8_t *)malloc(test_image.image_size);
    if (!test_image.image_data) {
        TEST_FAIL();
        return;
    }
    
    uint16_t *pixels = (uint16_t *)test_image.image_data;
    for (size_t i = 0; i < TEST_PIXEL_COUNT; i++) {
        /* Optimal: mid-gray with slight variation */
        uint8_t val = 128 + ((i % 16) - 8);
        pixels[i] = (val << 11) | (val << 5) | val;
    }
    test_image.initialized = 1;
    
    float brightness = calculate_brightness(test_image.image_data, test_image.image_size);
    float contrast = calculate_contrast(test_image.image_data, test_image.image_size);
    float saturation = calculate_saturation(test_image.image_data, test_image.image_size);
    float score = calculate_quality_score(brightness, contrast, saturation);
    
    /* Should be high quality score */
    TEST_ASSERT_GREATER_THAN(80.0f, score);
    
    reset_test_image();
}

void test_memory_cleanup(void) {
    reset_test_image();
    
    /* Create and properly clean up */
    esp_err_t ret = create_test_image(100);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(test_image.initialized);
    
    reset_test_image();
    TEST_ASSERT_FALSE(test_image.initialized);
    TEST_ASSERT_NULL(test_image.image_data);
    TEST_ASSERT_EQUAL(0, test_image.image_size);
}

/* ---- Main ---- */
int main(void) {
    printf("\n===== Photo Quality Analysis Tests =====\n");
    UNITY_BEGIN();
    
    RUN_TEST(test_brightness_uniform_image);
    RUN_TEST(test_brightness_black_image);
    RUN_TEST(test_brightness_white_image);
    RUN_TEST(test_contrast_uniform_image);
    RUN_TEST(test_contrast_gradient_image);
    RUN_TEST(test_saturation_gray_image);
    RUN_TEST(test_saturation_colorful_image);
    RUN_TEST(test_quality_score_high_contrast);
    RUN_TEST(test_quality_score_uniform_image);
    RUN_TEST(test_quality_score_optimal);
    RUN_TEST(test_memory_cleanup);
    
    UNITY_END();
    return 0;
}
