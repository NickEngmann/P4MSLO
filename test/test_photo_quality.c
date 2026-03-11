/**
 * @brief Tests for Photo Quality settings (app_video_photo_quality.c)
 *
 * Tests photo quality configuration, JPEG compression levels, and quality presets.
 */

#include "unity/unity.h"
#include "app_video_photo_quality.h"
#include "mocks/nvs.h"
#include "mocks/esp_log.h"

/* Test fixtures */
static photo_quality_config_t test_config;
static uint8_t test_jpeg_quality;
static int8_t test_sharpness;
static int8_t test_contrast;
static int8_t test_brightness;
static int8_t test_saturation;

void setUp(void) {
    /* Reset all test variables */
    test_config.jpeg_quality = 0;
    test_config.sharpness = 0;
    test_config.contrast = 0;
    test_config.brightness = 0;
    test_config.saturation = 0;
    test_jpeg_quality = 0;
    test_sharpness = 0;
    test_contrast = 0;
    test_brightness = 0;
    test_saturation = 0;
    
    /* Reset NVS mock */
    nvs_reset();
}

void tearDown(void) {
    /* Nothing to clean up */
}

/* ---- Test: Module initialization ---- */
void test_photo_quality_init_success(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_init());
}

void test_photo_quality_shutdown_success(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_init());
    TEST_ASSERT_EQUAL_INT(0, photo_quality_shutdown());
}

/* ---- Test: Quality level management ---- */
void test_quality_level_default_is_medium(void) {
    TEST_ASSERT_EQUAL_INT(QUALITY_MEDIUM, photo_quality_get_level());
}

void test_quality_set_low(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_level(QUALITY_LOW));
    TEST_ASSERT_EQUAL_INT(QUALITY_LOW, photo_quality_get_level());
}

void test_quality_set_medium(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_level(QUALITY_MEDIUM));
    TEST_ASSERT_EQUAL_INT(QUALITY_MEDIUM, photo_quality_get_level());
}

void test_quality_set_high(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_level(QUALITY_HIGH));
    TEST_ASSERT_EQUAL_INT(QUALITY_HIGH, photo_quality_get_level());
}

void test_quality_invalid_level_returns_error(void) {
    TEST_ASSERT_EQUAL_INT(-22, photo_quality_set_level((photo_quality_level_t)99));
}

/* ---- Test: JPEG quality settings ---- */
void test_jpeg_quality_default_is_85(void) {
    TEST_ASSERT_EQUAL_UINT8(85, photo_quality_get_jpeg_quality());
}

void test_jpeg_quality_set_50(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_jpeg_quality(50));
    TEST_ASSERT_EQUAL_UINT8(50, photo_quality_get_jpeg_quality());
}

void test_jpeg_quality_set_100(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_jpeg_quality(100));
    TEST_ASSERT_EQUAL_UINT8(100, photo_quality_get_jpeg_quality());
}

void test_jpeg_quality_set_1(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_jpeg_quality(1));
    TEST_ASSERT_EQUAL_UINT8(1, photo_quality_get_jpeg_quality());
}

void test_jpeg_quality_set_0_returns_error(void) {
    TEST_ASSERT_EQUAL_INT(-22, photo_quality_set_jpeg_quality(0));
}

void test_jpeg_quality_set_101_returns_error(void) {
    TEST_ASSERT_EQUAL_INT(-22, photo_quality_set_jpeg_quality(101));
}

/* ---- Test: Sharpness settings ---- */
void test_sharpness_default_is_0(void) {
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_sharpness());
}

void test_sharpness_set_5(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_sharpness(5));
    TEST_ASSERT_EQUAL_INT8(5, photo_quality_get_sharpness());
}

void test_sharpness_set_minus_5(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_sharpness(-5));
    TEST_ASSERT_EQUAL_INT8(-5, photo_quality_get_sharpness());
}

void test_sharpness_set_3(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_sharpness(3));
    TEST_ASSERT_EQUAL_INT8(3, photo_quality_get_sharpness());
}

void test_sharpness_set_6_returns_error(void) {
    TEST_ASSERT_EQUAL_INT(-22, photo_quality_set_sharpness(6));
}

void test_sharpness_set_minus_6_returns_error(void) {
    TEST_ASSERT_EQUAL_INT(-22, photo_quality_set_sharpness(-6));
}

/* ---- Test: Contrast settings ---- */
void test_contrast_default_is_0(void) {
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_contrast());
}

void test_contrast_set_4(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_contrast(4));
    TEST_ASSERT_EQUAL_INT8(4, photo_quality_get_contrast());
}

void test_contrast_set_minus_4(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_contrast(-4));
    TEST_ASSERT_EQUAL_INT8(-4, photo_quality_get_contrast());
}

void test_contrast_set_5(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_contrast(5));
    TEST_ASSERT_EQUAL_INT8(5, photo_quality_get_contrast());
}

void test_contrast_set_minus_5(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_contrast(-5));
    TEST_ASSERT_EQUAL_INT8(-5, photo_quality_get_contrast());
}

void test_contrast_set_6_returns_error(void) {
    TEST_ASSERT_EQUAL_INT(-22, photo_quality_set_contrast(6));
}

/* ---- Test: Brightness settings ---- */
void test_brightness_default_is_0(void) {
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_brightness());
}

void test_brightness_set_3(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_brightness(3));
    TEST_ASSERT_EQUAL_INT8(3, photo_quality_get_brightness());
}

void test_brightness_set_minus_3(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_brightness(-3));
    TEST_ASSERT_EQUAL_INT8(-3, photo_quality_get_brightness());
}

void test_brightness_set_5(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_brightness(5));
    TEST_ASSERT_EQUAL_INT8(5, photo_quality_get_brightness());
}

void test_brightness_set_minus_5(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_brightness(-5));
    TEST_ASSERT_EQUAL_INT8(-5, photo_quality_get_brightness());
}

void test_brightness_set_6_returns_error(void) {
    TEST_ASSERT_EQUAL_INT(-22, photo_quality_set_brightness(6));
}

/* ---- Test: Saturation settings ---- */
void test_saturation_default_is_0(void) {
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_saturation());
}

void test_saturation_set_4(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_saturation(4));
    TEST_ASSERT_EQUAL_INT8(4, photo_quality_get_saturation());
}

void test_saturation_set_minus_4(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_saturation(-4));
    TEST_ASSERT_EQUAL_INT8(-4, photo_quality_get_saturation());
}

void test_saturation_set_5(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_saturation(5));
    TEST_ASSERT_EQUAL_INT8(5, photo_quality_get_saturation());
}

void test_saturation_set_minus_5(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_saturation(-5));
    TEST_ASSERT_EQUAL_INT8(-5, photo_quality_get_saturation());
}

void test_saturation_set_6_returns_error(void) {
    TEST_ASSERT_EQUAL_INT(-22, photo_quality_set_saturation(6));
}

/* ---- Test: Configuration get/set ---- */
void test_get_config_populates_all_fields(void) {
    photo_quality_config_t config;
    
    photo_quality_set_jpeg_quality(75);
    photo_quality_set_sharpness(3);
    photo_quality_set_contrast(-2);
    photo_quality_set_brightness(4);
    photo_quality_set_saturation(-1);
    
    TEST_ASSERT_EQUAL_INT(0, photo_quality_get_config(&config));
    TEST_ASSERT_EQUAL_UINT8(75, config.jpeg_quality);
    TEST_ASSERT_EQUAL_INT8(3, config.sharpness);
    TEST_ASSERT_EQUAL_INT8(-2, config.contrast);
    TEST_ASSERT_EQUAL_INT8(4, config.brightness);
    TEST_ASSERT_EQUAL_INT8(-1, config.saturation);
}

void test_set_config_applies_all_values(void) {
    photo_quality_config_t config = {
        .jpeg_quality = 90,
        .sharpness = 2,
        .contrast = -3,
        .brightness = 5,
        .saturation = -2
    };
    
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_config(&config));
    TEST_ASSERT_EQUAL_UINT8(90, photo_quality_get_jpeg_quality());
    TEST_ASSERT_EQUAL_INT8(2, photo_quality_get_sharpness());
    TEST_ASSERT_EQUAL_INT8(-3, photo_quality_get_contrast());
    TEST_ASSERT_EQUAL_INT8(5, photo_quality_get_brightness());
    TEST_ASSERT_EQUAL_INT8(-2, photo_quality_get_saturation());
}

void test_get_config_null_pointer_returns_error(void) {
    TEST_ASSERT_EQUAL_INT(-22, photo_quality_get_config(NULL));
}

void test_set_config_null_pointer_returns_error(void) {
    TEST_ASSERT_EQUAL_INT(-22, photo_quality_set_config(NULL));
}

/* ---- Test: Quality presets ---- */
void test_quality_preset_low(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_level(QUALITY_LOW));
    TEST_ASSERT_EQUAL_UINT8(50, photo_quality_get_jpeg_quality());
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_sharpness());
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_contrast());
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_brightness());
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_saturation());
}

void test_quality_preset_medium(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_level(QUALITY_MEDIUM));
    TEST_ASSERT_EQUAL_UINT8(85, photo_quality_get_jpeg_quality());
    TEST_ASSERT_EQUAL_INT8(1, photo_quality_get_sharpness());
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_contrast());
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_brightness());
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_saturation());
}

void test_quality_preset_high(void) {
    TEST_ASSERT_EQUAL_INT(0, photo_quality_set_level(QUALITY_HIGH));
    TEST_ASSERT_EQUAL_UINT8(95, photo_quality_get_jpeg_quality());
    TEST_ASSERT_EQUAL_INT8(2, photo_quality_get_sharpness());
    TEST_ASSERT_EQUAL_INT8(1, photo_quality_get_contrast());
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_brightness());
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_saturation());
}

/* ---- Test: Reset defaults ---- */
void test_reset_defaults_restores_medium_preset(void) {
    photo_quality_set_jpeg_quality(100);
    photo_quality_set_sharpness(5);
    photo_quality_set_contrast(5);
    photo_quality_set_brightness(5);
    photo_quality_set_saturation(5);
    
    TEST_ASSERT_EQUAL_INT(0, photo_quality_reset_defaults());
    TEST_ASSERT_EQUAL_UINT8(85, photo_quality_get_jpeg_quality());
    TEST_ASSERT_EQUAL_INT8(1, photo_quality_get_sharpness());
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_contrast());
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_brightness());
    TEST_ASSERT_EQUAL_INT8(0, photo_quality_get_saturation());
}

/* ---- Test: String conversion ---- */
void test_quality_to_string_low(void) {
    const char* str = quality_to_string(QUALITY_LOW);
    TEST_ASSERT_EQUAL_STRING("Low", str);
}

void test_quality_to_string_medium(void) {
    const char* str = quality_to_string(QUALITY_MEDIUM);
    TEST_ASSERT_EQUAL_STRING("Medium", str);
}

void test_quality_to_string_high(void) {
    const char* str = quality_to_string(QUALITY_HIGH);
    TEST_ASSERT_EQUAL_STRING("High", str);
}
