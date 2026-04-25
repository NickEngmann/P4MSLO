/** Stub for ESP cache alignment query. Host: 64 B is a safe default. */
#pragma once
#include "esp_err.h"

static inline esp_err_t esp_cache_get_alignment(unsigned flags, size_t *out_align) {
    (void)flags; if (out_align) *out_align = 64; return ESP_OK;
}
