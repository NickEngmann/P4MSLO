/**
 * @brief Tests for NVS storage operations (app_storage.c)
 *
 * Tests NVS read/write for settings, camera parameters, interval state,
 * photo count, and gyroscope settings.
 */

#include "unity/unity.h"
#include "mocks/esp_err.h"
#include "mocks/esp_log.h"
#include "mocks/nvs.h"
#include "mocks/nvs_flash.h"
#include "mocks/sdkconfig.h"
#include "mocks/ui_extra.h"

/* ---- NVS Constants (from app_storage.c) ---- */
#define NVS_NAMESPACE "p4_eye_cfg"
#define NVS_KEY_OD "od"
#define NVS_KEY_RESOLUTION "resolution"
#define NVS_KEY_FLASH "flash"
#define NVS_KEY_INTERVAL_TIME "int_time"
#define NVS_KEY_MAGNIFICATION "magnify"
#define NVS_KEY_INTERVAL_ACTIVE "int_active"
#define NVS_KEY_NEXT_WAKE_TIME "wake_time"
#define NVS_KEY_PHOTO_COUNT "photo_count"
#define NVS_KEY_CONTRAST "contrast"
#define NVS_KEY_SATURATION "saturation"
#define NVS_KEY_BRIGHTNESS "brightness"
#define NVS_KEY_HUE "hue"
#define NVS_KEY_GYROSCOPE "gyroscope"

/* ---- Test: NVS init ---- */
void test_nvs_flash_init(void) {
    mock_nvs_reset();
    esp_err_t ret = nvs_flash_init();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

void test_nvs_flash_erase_and_reinit(void) {
    mock_nvs_reset();
    nvs_flash_init();
    /* Simulate NVS corrupt → erase → reinit */
    esp_err_t ret = nvs_flash_erase();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    ret = nvs_flash_init();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/* ---- Test: Photo count ---- */
void test_photo_count_save_and_load(void) {
    mock_nvs_reset();
    nvs_handle_t handle;
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    nvs_set_u16(handle, NVS_KEY_PHOTO_COUNT, 42);
    nvs_commit(handle);
    nvs_close(handle);

    uint16_t count = 0;
    nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    esp_err_t ret = nvs_get_u16(handle, NVS_KEY_PHOTO_COUNT, &count);
    nvs_close(handle);

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_UINT16(42, count);
}

void test_photo_count_not_found(void) {
    mock_nvs_reset();
    nvs_handle_t handle;
    nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    uint16_t count = 999;
    esp_err_t ret = nvs_get_u16(handle, NVS_KEY_PHOTO_COUNT, &count);
    nvs_close(handle);

    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, ret);
}

/* ---- Test: Interval state ---- */
void test_interval_state_roundtrip(void) {
    mock_nvs_reset();
    nvs_handle_t handle;

    /* Save */
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    nvs_set_u8(handle, NVS_KEY_INTERVAL_ACTIVE, 1);
    nvs_set_u32(handle, NVS_KEY_NEXT_WAKE_TIME, 1234567890);
    nvs_commit(handle);
    nvs_close(handle);

    /* Load */
    nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    uint8_t active = 0;
    uint32_t wake_time = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u8(handle, NVS_KEY_INTERVAL_ACTIVE, &active));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u32(handle, NVS_KEY_NEXT_WAKE_TIME, &wake_time));
    nvs_close(handle);

    TEST_ASSERT_EQUAL_UINT8(1, active);
    TEST_ASSERT_EQUAL_UINT32(1234567890, wake_time);
}

/* ---- Test: Settings roundtrip ---- */
void test_settings_save_and_load(void) {
    mock_nvs_reset();
    nvs_handle_t handle;

    /* Save */
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    nvs_set_u8(handle, NVS_KEY_OD, 1);          /* On */
    nvs_set_u8(handle, NVS_KEY_RESOLUTION, 1);   /* 1080P */
    nvs_set_u8(handle, NVS_KEY_FLASH, 0);        /* Off */
    nvs_set_u16(handle, NVS_KEY_INTERVAL_TIME, 30);
    nvs_set_u16(handle, NVS_KEY_MAGNIFICATION, 5);
    nvs_set_u8(handle, NVS_KEY_GYROSCOPE, 1);    /* On */
    nvs_commit(handle);
    nvs_close(handle);

    /* Load */
    nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    uint8_t od, res, flash, gyro;
    uint16_t interval, mag;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u8(handle, NVS_KEY_OD, &od));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u8(handle, NVS_KEY_RESOLUTION, &res));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u8(handle, NVS_KEY_FLASH, &flash));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u16(handle, NVS_KEY_INTERVAL_TIME, &interval));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u16(handle, NVS_KEY_MAGNIFICATION, &mag));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u8(handle, NVS_KEY_GYROSCOPE, &gyro));
    nvs_close(handle);

    TEST_ASSERT_EQUAL_UINT8(1, od);
    TEST_ASSERT_EQUAL_UINT8(1, res);
    TEST_ASSERT_EQUAL_UINT8(0, flash);
    TEST_ASSERT_EQUAL_UINT16(30, interval);
    TEST_ASSERT_EQUAL_UINT16(5, mag);
    TEST_ASSERT_EQUAL_UINT8(1, gyro);
}

/* ---- Test: Camera settings ---- */
void test_camera_settings_roundtrip(void) {
    mock_nvs_reset();
    nvs_handle_t handle;

    /* Save */
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    nvs_set_u32(handle, NVS_KEY_CONTRAST, 75);
    nvs_set_u32(handle, NVS_KEY_SATURATION, 50);
    nvs_set_u32(handle, NVS_KEY_BRIGHTNESS, 80);
    nvs_set_u32(handle, NVS_KEY_HUE, 120);
    nvs_commit(handle);
    nvs_close(handle);

    /* Load */
    nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    uint32_t c, s, b, h;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u32(handle, NVS_KEY_CONTRAST, &c));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u32(handle, NVS_KEY_SATURATION, &s));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u32(handle, NVS_KEY_BRIGHTNESS, &b));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u32(handle, NVS_KEY_HUE, &h));
    nvs_close(handle);

    TEST_ASSERT_EQUAL_UINT32(75, c);
    TEST_ASSERT_EQUAL_UINT32(50, s);
    TEST_ASSERT_EQUAL_UINT32(80, b);
    TEST_ASSERT_EQUAL_UINT32(120, h);
}

/* ---- Test: Gyroscope setting ---- */
void test_gyroscope_setting_on_off(void) {
    mock_nvs_reset();
    nvs_handle_t handle;

    /* Save enabled */
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    nvs_set_u8(handle, NVS_KEY_GYROSCOPE, 1);
    nvs_commit(handle);
    nvs_close(handle);

    nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    uint8_t val;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u8(handle, NVS_KEY_GYROSCOPE, &val));
    TEST_ASSERT_EQUAL_UINT8(1, val);
    nvs_close(handle);

    /* Save disabled */
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    nvs_set_u8(handle, NVS_KEY_GYROSCOPE, 0);
    nvs_commit(handle);
    nvs_close(handle);

    nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u8(handle, NVS_KEY_GYROSCOPE, &val));
    TEST_ASSERT_EQUAL_UINT8(0, val);
    nvs_close(handle);
}

/* ---- Test: Type mismatch ---- */
void test_nvs_type_mismatch(void) {
    mock_nvs_reset();
    nvs_handle_t handle;

    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    nvs_set_u8(handle, "test_key", 42);
    nvs_commit(handle);
    nvs_close(handle);

    nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    uint32_t val32;
    esp_err_t ret = nvs_get_u32(handle, "test_key", &val32);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_TYPE_MISMATCH, ret);
    nvs_close(handle);
}

/* ---- Test: Overwrite existing key ---- */
void test_nvs_overwrite(void) {
    mock_nvs_reset();
    nvs_handle_t handle;

    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    nvs_set_u16(handle, NVS_KEY_PHOTO_COUNT, 10);
    nvs_commit(handle);
    nvs_close(handle);

    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    nvs_set_u16(handle, NVS_KEY_PHOTO_COUNT, 20);
    nvs_commit(handle);
    nvs_close(handle);

    nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    uint16_t count;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u16(handle, NVS_KEY_PHOTO_COUNT, &count));
    TEST_ASSERT_EQUAL_UINT16(20, count);
    nvs_close(handle);
}

/* ---- Main ---- */
int main(void) {
    printf("\n===== NVS Storage Tests =====\n");
    UNITY_BEGIN();

    RUN_TEST(test_nvs_flash_init);
    RUN_TEST(test_nvs_flash_erase_and_reinit);
    RUN_TEST(test_photo_count_save_and_load);
    RUN_TEST(test_photo_count_not_found);
    RUN_TEST(test_interval_state_roundtrip);
    RUN_TEST(test_settings_save_and_load);
    RUN_TEST(test_camera_settings_roundtrip);
    RUN_TEST(test_gyroscope_setting_on_off);
    RUN_TEST(test_nvs_type_mismatch);
    RUN_TEST(test_nvs_overwrite);

    UNITY_END();
}
