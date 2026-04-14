/**
 * @file      Config.h
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 *
 * Compile-time defaults for Moment PIMSLO (Point-and-Shoot) Camera.
 */
#pragma once

#define MOMENT_VERSION          "0.2.0"

// Camera — highest quality JPEG, max resolution per sensor
#define CAMERA_JPEG_QUALITY     2       // Lowest = best quality (sensor may clamp internally)

// SD Card — FAT32 filesystem
#define SD_MOUNT_POINT          "/sdcard"
#define PHOTO_DIR               "/sdcard/DCIM"
#define PHOTO_PREFIX            "IMG_"

// SD Card SPI pins (Sense expansion board)
#define SD_CS_PIN               21
#define SD_SCK_PIN              7
#define SD_MISO_PIN             8
#define SD_MOSI_PIN             9

// WiFi — hardcoded credentials (primary + backup)
#define WIFI_SSID_PRIMARY       "The Garden"
#define WIFI_PASS_PRIMARY       "SanDiego2019!"
#define WIFI_SSID_BACKUP        "NYCR24"
#define WIFI_PASS_BACKUP        "clubmate"
#define WIFI_CONNECT_TIMEOUT_MS 15000

// Network
#define HTTP_SERVER_PORT        80

// Trigger — D0 (GPIO 0) external shutter trigger (active LOW)
// WARNING: GPIO 0 is a strapping pin. Must have pull-up, trigger must be
// momentary pulse (not sustained low, or chip enters download mode on reset).
#define TRIGGER_PIN             0
#define TRIGGER_DEBOUNCE_MS     100

// NeoPixel (external WS2812 on GPIO 1)
#define NEOPIXEL_PIN            1
#define LED_BRIGHTNESS          32

// OTA
#define OTA_VALID_TIMEOUT_MS    30000

// NVS
#define NVS_NAMESPACE           "moment"
