/**
 * @file      CameraManager.cpp
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 *
 * Auto-detects OV5640 (5MP) or OV3660 (3MP) and captures at max resolution.
 */

#include "CameraManager.h"
#include "../config/Config.h"

#ifndef NATIVE_BUILD
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camera";

CameraManager::CameraManager()
    : _initialized(false), _width(0), _height(0), _fb(nullptr) {}

CameraManager::~CameraManager() {
    stop();
}

bool CameraManager::begin() {
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = 15;
    config.pin_d1 = 17;
    config.pin_d2 = 18;
    config.pin_d3 = 16;
    config.pin_d4 = 14;
    config.pin_d5 = 12;
    config.pin_d6 = 11;
    config.pin_d7 = 48;
    config.pin_xclk = 10;
    config.pin_pclk = 13;
    config.pin_vsync = 38;
    config.pin_href = 47;
    config.pin_sccb_sda = 40;
    config.pin_sccb_scl = 39;
    config.pin_pwdn = -1;
    config.pin_reset = -1;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    // Start with max resolution — will be adjusted after sensor detection
    config.frame_size = FRAMESIZE_QSXGA;  // 2592x1944 (OV5640 max)
    config.jpeg_quality = CAMERA_JPEG_QUALITY;
    /* fb_count=2 gives the DMA a slack buffer, preventing NULL returns from
     * esp_camera_fb_get() when timing jitters (WiFi activity, thermal drift).
     * Documented fix for esp32-camera issue #620. */
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    /* OV5640 datasheet requires ≥20ms after VDD stable before SCCB is reliable.
     * Without pwdn/reset pins on the XIAO Sense, we rely on this delay alone. */
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "Initializing camera...");
    esp_err_t err = esp_camera_init(&config);

    if (err != ESP_OK) {
        // If QSXGA fails, try QXGA (OV3660 max)
        ESP_LOGW(TAG, "QSXGA init failed (0x%x), trying QXGA...", err);
        config.frame_size = FRAMESIZE_QXGA;  // 2048x1536
        err = esp_camera_init(&config);
    }

    if (err != ESP_OK) {
        // Last resort: try 720p
        ESP_LOGW(TAG, "QXGA init failed (0x%x), trying 720p...", err);
        config.frame_size = FRAMESIZE_HD;  // 1280x720
        err = esp_camera_init(&config);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
        return false;
    }

    // Detect sensor
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        uint16_t pid = sensor->id.PID;
        if (pid == 0x5640) {
            _sensorName = "OV5640";
            ESP_LOGI(TAG, "OV5640 detected (5MP, autofocus)");
        } else if (pid == 0x3660) {
            _sensorName = "OV3660";
            ESP_LOGI(TAG, "OV3660 detected (3MP)");
            // OV3660 max is QXGA, not QSXGA
            if (config.frame_size == FRAMESIZE_QSXGA) {
                sensor->set_framesize(sensor, FRAMESIZE_QXGA);
            }
        } else {
            _sensorName = "Unknown";
            ESP_LOGW(TAG, "Unknown sensor PID: 0x%04x", pid);
        }
        /* Note: jpeg_quality is already set via config.jpeg_quality in
         * esp_camera_init() — don't re-apply via sensor->set_quality(),
         * which can leave the sensor in a transient state under thermal stress. */
    }

    // Test capture to get actual dimensions
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        _width = fb->width;
        _height = fb->height;
        ESP_LOGI(TAG, "Test capture: %ux%u, %zu bytes", _width, _height, fb->len);
        esp_camera_fb_return(fb);
    }

    _initialized = true;
    ESP_LOGI(TAG, "Camera ready: %s @ %ux%u, JPEG quality=%d",
             _sensorName.c_str(), _width, _height, CAMERA_JPEG_QUALITY);
    return true;
}

void CameraManager::stop() {
    if (_initialized) {
        releasePhoto();
        esp_camera_deinit();
        _initialized = false;
    }
}

bool CameraManager::capturePhoto(uint8_t **outBuf, size_t *outLen) {
    if (!_initialized) return false;

    // Release any previously held frame
    releasePhoto();

    uint64_t t0 = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    uint64_t t1 = esp_timer_get_time();

    if (!fb) {
        ESP_LOGE(TAG, "Capture failed");
        return false;
    }

    if (fb->len < 2 || fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
        ESP_LOGE(TAG, "Not JPEG data");
        esp_camera_fb_return(fb);
        return false;
    }

    _fb = fb;
    *outBuf = fb->buf;
    *outLen = fb->len;

    ESP_LOGI(TAG, "Captured %ux%u JPEG: %zu bytes in %llums",
             fb->width, fb->height, fb->len, (t1 - t0) / 1000);
    return true;
}

void CameraManager::releasePhoto() {
    if (_fb) {
        esp_camera_fb_return((camera_fb_t *)_fb);
        _fb = nullptr;
    }
}

#else  // NATIVE_BUILD

CameraManager::CameraManager() : _initialized(false), _width(0), _height(0) {}
CameraManager::~CameraManager() {}
bool CameraManager::begin() { return false; }
void CameraManager::stop() {}
bool CameraManager::capturePhoto(uint8_t**, size_t*) { return false; }
void CameraManager::releasePhoto() {}

#endif
