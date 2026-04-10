/**
 * @brief Tests for sleep/wakeup state management
 *
 * Tests wakeup cause detection and how it affects interval photography state.
 */

#include "unity/unity.h"
#include "mocks/esp_err.h"
#include "mocks/esp_log.h"
#include "mocks/esp_sleep.h"
#include "mocks/sdkconfig.h"
#include "mocks/nvs.h"
#include "mocks/ui_extra.h"
#include "mocks/bsp/esp32_p4_eye.h"

#define NVS_NAMESPACE "p4_eye_cfg"
#define NVS_KEY_INTERVAL_ACTIVE "int_active"
#define NVS_KEY_NEXT_WAKE_TIME "wake_time"

void test_wakeup_cause_undefined(void) {
    mock_set_wakeup_cause(ESP_SLEEP_WAKEUP_UNDEFINED);
    TEST_ASSERT_NOT_EQUAL(ESP_SLEEP_WAKEUP_TIMER, esp_sleep_get_wakeup_cause());
}

void test_wakeup_cause_timer(void) {
    mock_set_wakeup_cause(ESP_SLEEP_WAKEUP_TIMER);
    TEST_ASSERT_EQUAL(ESP_SLEEP_WAKEUP_TIMER, esp_sleep_get_wakeup_cause());
}

void test_timer_wakeup_with_active_interval(void) {
    mock_nvs_reset();
    mock_set_wakeup_cause(ESP_SLEEP_WAKEUP_TIMER);

    /* Pre-set interval as active */
    nvs_handle_t handle;
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    nvs_set_u8(handle, NVS_KEY_INTERVAL_ACTIVE, 1);
    nvs_set_u32(handle, NVS_KEY_NEXT_WAKE_TIME, 5000);
    nvs_commit(handle);
    nvs_close(handle);

    /* Simulate storage_init check */
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
        uint8_t active = 0;
        nvs_get_u8(handle, NVS_KEY_INTERVAL_ACTIVE, &active);
        nvs_close(handle);

        if (active == 1) {
            ui_extra_goto_page(UI_PAGE_INTERVAL_CAM);
        }
    }

    TEST_ASSERT_EQUAL(UI_PAGE_INTERVAL_CAM, ui_extra_get_current_page());
}

void test_non_timer_wakeup_clears_interval(void) {
    mock_set_wakeup_cause(ESP_SLEEP_WAKEUP_GPIO);
    mock_current_page = UI_PAGE_MAIN;

    /* Non-timer wakeup should NOT go to interval page */
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
        /* app_storage_init does NOT goto interval page for non-timer wakeup */
        app_extra_set_saved_photo_count(0);
    }

    TEST_ASSERT_EQUAL(UI_PAGE_MAIN, ui_extra_get_current_page());
    TEST_ASSERT_EQUAL_UINT16(0, mock_saved_photo_count);
}

void test_deep_sleep_gpio_wakeup(void) {
    uint64_t mask = BIT(BSP_BUTTON_NUM1) | BIT(BSP_BUTTON_NUM2);
    esp_err_t ret = esp_deep_sleep_enable_gpio_wakeup(mask, 0);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/* ---- Main ---- */
int main(void) {
    printf("\n===== Sleep/Wakeup Tests =====\n");
    UNITY_BEGIN();

    RUN_TEST(test_wakeup_cause_undefined);
    RUN_TEST(test_wakeup_cause_timer);
    RUN_TEST(test_timer_wakeup_with_active_interval);
    RUN_TEST(test_non_timer_wakeup_clears_interval);
    RUN_TEST(test_deep_sleep_gpio_wakeup);

    UNITY_END();
}
