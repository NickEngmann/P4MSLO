/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "bsp/esp-bsp.h"

#include "ui_extra.h"

#include "app_control.h"
#include "app_video_stream.h"
#include "app_storage.h"
#include "app_ai_detect.h"
#include "app_qma6100.h"
#include "app_serial_cmd.h"
#include "app_pimslo.h"
#include "app_gifs.h"

static const char *TAG = "main";

#ifndef P4MSLO_FIRMWARE_VERSION
#define P4MSLO_FIRMWARE_VERSION "unknown"
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  P4MSLO firmware %s", P4MSLO_FIRMWARE_VERSION);
    ESP_LOGI(TAG, "========================================");

    // Initialize NVS
    ESP_LOGI(TAG, "Initialize NVS");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Set default system time to 2026-01-01 (no RTC, files get 1980 otherwise)
    struct timeval tv = { .tv_sec = 1767225600 };  /* 2026-01-01 00:00:00 UTC */
    settimeofday(&tv, NULL);

    // Initialize the flashlight
    ESP_LOGI(TAG, "Initialize the flashlight");
    ESP_ERROR_CHECK(bsp_flashlight_init());

    // Initialize the I2C first (needed for QMA6100)
    ESP_LOGI(TAG, "Initialize the I2C");
    i2c_master_bus_handle_t i2c_handle;
    ESP_ERROR_CHECK(bsp_i2c_init());
    bsp_get_i2c_bus_handle(&i2c_handle);

    // AI detection models disabled at boot to save ~10MB PSRAM.
    // Can be initialized on-demand via app_ai_detect_init() when needed.
    // ESP_ERROR_CHECK(app_ai_detect_init());

    // Initialize the display
    ESP_LOGI(TAG, "Initialize the display");
    bsp_display_start();

    bsp_display_lock(0);
    ui_extra_init();
    bsp_display_unlock();

    // Initialize the QMA6100 IMU sensor with integrated display auto-rotation
    ESP_LOGI(TAG, "Initialize the QMA6100 IMU sensor with display auto-rotation");
    ret = app_qma6100_init(i2c_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize QMA6100: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize the storage
    ESP_LOGI(TAG, "Initialize the storage");
    ESP_ERROR_CHECK(app_storage_init());

    // Turn on the display backlight
    bsp_display_backlight_on();

    // Initialize the application control module
    ESP_LOGI(TAG, "Initialize the application control module");
    ESP_ERROR_CHECK(app_control_init());

    /* Initialize the video streaming application.
     *
     * app_video_stream_init talks to the P4-EYE's built-in MIPI-CSI
     * camera (OV2710) over SCCB/I2C. If the camera's ribbon cable is
     * loose or its rail is down, the SCCB probe fails. Previously this
     * path used ESP_ERROR_CHECK → abort → reboot loop. That's too
     * aggressive: the rest of the device (gallery, settings, SPI rig)
     * is still useful without the P4 viewfinder. Log and continue; the
     * UI will just show a black camera canvas. */
    ESP_LOGI(TAG, "Initialize the video streaming application");
    esp_err_t video_ret = app_video_stream_init(i2c_handle);
    if (video_ret != ESP_OK) {
        ESP_LOGE(TAG, "Video stream init FAILED (0x%x) — camera viewfinder "
                      "will be unavailable. Check the MIPI-CSI ribbon to the "
                      "P4-EYE camera sensor. Continuing boot.", video_ret);
    }
    
    // Initialize PIMSLO background capture + GIF pipeline
    ESP_LOGI(TAG, "Initialize PIMSLO subsystem");
    app_pimslo_init();

    // Initialize serial command interface for automated testing
    ESP_LOGI(TAG, "Initialize serial command interface");
    app_serial_cmd_init();

    // Start the gallery background worker. It pre-renders .p4ms files for
    // any .gif that lacks one, then re-encodes any stale PIMSLO captures
    // whose .gif was never produced (e.g. reboot mid-encode). Idles when
    // the user is on the gallery or PIMSLO is actively capturing/encoding.
    app_gifs_start_background_worker();

    ESP_LOGI(TAG, "Application initialization completed");
}