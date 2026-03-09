/**
 * @brief ESP sleep mock for host-based testing
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED,
    ESP_SLEEP_WAKEUP_ALL,
    ESP_SLEEP_WAKEUP_EXT0,
    ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER,
    ESP_SLEEP_WAKEUP_TOUCHPAD,
    ESP_SLEEP_WAKEUP_ULP,
    ESP_SLEEP_WAKEUP_GPIO,
    ESP_SLEEP_WAKEUP_UART,
} esp_sleep_source_t;

typedef enum {
    ESP_PD_DOMAIN_RTC_PERIPH,
    ESP_PD_DOMAIN_VDDSDIO,
} esp_sleep_pd_domain_t;

typedef enum {
    ESP_PD_OPTION_OFF,
    ESP_PD_OPTION_ON,
    ESP_PD_OPTION_AUTO,
} esp_sleep_pd_option_t;

static esp_sleep_source_t mock_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;

static inline void mock_set_wakeup_cause(esp_sleep_source_t cause) {
    mock_wakeup_cause = cause;
}

static inline esp_sleep_source_t esp_sleep_get_wakeup_cause(void) {
    return mock_wakeup_cause;
}

static inline esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t mask, int level) {
    (void)mask; (void)level;
    return ESP_OK;
}

static inline esp_err_t esp_sleep_pd_config(esp_sleep_pd_domain_t domain, esp_sleep_pd_option_t option) {
    (void)domain; (void)option;
    return ESP_OK;
}
