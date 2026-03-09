/**
 * @brief BSP error check macros mock
 */
#pragma once

#include "../esp_err.h"
#include "../esp_log.h"

#define BSP_ERROR_CHECK_RETURN_ERR(x) do { \
    esp_err_t __err = (x); \
    if (__err != ESP_OK) return __err; \
} while(0)

#define BSP_ERROR_CHECK_RETURN_NULL(x) do { \
    esp_err_t __err = (x); \
    if (__err != ESP_OK) return NULL; \
} while(0)

#define BSP_NULL_CHECK(ptr, ret) do { \
    if ((ptr) == NULL) return ret; \
} while(0)
