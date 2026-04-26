/**
 * @brief ESP32-P4-EYE BSP mock for host-based testing
 */
#pragma once

#include "../sdkconfig.h"
#include "../esp_err.h"
#include "../driver/gpio.h"
#include "../driver/i2c_master.h"
#include <stdint.h>
#include <stdbool.h>

/* Pin definitions */
#define BSP_I2C_SCL           GPIO_NUM_13
#define BSP_I2C_SDA           GPIO_NUM_14
#define BSP_I2S_DAT           GPIO_NUM_21
#define BSP_I2S_CLK           GPIO_NUM_22
#define BSP_LCD_SPI_MOSI      GPIO_NUM_16
#define BSP_LCD_SPI_CLK       GPIO_NUM_17
#define BSP_LCD_SPI_CS        GPIO_NUM_18
#define BSP_LCD_DC            GPIO_NUM_19
#define BSP_LCD_RST           GPIO_NUM_15
#define BSP_LCD_BACKLIGHT     GPIO_NUM_20
#define BSP_KNOB_A            GPIO_NUM_48
#define BSP_KNOB_B            GPIO_NUM_47
#define BSP_CAMERA_EN_PIN     GPIO_NUM_12
#define BSP_CAMERA_RST_PIN    GPIO_NUM_26
#define BSP_CAMERA_XCLK_PIN   GPIO_NUM_11
#define BSP_C6_EN_PIN         GPIO_NUM_9
#define BSP_SD_EN_PIN         GPIO_NUM_46
#define BSP_SD_DETECT_PIN     GPIO_NUM_45
#define BSP_BUTTON_NUM1       GPIO_NUM_3
#define BSP_BUTTON_NUM2       GPIO_NUM_4
#define BSP_BUTTON_NUM3       GPIO_NUM_5
#define BSP_BUTTON_ENCODER    GPIO_NUM_2
#define BSP_MIPI_CAMERA_XCLK_FREQUENCY 24000000

#define BSP_LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)
#define BSP_LCD_SPI_NUM        0
#define BSP_LCD_DRAW_BUFF_SIZE (BSP_LCD_H_RES * BSP_LCD_V_RES)
#define BSP_LCD_DRAW_BUFF_DOUBLE 0

#define BSP_SD_MOUNT_POINT "/sdcard"

typedef enum {
    BSP_LED_FLASHLIGHT = GPIO_NUM_23,
} bsp_led_t;

typedef enum {
    BSP_BUTTON_1 = 0,
    BSP_BUTTON_2,
    BSP_BUTTON_3,
    BSP_BUTTON_ED,
    BSP_BUTTON_NUM
} bsp_button_t;

/* Button mock types */
typedef void* button_handle_t;
typedef int button_event_t;
typedef void (*button_cb_t)(void *arg, void *data);

#define BUTTON_PRESS_DOWN 0
#define BUTTON_PRESS_UP   1
#define BUTTON_TYPE_GPIO  0

typedef struct {
    int type;
    struct {
        int active_level;
        int gpio_num;
    } gpio_button_config;
} button_config_t;

/* Knob mock types */
typedef void* knob_handle_t;
typedef int knob_event_t;
typedef void (*knob_cb_t)(void *arg, void *data);

#define KNOB_LEFT  0
#define KNOB_RIGHT 1

typedef struct {
    int default_direction;
    int gpio_encoder_a;
    int gpio_encoder_b;
} knob_config_t;

/* Mock state tracking */
static int mock_bsp_flashlight_state = 0;
static bool mock_bsp_display_locked = false;
static int mock_bsp_brightness = 0;
static bool mock_bsp_sd_present = false;

/* BSP function stubs */
static inline esp_err_t bsp_flashlight_init(void) { return ESP_OK; }
static inline esp_err_t bsp_flashlight_set(const bool on) { mock_bsp_flashlight_state = on; return ESP_OK; }
static inline bool bsp_get_flashlight_status(void) { return mock_bsp_flashlight_state; }

static inline esp_err_t bsp_i2c_init(void) { return ESP_OK; }
static inline esp_err_t bsp_i2c_deinit(void) { return ESP_OK; }
static inline esp_err_t bsp_get_i2c_bus_handle(i2c_master_bus_handle_t *h) { *h = (void*)0x1234; return ESP_OK; }

static inline bool bsp_display_lock(uint32_t timeout_ms) { mock_bsp_display_locked = true; (void)timeout_ms; return true; }
static inline void bsp_display_unlock(void) { mock_bsp_display_locked = false; }
static inline void* bsp_display_start(void) { return (void*)1; }
static inline esp_err_t bsp_display_backlight_on(void) { mock_bsp_brightness = 100; return ESP_OK; }
static inline esp_err_t bsp_display_backlight_off(void) { mock_bsp_brightness = 0; return ESP_OK; }
static inline esp_err_t bsp_display_brightness_set(int pct) { mock_bsp_brightness = pct; return ESP_OK; }

static inline esp_err_t bsp_sdcard_detect_init(void) { return ESP_OK; }
static inline bool bsp_sdcard_is_present(void) { return mock_bsp_sd_present; }
static inline esp_err_t bsp_sdcard_mount(void) { return ESP_OK; }
static inline esp_err_t bsp_sdcard_unmount(void) { return ESP_OK; }

/* SD card handle stub — the simulator never actually formats anything,
 * but ui_extra.c's format BG task references the symbol and type. */
typedef struct sdmmc_card { int dummy; } sdmmc_card_t;
static inline esp_err_t bsp_get_sdcard_handle(sdmmc_card_t **card) {
    if (card) *card = NULL;
    return ESP_OK;
}

static inline esp_err_t bsp_knob_init(void) { return ESP_OK; }
static inline esp_err_t bsp_knob_register_cb(knob_event_t event, knob_cb_t cb, void *data) {
    (void)event; (void)cb; (void)data; return ESP_OK;
}

static inline esp_err_t bsp_iot_button_create(button_handle_t btn_array[], int *btn_cnt, int size) {
    for (int i = 0; i < size && i < BSP_BUTTON_NUM; i++) btn_array[i] = (void*)(uintptr_t)(i+1);
    if (btn_cnt) *btn_cnt = BSP_BUTTON_NUM;
    return ESP_OK;
}

static inline esp_err_t iot_button_register_cb(button_handle_t btn, button_event_t event, button_cb_t cb, void *data) {
    (void)btn; (void)event; (void)cb; (void)data; return ESP_OK;
}

static inline button_handle_t iot_button_create(const button_config_t *cfg) { (void)cfg; return (void*)1; }
static inline knob_handle_t iot_knob_create(const knob_config_t *cfg) { (void)cfg; return (void*)1; }
static inline esp_err_t iot_knob_register_cb(knob_handle_t knob, knob_event_t event, knob_cb_t cb, void *data) {
    (void)knob; (void)event; (void)cb; (void)data; return ESP_OK;
}
