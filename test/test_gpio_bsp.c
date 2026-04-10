/**
 * @brief Tests for GPIO and BSP operations
 *
 * Tests GPIO pin state tracking, BSP initialization functions,
 * display brightness, flashlight control, and SD card detection.
 */

#include "unity/unity.h"
#include "mocks/esp_err.h"
#include "mocks/esp_log.h"
#include "mocks/driver/gpio.h"
#include "mocks/bsp/esp32_p4_eye.h"
#include "mocks/sdkconfig.h"

/* ---- GPIO Tests ---- */
void test_gpio_set_and_get(void) {
    mock_gpio_reset();
    gpio_set_level(GPIO_NUM_23, 1);
    TEST_ASSERT_EQUAL(1, gpio_get_level(GPIO_NUM_23));
    gpio_set_level(GPIO_NUM_23, 0);
    TEST_ASSERT_EQUAL(0, gpio_get_level(GPIO_NUM_23));
}

void test_gpio_config_sets_mode(void) {
    mock_gpio_reset();
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(GPIO_NUM_3) | BIT64(GPIO_NUM_4),
        .mode = GPIO_MODE_INPUT,
    };
    esp_err_t ret = gpio_config(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT, mock_gpio_modes[GPIO_NUM_3]);
    TEST_ASSERT_EQUAL(GPIO_MODE_INPUT, mock_gpio_modes[GPIO_NUM_4]);
}

void test_gpio_invalid_pin(void) {
    esp_err_t ret = gpio_set_level(-1, 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

void test_gpio_multiple_pins(void) {
    mock_gpio_reset();
    for (int i = 0; i < 10; i++) {
        gpio_set_level(i, i % 2);
    }
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL(i % 2, gpio_get_level(i));
    }
}

/* ---- BSP Tests ---- */
void test_bsp_flashlight(void) {
    mock_bsp_flashlight_state = 0;
    TEST_ASSERT_EQUAL(ESP_OK, bsp_flashlight_init());
    TEST_ASSERT_FALSE(bsp_get_flashlight_status());

    bsp_flashlight_set(true);
    TEST_ASSERT_TRUE(bsp_get_flashlight_status());

    bsp_flashlight_set(false);
    TEST_ASSERT_FALSE(bsp_get_flashlight_status());
}

void test_bsp_display_lock_unlock(void) {
    mock_bsp_display_locked = false;
    TEST_ASSERT_TRUE(bsp_display_lock(0));
    TEST_ASSERT_TRUE(mock_bsp_display_locked);
    bsp_display_unlock();
    TEST_ASSERT_FALSE(mock_bsp_display_locked);
}

void test_bsp_display_brightness(void) {
    mock_bsp_brightness = 0;
    bsp_display_backlight_on();
    TEST_ASSERT_EQUAL(100, mock_bsp_brightness);
    bsp_display_backlight_off();
    TEST_ASSERT_EQUAL(0, mock_bsp_brightness);
    bsp_display_brightness_set(50);
    TEST_ASSERT_EQUAL(50, mock_bsp_brightness);
}

void test_bsp_i2c_init(void) {
    TEST_ASSERT_EQUAL(ESP_OK, bsp_i2c_init());
    i2c_master_bus_handle_t handle;
    TEST_ASSERT_EQUAL(ESP_OK, bsp_get_i2c_bus_handle(&handle));
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(ESP_OK, bsp_i2c_deinit());
}

void test_bsp_sd_card_detection(void) {
    mock_bsp_sd_present = false;
    TEST_ASSERT_FALSE(bsp_sdcard_is_present());
    mock_bsp_sd_present = true;
    TEST_ASSERT_TRUE(bsp_sdcard_is_present());
}

void test_bsp_button_create(void) {
    button_handle_t btns[BSP_BUTTON_NUM];
    int cnt = 0;
    TEST_ASSERT_EQUAL(ESP_OK, bsp_iot_button_create(btns, &cnt, BSP_BUTTON_NUM));
    TEST_ASSERT_EQUAL(BSP_BUTTON_NUM, cnt);
    for (int i = 0; i < BSP_BUTTON_NUM; i++) {
        TEST_ASSERT_NOT_NULL(btns[i]);
    }
}

void test_bsp_knob_init_and_register(void) {
    TEST_ASSERT_EQUAL(ESP_OK, bsp_knob_init());
    TEST_ASSERT_EQUAL(ESP_OK, bsp_knob_register_cb(KNOB_LEFT, NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, bsp_knob_register_cb(KNOB_RIGHT, NULL, NULL));
}

/* ---- Pin Definitions ---- */
void test_pin_definitions(void) {
    /* Verify key pin assignments match hardware */
    TEST_ASSERT_EQUAL(GPIO_NUM_13, BSP_I2C_SCL);
    TEST_ASSERT_EQUAL(GPIO_NUM_14, BSP_I2C_SDA);
    TEST_ASSERT_EQUAL(GPIO_NUM_23, BSP_LED_FLASHLIGHT);
    TEST_ASSERT_EQUAL(GPIO_NUM_45, BSP_SD_DETECT_PIN);
    TEST_ASSERT_EQUAL(GPIO_NUM_3, BSP_BUTTON_NUM1);
    TEST_ASSERT_EQUAL(GPIO_NUM_4, BSP_BUTTON_NUM2);
    TEST_ASSERT_EQUAL(GPIO_NUM_5, BSP_BUTTON_NUM3);
    TEST_ASSERT_EQUAL(GPIO_NUM_2, BSP_BUTTON_ENCODER);
    TEST_ASSERT_EQUAL(GPIO_NUM_48, BSP_KNOB_A);
    TEST_ASSERT_EQUAL(GPIO_NUM_47, BSP_KNOB_B);
    TEST_ASSERT_EQUAL(4, BSP_BUTTON_NUM);
}

/* ---- Main ---- */
int main(void) {
    printf("\n===== GPIO & BSP Tests =====\n");
    UNITY_BEGIN();

    RUN_TEST(test_gpio_set_and_get);
    RUN_TEST(test_gpio_config_sets_mode);
    RUN_TEST(test_gpio_invalid_pin);
    RUN_TEST(test_gpio_multiple_pins);
    RUN_TEST(test_bsp_flashlight);
    RUN_TEST(test_bsp_display_lock_unlock);
    RUN_TEST(test_bsp_display_brightness);
    RUN_TEST(test_bsp_i2c_init);
    RUN_TEST(test_bsp_sd_card_detection);
    RUN_TEST(test_bsp_button_create);
    RUN_TEST(test_bsp_knob_init_and_register);
    RUN_TEST(test_pin_definitions);

    UNITY_END();
}
