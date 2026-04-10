/**
 * @brief Tests for UI state management and page navigation
 *
 * Tests page navigation, magnification factor, AI detect mode,
 * SD card/USB state, and settings struct usage patterns.
 */

#include "unity/unity.h"
#include "mocks/esp_err.h"
#include "mocks/esp_log.h"
#include "mocks/sdkconfig.h"
#include "mocks/ui_extra.h"
#include "mocks/esp_timer.h"

/* ---- Page Navigation ---- */
void test_default_page_is_main(void) {
    mock_current_page = UI_PAGE_MAIN;
    TEST_ASSERT_EQUAL(UI_PAGE_MAIN, ui_extra_get_current_page());
}

void test_goto_camera_page(void) {
    ui_extra_goto_page(UI_PAGE_CAMERA);
    TEST_ASSERT_EQUAL(UI_PAGE_CAMERA, ui_extra_get_current_page());
}

void test_goto_all_pages(void) {
    ui_page_t pages[] = {
        UI_PAGE_MAIN, UI_PAGE_CAMERA, UI_PAGE_SETTINGS,
        UI_PAGE_ALBUM, UI_PAGE_USB_DISK, UI_PAGE_INTERVAL_CAM,
        UI_PAGE_VIDEO_MODE, UI_PAGE_AI_DETECT, UI_PAGE_PIC_SAVE,
    };
    for (int i = 0; i < (int)(sizeof(pages) / sizeof(pages[0])); i++) {
        ui_extra_goto_page(pages[i]);
        TEST_ASSERT_EQUAL(pages[i], ui_extra_get_current_page());
    }
}

/* ---- Magnification ---- */
void test_magnification_factor(void) {
    app_extra_set_magnification_factor(1);
    TEST_ASSERT_EQUAL(1, app_extra_get_magnification_factor());
    app_extra_set_magnification_factor(5);
    TEST_ASSERT_EQUAL(5, app_extra_get_magnification_factor());
}

void test_magnification_increment_decrement(void) {
    app_extra_set_magnification_factor(3);
    app_extra_set_magnification_factor(app_extra_get_magnification_factor() + 1);
    TEST_ASSERT_EQUAL(4, app_extra_get_magnification_factor());
    app_extra_set_magnification_factor(app_extra_get_magnification_factor() - 1);
    TEST_ASSERT_EQUAL(3, app_extra_get_magnification_factor());
}

/* ---- AI Detect Mode ---- */
void test_ai_detect_mode(void) {
    mock_ai_detect_mode = AI_DETECT_FACE;
    TEST_ASSERT_EQUAL(AI_DETECT_FACE, ui_extra_get_ai_detect_mode());
    mock_ai_detect_mode = AI_DETECT_PEDESTRIAN;
    TEST_ASSERT_EQUAL(AI_DETECT_PEDESTRIAN, ui_extra_get_ai_detect_mode());
}

/* ---- UI Init State ---- */
void test_ui_init_state(void) {
    mock_ui_initialized = false;
    TEST_ASSERT_FALSE(ui_extra_is_ui_init());
    ui_extra_init();
    TEST_ASSERT_TRUE(ui_extra_is_ui_init());
}

/* ---- SD/USB State ---- */
void test_sd_card_state(void) {
    ui_extra_set_sd_card_mounted(false);
    TEST_ASSERT_FALSE(mock_sd_card_mounted);
    ui_extra_set_sd_card_mounted(true);
    TEST_ASSERT_TRUE(mock_sd_card_mounted);
}

void test_usb_disk_state(void) {
    ui_extra_set_usb_disk_mounted(false);
    TEST_ASSERT_FALSE(mock_usb_disk_mounted);
    ui_extra_set_usb_disk_mounted(true);
    TEST_ASSERT_TRUE(mock_usb_disk_mounted);
}

/* ---- Settings Struct ---- */
void test_settings_struct(void) {
    settings_info_t settings = {
        .od = "On",
        .resolution = "1080P",
        .flash = "Off",
        .gyroscope = "On",
    };
    TEST_ASSERT_EQUAL_STRING("On", settings.od);
    TEST_ASSERT_EQUAL_STRING("1080P", settings.resolution);
    TEST_ASSERT_EQUAL_STRING("Off", settings.flash);
    TEST_ASSERT_EQUAL_STRING("On", settings.gyroscope);
}

/* ---- Timer Mock ---- */
void test_timer_mock(void) {
    mock_timer_set(0);
    TEST_ASSERT_EQUAL(0, esp_timer_get_time());
    mock_timer_advance(1000000);  /* 1 second */
    TEST_ASSERT_EQUAL(1000000, esp_timer_get_time());
    mock_timer_advance(500000);
    TEST_ASSERT_EQUAL(1500000, esp_timer_get_time());
}

/* ---- Photo Count ---- */
void test_saved_photo_count(void) {
    app_extra_set_saved_photo_count(0);
    TEST_ASSERT_EQUAL_UINT16(0, mock_saved_photo_count);
    app_extra_set_saved_photo_count(100);
    TEST_ASSERT_EQUAL_UINT16(100, mock_saved_photo_count);
}

/* ---- Main ---- */
int main(void) {
    printf("\n===== UI State Tests =====\n");
    UNITY_BEGIN();

    RUN_TEST(test_default_page_is_main);
    RUN_TEST(test_goto_camera_page);
    RUN_TEST(test_goto_all_pages);
    RUN_TEST(test_magnification_factor);
    RUN_TEST(test_magnification_increment_decrement);
    RUN_TEST(test_ai_detect_mode);
    RUN_TEST(test_ui_init_state);
    RUN_TEST(test_sd_card_state);
    RUN_TEST(test_usb_disk_state);
    RUN_TEST(test_settings_struct);
    RUN_TEST(test_timer_mock);
    RUN_TEST(test_saved_photo_count);

    UNITY_END();
}
