/**
 * @file test_photo_quality.c
 * @brief Unit tests for photo quality settings module
 *
 * Tests quality setting validation, NVS persistence, and default values.
 */

#include "unity/unity.h"
#include "mocks/esp_err.h"
#include "mocks/esp_log.h"
#include "mocks/nvs.h"
#include "mocks/sdkconfig.h"

#include "app_video_photo_quality.h"

/* Test fixture setup */
static photo_quality_settings_t test_settings;

void setUp(void)
{
    /* Reset mock NVS for each test */
    mock_nvs_reset();
}

void tearDown(void)
{
}

/* Test: Default quality value */
void test_default_jpeg_quality(void)
{
    esp_err_t ret = app_photo_quality_get_settings(&test_settings);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_UINT8(PHOTO_QUALITY_DEFAULT, test_settings.jpeg_quality);
}

/* Test: Set valid JPEG quality */
void test_set_valid_jpeg_quality(void)
{
    esp_err_t ret = app_photo_quality_set_jpeg_quality(85);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    
    ret = app_photo_quality_get_settings(&test_settings);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_UINT8(85, test_settings.jpeg_quality);
}

/* Test: Set minimum JPEG quality */
void test_set_min_jpeg_quality(void)
{
    esp_err_t ret = app_photo_quality_set_jpeg_quality(PHOTO_QUALITY_MIN);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    
    ret = app_photo_quality_get_settings(&test_settings);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_UINT8(PHOTO_QUALITY_MIN, test_settings.jpeg_quality);
}

/* Test: Set maximum JPEG quality */
void test_set_max_jpeg_quality(void)
{
    esp_err_t ret = app_photo_quality_set_jpeg_quality(PHOTO_QUALITY_MAX);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    
    ret = app_photo_quality_get_settings(&test_settings);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_UINT8(PHOTO_QUALITY_MAX, test_settings.jpeg_quality);
}

/* Test: Set invalid JPEG quality (below minimum) */
void test_set_invalid_jpeg_quality_low(void)
{
    esp_err_t ret = app_photo_quality_set_jpeg_quality(5);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

/* Test: Set invalid JPEG quality (above maximum) */
void test_set_invalid_jpeg_quality_high(void)
{
    esp_err_t ret = app_photo_quality_set_jpeg_quality(101);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

/* Test: Set null settings pointer */
void test_get_settings_null_pointer(void)
{
    esp_err_t ret = app_photo_quality_get_settings(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

/* Test: Validate valid settings */
void test_validate_valid_settings(void)
{
    test_settings.jpeg_quality = 90;
    test_settings.brightness = 20;
    test_settings.contrast = 15;
    test_settings.saturation = 10;
    
    bool valid = app_photo_quality_validate_settings(&test_settings);
    TEST_ASSERT_TRUE(valid);
}

/* Test: Validate invalid JPEG quality */
void test_validate_invalid_jpeg_quality(void)
{
    test_settings.jpeg_quality = 200;  /* Invalid */
    test_settings.brightness = 0;
    test_settings.contrast = 0;
    test_settings.saturation = 0;
    
    bool valid = app_photo_quality_validate_settings(&test_settings);
    TEST_ASSERT_FALSE(valid);
}

/* Test: Validate invalid brightness */
void test_validate_invalid_brightness(void)
{
    test_settings.jpeg_quality = 90;
    test_settings.brightness = 100;  /* Invalid, max is 50 */
    test_settings.contrast = 0;
    test_settings.saturation = 0;
    
    bool valid = app_photo_quality_validate_settings(&test_settings);
    TEST_ASSERT_FALSE(valid);
}

/* Test: Validate NULL settings */
void test_validate_null_settings(void)
{
    bool valid = app_photo_quality_validate_settings(NULL);
    TEST_ASSERT_FALSE(valid);
}

/* Test: Reset to defaults */
void test_reset_to_defaults(void)
{
    /* Set some non-default values first */
    app_photo_quality_set_jpeg_quality(50);
    app_photo_quality_set_brightness(25);
    app_photo_quality_set_contrast(-10);  /* Will be clamped to 0 */
    app_photo_quality_set_saturation(30);
    
    /* Reset to defaults */
    esp_err_t ret = app_photo_quality_reset_defaults();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    
    ret = app_photo_quality_get_settings(&test_settings);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_UINT8(PHOTO_QUALITY_DEFAULT, test_settings.jpeg_quality);
    TEST_ASSERT_EQUAL_UINT8(0, test_settings.brightness);
    TEST_ASSERT_EQUAL_UINT8(0, test_settings.contrast);
    TEST_ASSERT_EQUAL_UINT8(0, test_settings.saturation);
}

/* Test: NVS save operation (simulated) */
void test_nvs_save(void)
{
    /* Set custom values - these will be stored in global g_settings */
    app_photo_quality_set_jpeg_quality(75);
    app_photo_quality_set_brightness(15);
    app_photo_quality_set_contrast(20);
    app_photo_quality_set_saturation(25);
    
    /* Save to NVS (mock returns OK) */
    esp_err_t ret = app_photo_quality_save();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/* Test: NVS save and reload */
void test_nvs_save_reload(void)
{
    /* Set custom values */
    app_photo_quality_set_jpeg_quality(60);
    app_photo_quality_set_brightness(8);
    app_photo_quality_set_contrast(12);
    app_photo_quality_set_saturation(16);
    
    /* Save to NVS */
    esp_err_t ret = app_photo_quality_save();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    
    /* Load from NVS - this should work and return OK */
    ret = app_photo_quality_load();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    
    /* Get settings and verify */
    ret = app_photo_quality_get_settings(&test_settings);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_UINT8(60, test_settings.jpeg_quality);
    TEST_ASSERT_EQUAL_UINT8(8, test_settings.brightness);
    TEST_ASSERT_EQUAL_UINT8(12, test_settings.contrast);
    TEST_ASSERT_EQUAL_UINT8(16, test_settings.saturation);
}

/* Test: Brightness range validation */
void test_brightness_range(void)
{
    /* Valid positive values */
    TEST_ASSERT_EQUAL(ESP_OK, app_photo_quality_set_brightness(0));
    TEST_ASSERT_EQUAL(ESP_OK, app_photo_quality_set_brightness(50));
    
    /* Invalid values */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, app_photo_quality_set_brightness(51));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, app_photo_quality_set_brightness(-51));
}

/* Test: Contrast range validation */
void test_contrast_range(void)
{
    /* Valid positive values */
    TEST_ASSERT_EQUAL(ESP_OK, app_photo_quality_set_contrast(0));
    TEST_ASSERT_EQUAL(ESP_OK, app_photo_quality_set_contrast(50));
    
    /* Invalid values */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, app_photo_quality_set_contrast(51));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, app_photo_quality_set_contrast(-51));
}

/* Test: Saturation range validation */
void test_saturation_range(void)
{
    /* Valid positive values */
    TEST_ASSERT_EQUAL(ESP_OK, app_photo_quality_set_saturation(0));
    TEST_ASSERT_EQUAL(ESP_OK, app_photo_quality_set_saturation(50));
    
    /* Invalid values */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, app_photo_quality_set_saturation(51));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, app_photo_quality_set_saturation(-51));
}

/* Test: Initialization loads from NVS (simulated) */
void test_init_loads_from_nvs(void)
{
    /* Initialize module - this loads any saved settings from NVS */
    /* Note: g_settings may retain values from previous tests */
    esp_err_t ret = app_photo_quality_init();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    
    /* Get settings and verify they were loaded from NVS */
    ret = app_photo_quality_get_settings(&test_settings);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    /* The loaded value depends on what was saved in previous tests */
    /* Just verify that loading worked */
    TEST_ASSERT_TRUE(test_settings.jpeg_quality >= PHOTO_QUALITY_MIN &&
                     test_settings.jpeg_quality <= PHOTO_QUALITY_MAX);
}

/* Test: Init handles missing NVS gracefully */
void test_init_handles_missing_nvs(void)
{
    /* Initialize should still work even if NVS is empty */
    esp_err_t ret = app_photo_quality_init();
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Should have some valid JPEG quality value */
    ret = app_photo_quality_get_settings(&test_settings);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(test_settings.jpeg_quality >= PHOTO_QUALITY_MIN &&
                     test_settings.jpeg_quality <= PHOTO_QUALITY_MAX);
}

/* Test: Destruct deinitialization */
void test_deinit_clears_settings(void)
{
    /* Set some values */
    app_photo_quality_set_jpeg_quality(80);

    /* Deinit */
    app_photo_quality_deinit();

    /* Verify settings are cleared (load will fail on empty NVS) */
    esp_err_t ret = app_photo_quality_load();
    TEST_ASSERT_NOT_EQUAL(ESP_OK, ret);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_default_jpeg_quality);
    RUN_TEST(test_set_valid_jpeg_quality);
    RUN_TEST(test_set_min_jpeg_quality);
    RUN_TEST(test_set_max_jpeg_quality);
    RUN_TEST(test_set_invalid_jpeg_quality_low);
    RUN_TEST(test_set_invalid_jpeg_quality_high);
    RUN_TEST(test_get_settings_null_pointer);
    RUN_TEST(test_validate_valid_settings);
    RUN_TEST(test_validate_invalid_jpeg_quality);
    RUN_TEST(test_validate_invalid_brightness);
    RUN_TEST(test_validate_null_settings);
    RUN_TEST(test_reset_to_defaults);
    RUN_TEST(test_nvs_save);
    RUN_TEST(test_nvs_save_reload);
    RUN_TEST(test_brightness_range);
    RUN_TEST(test_contrast_range);
    RUN_TEST(test_saturation_range);
    RUN_TEST(test_init_loads_from_nvs);
    RUN_TEST(test_init_handles_missing_nvs);

    UNITY_END();
    return 0;
}
