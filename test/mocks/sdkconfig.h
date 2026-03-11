#ifndef SDKCONFIG_H
#define SDKCONFIG_H

// ESP32-P4 specific configuration defines
#define CONFIG_ESP32P4
#define CONFIG_IDF_TARGET_ESP32P4

// Display configuration
#define CONFIG_LCD_WIDTH 720
#define CONFIG_LCD_HEIGHT 720
#define CONFIG_LCD_BITS_PER_PIXEL 16

// Camera configuration
#define CONFIG_CAMERA_WIDTH 640
#define CONFIG_CAMERA_HEIGHT 480
#define CONFIG_CAMERA_PIXEL_FORMAT JPEG

// Storage configuration
#define CONFIG_STORAGE_TYPE NVS
#define CONFIG_STORAGE_PARTITION_SIZE 1048576

// GPIO configuration
#define CONFIG_BUTTON_POWER_PIN 0
#define CONFIG_BUTTON_OK_PIN 1
#define CONFIG_BUTTON_BACK_PIN 2
#define CONFIG_KNOB_PIN 3

// I2C configuration
#define CONFIG_I2C_SDA_PIN 4
#define CONFIG_I2C_SCL_PIN 5

// SD card configuration
#define CONFIG_SD_CARD_DETECT_PIN 6

// Flashlight configuration
#define CONFIG_FLASHLIGHT_PIN 7

#endif /* SDKCONFIG_H */
