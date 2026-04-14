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

// Camera — JPEG quality (lower = higher quality, 2-63)
// Quality 4 = near-lossless from the OV3660 sensor
#define CAMERA_JPEG_QUALITY     4

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

// Trigger — D0 (GPIO 1) external shutter trigger (active LOW)
// D0 on XIAO ESP32-S3 = GPIO 1 (not GPIO 0!)
#define TRIGGER_PIN             1
#define TRIGGER_DEBOUNCE_MS     100

// NeoPixel (external WS2812 on GPIO 1)
#define NEOPIXEL_PIN            1
#define LED_BRIGHTNESS          32

// OTA
#define OTA_VALID_TIMEOUT_MS    30000

// SPI Slave — shares pins with SD card (SD must be disabled)
#define SPI_SLAVE_CLK_PIN       7
#define SPI_SLAVE_MOSI_PIN      9   // Master→Slave (commands from P4)
#define SPI_SLAVE_MISO_PIN      8   // Slave→Master (JPEG data to P4)
#define SPI_SLAVE_CS_PIN        2
#define SPI_SLAVE_DMA_CHAN      SPI_DMA_CH_AUTO

// Set to 1 to enable SPI slave mode (disables SD card)
#define ENABLE_SPI_SLAVE        1

// NVS
#define NVS_NAMESPACE           "moment"
#define NVS_KEY_CAMERA_POS      "cam_pos"   // Camera position 1-4 (0 = unset)
