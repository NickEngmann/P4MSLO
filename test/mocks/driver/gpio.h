/**
 * @brief GPIO driver mock for host-based testing
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../esp_err.h"

typedef int gpio_num_t;
typedef int gpio_mode_t;
typedef int gpio_pullup_t;
typedef int gpio_pulldown_t;
typedef int gpio_int_type_t;

#define GPIO_NUM_NC   (-1)
#define GPIO_NUM_0    0
#define GPIO_NUM_1    1
#define GPIO_NUM_2    2
#define GPIO_NUM_3    3
#define GPIO_NUM_4    4
#define GPIO_NUM_5    5
#define GPIO_NUM_9    9
#define GPIO_NUM_11   11
#define GPIO_NUM_12   12
#define GPIO_NUM_13   13
#define GPIO_NUM_14   14
#define GPIO_NUM_15   15
#define GPIO_NUM_16   16
#define GPIO_NUM_17   17
#define GPIO_NUM_18   18
#define GPIO_NUM_19   19
#define GPIO_NUM_20   20
#define GPIO_NUM_21   21
#define GPIO_NUM_22   22
#define GPIO_NUM_23   23
#define GPIO_NUM_26   26
#define GPIO_NUM_45   45
#define GPIO_NUM_46   46
#define GPIO_NUM_47   47
#define GPIO_NUM_48   48

#define GPIO_MODE_INPUT       0x01
#define GPIO_MODE_OUTPUT      0x02
#define GPIO_PULLUP_DISABLE   0
#define GPIO_PULLUP_ENABLE    1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_PULLDOWN_ENABLE  1
#define GPIO_INTR_DISABLE     0

#define BIT(x) (1 << (x))
#define BIT64(x) (1ULL << (x))

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

/* Mock GPIO state tracking */
#define MOCK_GPIO_MAX_PINS 64

static int mock_gpio_levels[MOCK_GPIO_MAX_PINS] = {0};
static gpio_mode_t mock_gpio_modes[MOCK_GPIO_MAX_PINS] = {0};

static inline void mock_gpio_reset(void) {
    for (int i = 0; i < MOCK_GPIO_MAX_PINS; i++) {
        mock_gpio_levels[i] = 0;
        mock_gpio_modes[i] = 0;
    }
}

static inline esp_err_t gpio_config(const gpio_config_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < MOCK_GPIO_MAX_PINS; i++) {
        if (cfg->pin_bit_mask & BIT64(i)) {
            mock_gpio_modes[i] = cfg->mode;
        }
    }
    return ESP_OK;
}

static inline esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level) {
    if (gpio_num < 0 || gpio_num >= MOCK_GPIO_MAX_PINS) return ESP_ERR_INVALID_ARG;
    mock_gpio_levels[gpio_num] = level;
    return ESP_OK;
}

static inline int gpio_get_level(gpio_num_t gpio_num) {
    if (gpio_num < 0 || gpio_num >= MOCK_GPIO_MAX_PINS) return 0;
    return mock_gpio_levels[gpio_num];
}
