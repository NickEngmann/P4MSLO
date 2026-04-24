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
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
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

static const char *TAG = "moment";

#ifndef P4MSLO_FIRMWARE_VERSION
#define P4MSLO_FIRMWARE_VERSION "unknown"
#endif

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

/* ─────────── SPI-controlled WiFi / control-command subsystem ─────────────
 *
 * With DISABLE_WIFI=1 WiFi does not start at boot. The P4 master can send
 * one of four SPI control commands to toggle WiFi, reboot, or blink the LED:
 *
 *   CMD_WIFI_ON  — start WiFi + HTTP server (for OTA or debugging)
 *   CMD_WIFI_OFF — stop HTTP + WiFi (returns to silent low-power state)
 *   CMD_REBOOT   — esp_restart() after a short delay
 *   CMD_IDENTIFY — blink NeoPixel for a few seconds so user can physically
 *                  spot which camera responded (useful when 4 look the same)
 *
 * The SPI task cannot call esp_wifi_start directly (it would block the
 * master-facing transaction loop and WiFi init touches esp_netif which isn't
 * thread-safe from that context). So SPI enqueues a command and a dedicated
 * control task dequeues and executes. */
/* Control-queue message: cmd byte plus up to 8 bytes of optional payload.
 * SET_EXPOSURE carries 5 bytes (gain + exposure); the rest ignore payload. */
struct SpiControlMsg {
    uint8_t cmd;
    uint8_t payload_len;
    uint8_t payload[8];
};
static QueueHandle_t s_controlQueue = nullptr;
static bool          s_wifiStarted  = false;

static void wifi_start_subsystem() {
    if (s_wifiStarted) {
        spiSlave.setWifiStatus(true, wifiManager.isConnected());
        return;
    }
    ESP_LOGI(TAG, "[SPI control] WiFi → ON");
    statusLED.setState(LEDState::WIFI_CONNECTING);
    wifiManager.beginWithFallback(
        WIFI_SSID_PRIMARY, WIFI_PASS_PRIMARY,
        WIFI_SSID_BACKUP,  WIFI_PASS_BACKUP
    );
    otaManager.markValid();
    httpServer.begin(&cameraManager, &sdCardManager, &otaManager);
    httpServer.onCaptureRequest([]() { captureRequested = true; });
    s_wifiStarted = true;
    spiSlave.setWifiStatus(true, wifiManager.isConnected());
}

static void wifi_stop_subsystem() {
    if (!s_wifiStarted) {
        spiSlave.setWifiStatus(false, false);
        return;
    }
    ESP_LOGI(TAG, "[SPI control] WiFi → OFF");
    httpServer.stop();
    wifiManager.stop();
    s_wifiStarted = false;
    spiSlave.setWifiStatus(false, false);
    statusLED.setState(LEDState::READY);
}

static void do_identify_blink() {
    ESP_LOGI(TAG, "[SPI control] IDENTIFY — blinking for physical identification");
    for (int i = 0; i < 10; i++) {
        statusLED.setState(LEDState::CAPTURING);
        vTaskDelay(pdMS_TO_TICKS(150));
        statusLED.setState(LEDState::READY);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void refresh_exposure_header() {
    uint16_t g = 0;
    uint32_t e = 0;
    if (cameraManager.getExposure(&g, &e)) {
        spiSlave.setExposureHeader(g, e);
    }
}

static void do_autofocus() {
    ESP_LOGI(TAG, "[SPI control] AUTOFOCUS");
    spiSlave.setAfLocked(false);
    bool ok = cameraManager.autofocus(2000);
    spiSlave.setAfLocked(ok);
}

static void do_set_exposure(const uint8_t *payload, size_t len) {
    if (len < SPI_SET_EXPOSURE_PAYLOAD_LEN) {
        ESP_LOGW(TAG, "[SPI control] SET_EXPOSURE: short payload (%zu)", len);
        return;
    }
    uint16_t gain     = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    uint32_t exposure = (uint32_t)payload[2] |
                        ((uint32_t)payload[3] << 8) |
                        ((uint32_t)payload[4] << 16);
    ESP_LOGI(TAG, "[SPI control] SET_EXPOSURE gain=%u exposure=%lu",
             (unsigned)gain, (unsigned long)exposure);
    cameraManager.setExposure(gain, exposure);
    spiSlave.setExposureHeader(gain, exposure);
}

static void task_control(void *arg) {
    SpiControlMsg msg;
    while (xQueueReceive(s_controlQueue, &msg, portMAX_DELAY) == pdPASS) {
        switch (msg.cmd) {
            case SPI_CMD_WIFI_ON:      wifi_start_subsystem(); break;
            case SPI_CMD_WIFI_OFF:     wifi_stop_subsystem();  break;
            case SPI_CMD_IDENTIFY:     do_identify_blink();    break;
            case SPI_CMD_AUTOFOCUS:    do_autofocus();         break;
            case SPI_CMD_SET_EXPOSURE: do_set_exposure(msg.payload, msg.payload_len); break;
            case SPI_CMD_REBOOT:
                ESP_LOGW(TAG, "[SPI control] REBOOT requested — restarting in 100ms");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
                break;
            default:
                ESP_LOGW(TAG, "[SPI control] unknown cmd 0x%02X", msg.cmd);
                break;
        }
    }
}

/* Periodically syncs the WiFi-connected flag in the SPI status byte (master
 * polls this to learn when WiFi finished associating). */
static void task_wifi_status_sync(void *arg) {
    bool lastConnected = false;
    while (true) {
        bool c = s_wifiStarted && wifiManager.isConnected();
        if (c != lastConnected) {
            spiSlave.setWifiStatus(s_wifiStarted, c);
            lastConnected = c;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
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
        /* Snapshot the freshly-settled AE state into the IDLE header so a
         * PIMSLO master can read this camera's exposure via status poll. */
        refresh_exposure_header();
        /* Hot path: one per capture. Demoted to DEBUG so steady-state
         * capture cycles don't spam the CDC. Enable with
         * esp_log_level_set("main", ESP_LOG_DEBUG) when needed. */
        ESP_LOGD(TAG, "Photo captured: %zu bytes (available via SPI)", jpegLen);
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

// ─── Trigger monitoring (ISR-driven) ────────────────────────
//
// Was a 10 ms-poll loop with edge detection. Failure mode: if the
// trigger monitor task got preempted for longer than the P4's 250 ms
// pulse width (camera driver, SD writes, and the SPI slave task at
// prio 10 can all push it out), the falling edge happened entirely
// between polls and lastState stayed HIGH → trigger missed. That
// showed up as random camera drop-outs in 4/4 capture runs.
//
// Now: GPIO ISR on the falling edge gives a semaphore. The task just
// waits on the semaphore. ISR latency is ~us regardless of what
// other tasks are doing, so a missed trigger is no longer possible
// short of a genuine hang. Debounce moves into the task (after the
// semaphore take) so the ISR itself stays short.

static SemaphoreHandle_t s_trigger_sem = nullptr;

static void IRAM_ATTR trigger_isr(void *arg) {
    BaseType_t higher = pdFALSE;
    xSemaphoreGiveFromISR(s_trigger_sem, &higher);
    if (higher == pdTRUE) portYIELD_FROM_ISR();
}

static void task_trigger_monitor(void *param) {
    // Configure D0 as input with pull-up, interrupt on falling edge.
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << TRIGGER_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&io_conf);

    s_trigger_sem = xSemaphoreCreateBinary();
    configASSERT(s_trigger_sem);

    // install_isr_service is idempotent — if some other subsystem
    // already installed it (shouldn't happen here but be defensive),
    // the ESP_ERR_INVALID_STATE return is harmless.
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add((gpio_num_t)TRIGGER_PIN, trigger_isr, nullptr);

    uint32_t lastTriggerMs = 0;

    while (true) {
        // Block here until either an edge fires the ISR OR an HTTP
        // capture request comes in. 50 ms poll period is for the HTTP
        // path (flag set by captureRequested=true); the ISR wakes us
        // within microseconds of the edge regardless.
        if (xSemaphoreTake(s_trigger_sem, pdMS_TO_TICKS(50)) == pdTRUE) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (now - lastTriggerMs > TRIGGER_DEBOUNCE_MS) {
                lastTriggerMs = now;
                ESP_LOGI(TAG, "D0 trigger detected (ISR)");
                captureRequested = true;
            }
        }

        if (captureRequested) {
            captureRequested = false;
            doCapture();
        }
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
    ESP_LOGI(TAG, "  Moment PIMSLO v%s  (%s)", MOMENT_VERSION, P4MSLO_FIRMWARE_VERSION);
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

        // Control-command plumbing: SPI task enqueues, control task executes
        s_controlQueue = xQueueCreate(4, sizeof(SpiControlMsg));
        spiSlave.setControlCallback([](uint8_t cmd, const uint8_t *payload, size_t payload_len) {
            if (!s_controlQueue) return;
            SpiControlMsg msg = {};
            msg.cmd = cmd;
            msg.payload_len = (uint8_t)(payload_len > sizeof(msg.payload) ? sizeof(msg.payload) : payload_len);
            if (payload && msg.payload_len) memcpy(msg.payload, payload, msg.payload_len);
            xQueueSend(s_controlQueue, &msg, 0);
        });

        /* Visual indicator: white NeoPixel for the duration of the actual
         * SPI data transfer to the P4. This is the "I see the trigger +
         * I'm shipping the bytes" signal the user asked for. Fires from
         * the SPI task context — setState() is just a uint write, no
         * blocking, safe to call inline. */
        spiSlave.setTransferStartCallback([]() {
            statusLED.setState(LEDState::CAPTURING);
        });
        spiSlave.setTransferEndCallback([]() {
            statusLED.setState(LEDState::READY);
        });
        xTaskCreatePinnedToCore(task_control, "SpiCtrl",
                                8192, nullptr, 5, nullptr, 0);
        xTaskCreatePinnedToCore(task_wifi_status_sync, "WiFiSync",
                                2048, nullptr, 2, nullptr, 0);
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

#if DISABLE_WIFI
    ESP_LOGI(TAG, "[5/6] WiFi: OFF by default — send SPI CMD_WIFI_ON to enable");
    ESP_LOGI(TAG, "[6/6] HTTP: not started (starts when WiFi does)");
#else
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
#endif

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
