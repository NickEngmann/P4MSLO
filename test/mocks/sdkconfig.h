/**
 * @brief Mock sdkconfig for host-based testing
 * Simulates ESP32-P4 configuration defaults
 */
#pragma once

#define CONFIG_IDF_TARGET_ESP32P4       1
#define CONFIG_IDF_TARGET               "esp32p4"

/* BSP Configuration */
#define CONFIG_BSP_I2C_NUM              0
#define CONFIG_BSP_SPIFFS_MOUNT_POINT   "/spiffs"
#define CONFIG_BSP_SPIFFS_PARTITION_LABEL "storage"
#define CONFIG_BSP_SPIFFS_MAX_FILES     5
#define CONFIG_BSP_SD_MOUNT_POINT       "/sdcard"
#define CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH 0
#define CONFIG_BSP_DISPLAY_LVGL_TICK    5
#define CONFIG_BSP_DISPLAY_LVGL_MAX_SLEEP 500
#define CONFIG_BSP_DISPLAY_LVGL_TASK_PRIORITY 2

/* Display */
#define BSP_LCD_H_RES   240
#define BSP_LCD_V_RES   240
#define BSP_LCD_BITS_PER_PIXEL 16
#define BSP_LCD_COLOR_SPACE 0

/* Memory */
#define CONFIG_SPIRAM    1

/* ESP painter */
#define CONFIG_ESP_PAINTER_BASIC_FONT_20  1

/* AI detect */
#define AI_DETECT_FACE       0
#define AI_DETECT_PEDESTRIAN 1
#define AI_DETECT_COCO       2
