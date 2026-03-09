/**
 * @brief Display header mock
 */
#pragma once

#include "../sdkconfig.h"

/* Display resolution — same as real hardware */
#ifndef BSP_LCD_H_RES
#define BSP_LCD_H_RES 240
#endif
#ifndef BSP_LCD_V_RES
#define BSP_LCD_V_RES 240
#endif

typedef struct {
    uint32_t max_transfer_sz;
} bsp_display_config_t;
