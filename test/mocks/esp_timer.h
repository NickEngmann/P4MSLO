/**
 * @brief ESP timer mock for host-based testing
 */
#pragma once

#include <stdint.h>

static int64_t mock_timer_value = 0;

static inline void mock_timer_set(int64_t value_us) {
    mock_timer_value = value_us;
}

static inline void mock_timer_advance(int64_t delta_us) {
    mock_timer_value += delta_us;
}

static inline int64_t esp_timer_get_time(void) {
    return mock_timer_value;
}
