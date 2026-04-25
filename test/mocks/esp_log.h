/**
 * @brief ESP-IDF logging mock for host-based testing
 */
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <time.h>

static inline uint32_t esp_log_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE
} esp_log_level_t;

#define ESP_LOGE(tag, format, ...) fprintf(stderr, "E (%s) " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) fprintf(stderr, "W (%s) " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) fprintf(stdout, "I (%s) " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) ((void)0)
#define ESP_LOGV(tag, format, ...) ((void)0)

#define ESP_RETURN_ON_ERROR(x, tag, msg, ...) do { \
    esp_err_t __err = (x); \
    if (__err != ESP_OK) { \
        ESP_LOGE(tag, msg, ##__VA_ARGS__); \
        return __err; \
    } \
} while(0)

#define ESP_GOTO_ON_ERROR(x, goto_tag, log_tag, msg, ...) do { \
    esp_err_t __err = (x); \
    if (__err != ESP_OK) { \
        ESP_LOGE(log_tag, msg, ##__VA_ARGS__); \
        goto goto_tag; \
    } \
} while(0)
