/**
 * @file      main.cpp
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 *
 * PIMSLO — Point-and-shoot camera firmware.
 * Idle until D0 triggered → capture highest-quality JPEG → save to FAT32 SD.
 * HTTP API for remote capture, photo download, and OTA updates.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "config/Config.h"
#include "camera/CameraManager.h"
#include "storage/SDCardManager.h"
#include "network/WiFiManager.h"
#include "network/HttpServer.h"
#include "ota/OTAManager.h"
#include "led/StatusLED.h"
#include "spi/SPISlave.h"
#include "camera/JpegReencoder.h"

static const char *TAG = "moment";

static CameraManager  cameraManager;
static SDCardManager  sdCardManager;
static SPISlave       spiSlave;
WiFiManager           wifiManager;
static HttpServer     httpServer;
static OTAManager     otaManager;
StatusLED             statusLED;

// Photo counter persisted in NVS
static uint32_t photoCounter = 0;
static volatile bool captureRequested = false;

// Camera position (1-4) for PIMSLO parallax cropping, persisted in NVS
uint8_t cameraPosition = 0;

// Re-encoded JPEG buffer (4:2:0 for P4 HW decoder compatibility)
static uint8_t *s_reencoded_jpeg = nullptr;
static size_t   s_reencoded_len = 0;

static void loadCameraPosition() {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, NVS_KEY_CAMERA_POS, &cameraPosition);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Camera position: %d%s", cameraPosition,
             cameraPosition == 0 ? " (unset)" : "");
}

static void loadPhotoCounter() {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u32(handle, "photo_cnt", &photoCounter);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Photo counter: %lu", (unsigned long)photoCounter);
}

static void savePhotoCounter() {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u32(handle, "photo_cnt", photoCounter);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void doCapture() {
#if ENABLE_SPI_SLAVE
    if (!cameraManager.isInitialized()) {
        ESP_LOGE(TAG, "Cannot capture — camera not ready");
        return;
    }
#else
    if (!cameraManager.isInitialized() || !sdCardManager.isMounted()) {
        ESP_LOGE(TAG, "Cannot capture — camera or SD not ready");
        return;
    }
#endif

    statusLED.setState(LEDState::CAPTURING);

    /* Clear any previous JPEG data from SPI slave */
    spiSlave.clearJpegData();

    uint8_t *jpegBuf = nullptr;
    size_t jpegLen = 0;

    if (cameraManager.capturePhoto(&jpegBuf, &jpegLen)) {
#if ENABLE_SPI_SLAVE
        /* Make JPEG available for SPI transfer (don't release camera fb yet!) */
        spiSlave.setJpegData(jpegBuf, jpegLen);
        photoCounter++;
        savePhotoCounter();
        ESP_LOGI(TAG, "Photo captured: %zu bytes (available via SPI)", jpegLen);
        /* NOTE: camera fb is NOT released here — it stays valid until
         * the SPI master reads the data, then we release on next capture */
#else
        photoCounter++;
        std::string filename = sdCardManager.savePhoto(jpegBuf, jpegLen, photoCounter);
        cameraManager.releasePhoto();

        if (!filename.empty()) {
            savePhotoCounter();
            ESP_LOGI(TAG, "Photo saved: %s (%zu bytes)", filename.c_str(), jpegLen);
        } else {
            ESP_LOGE(TAG, "Failed to save photo to SD");
        }
#endif
    } else {
        ESP_LOGE(TAG, "Capture failed");
    }

    // Brief flash then back to ready
    vTaskDelay(pdMS_TO_TICKS(200));
    statusLED.setState(LEDState::READY);
}

// ─── Trigger monitoring task ────────────────────────────────

static void task_trigger_monitor(void *param) {
    // Configure D0 as input with pull-up
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << TRIGGER_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    bool lastState = true;  // Pull-up: idle = HIGH
    uint32_t lastTriggerMs = 0;

    while (true) {
        bool currentState = gpio_get_level((gpio_num_t)TRIGGER_PIN);

        // Detect falling edge (HIGH -> LOW) with debounce
        if (lastState && !currentState) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (now - lastTriggerMs > TRIGGER_DEBOUNCE_MS) {
                lastTriggerMs = now;
                ESP_LOGI(TAG, "D0 trigger detected!");
                captureRequested = true;
            }
        }

        // Also check for HTTP-triggered captures
        if (captureRequested) {
            captureRequested = false;
            doCapture();
        }

        lastState = currentState;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void task_status_led(void *param) {
    StatusLED *led = static_cast<StatusLED *>(param);
    uint32_t logCounter = 0;
    while (true) {
        led->update();
        logCounter++;
        // Print status every 10 seconds for debugging via serial
        if (logCounter % 200 == 0) {
            ESP_LOGI("status", "ip=%s wifi=%s photos=%lu sd=%s uptime=%llus",
                     wifiManager.isConnected() ? wifiManager.getIPAddress().c_str() : "NONE",
                     wifiManager.isConnected() ? "OK" : "DISCONNECTED",
                     (unsigned long)photoCounter,
                     sdCardManager.isMounted() ? "OK" : "FAIL",
                     (unsigned long long)(esp_timer_get_time() / 1000000));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ─── Entry point ────────────────────────────────────────────

extern "C" void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Moment PIMSLO v%s", MOMENT_VERSION);
    ESP_LOGI(TAG, "  Point-and-Shoot Camera");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // ─── NeoPixel ───────────────────────────────────────
    ESP_LOGI(TAG, "[1/6] NeoPixel...");
    if (statusLED.begin()) {
        statusLED.setState(LEDState::BOOTING);
        ESP_LOGI(TAG, "[1/6] NeoPixel: OK");
    }

    // ─── NVS ────────────────────────────────────────────
    ESP_LOGI(TAG, "[2/6] NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret == ESP_OK) {
        loadPhotoCounter();
        loadCameraPosition();
        ESP_LOGI(TAG, "[2/6] NVS: OK");
    } else {
        ESP_LOGE(TAG, "[2/6] NVS: FAILED (0x%x)", ret);
    }

    // ─── Camera ─────────────────────────────────────────
    ESP_LOGI(TAG, "[3/6] Camera...");
    if (cameraManager.begin()) {
        ESP_LOGI(TAG, "[3/6] Camera: %s @ %ux%u, quality=%d",
                 cameraManager.getSensorName().c_str(),
                 cameraManager.getWidth(), cameraManager.getHeight(),
                 CAMERA_JPEG_QUALITY);
    } else {
        ESP_LOGE(TAG, "[3/6] Camera: FAILED");
    }

    vTaskDelay(pdMS_TO_TICKS(200));

#if ENABLE_SPI_SLAVE
    // ─── SPI Slave (replaces SD card) ───────────────────
    ESP_LOGI(TAG, "[4/6] SPI Slave...");
    if (spiSlave.begin()) {
        ESP_LOGI(TAG, "[4/6] SPI Slave: OK (SD card disabled)");
    } else {
        ESP_LOGE(TAG, "[4/6] SPI Slave: FAILED");
    }
#else
    // ─── SD Card (FAT32) ────────────────────────────────
    ESP_LOGI(TAG, "[4/6] SD Card (FAT32)...");
    if (sdCardManager.begin()) {
        ESP_LOGI(TAG, "[4/6] SD Card: OK (free: %llu MB)",
                 sdCardManager.getFreeBytes() / (1024 * 1024));
    } else {
        ESP_LOGE(TAG, "[4/6] SD Card: FAILED");
        statusLED.setState(LEDState::NO_SD_CARD);
    }
#endif

    // ─── WiFi (dual-SSID) ───────────────────────────────
    ESP_LOGI(TAG, "[5/6] WiFi...");
    statusLED.setState(LEDState::WIFI_CONNECTING);
    wifiManager.beginWithFallback(
        WIFI_SSID_PRIMARY, WIFI_PASS_PRIMARY,
        WIFI_SSID_BACKUP, WIFI_PASS_BACKUP
    );

    // ─── HTTP + OTA ─────────────────────────────────────
    ESP_LOGI(TAG, "[6/6] HTTP server + OTA...");
    otaManager.markValid();
    httpServer.begin(&cameraManager, &sdCardManager, &otaManager);
    httpServer.onCaptureRequest([]() {
        captureRequested = true;
    });
    ESP_LOGI(TAG, "[6/6] HTTP: OK, OTA: OK");

    // ─── RTOS Tasks ─────────────────────────────────────
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Starting tasks...");

    xTaskCreatePinnedToCore(task_trigger_monitor, "Trigger",
                            4096, NULL, 10, NULL, 0);  // Core 0, lower priority
    ESP_LOGI(TAG, "  [Core 0] Trigger monitor (D0)");

    xTaskCreatePinnedToCore(task_status_led, "StatusLED",
                            4096, &statusLED, 5, NULL, 0);
    ESP_LOGI(TAG, "  [Core 0] StatusLED");

    if (cameraManager.isInitialized() && sdCardManager.isMounted()) {
        statusLED.setState(LEDState::READY);
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  PIMSLO ready! Trigger D0 to capture.");
    ESP_LOGI(TAG, "  Camera: %s", cameraManager.isInitialized() ? "OK" : "FAILED");
    ESP_LOGI(TAG, "  SD Card: %s", sdCardManager.isMounted() ? "MOUNTED" : "FAILED");
    ESP_LOGI(TAG, "  Photos taken: %lu", (unsigned long)photoCounter);
    ESP_LOGI(TAG, "  Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "========================================");
}
