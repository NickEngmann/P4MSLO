/**
 * @file      OTAManager.cpp
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 *
 * OTA firmware update. Downloads from HTTP/HTTPS URL and flashes.
 */

#include "OTAManager.h"
#include "../config/Config.h"

#ifndef NATIVE_BUILD
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

OTAManager::OTAManager() : _state(OTAState::IDLE) {}
OTAManager::~OTAManager() {}

void OTAManager::markValid() {
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "Firmware marked valid");
}

// OTA runs in a separate task to avoid blocking the HTTP response
static void ota_task(void *param) {
    std::string *url = (std::string *)param;
    ESP_LOGI(TAG, "Starting OTA download from: %s", url->c_str());

    esp_http_client_config_t http_config = {};
    http_config.url = url->c_str();
    http_config.timeout_ms = 30000;

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        delete url;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: 0x%x", err);
        esp_http_client_cleanup(client);
        delete url;
        vTaskDelete(NULL);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "Firmware size: %d bytes", content_length);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition available");
        esp_http_client_cleanup(client);
        delete url;
        vTaskDelete(NULL);
        return;
    }

    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: 0x%x", err);
        esp_http_client_cleanup(client);
        delete url;
        vTaskDelete(NULL);
        return;
    }

    // Download and write in 4KB chunks
    uint8_t *buf = (uint8_t *)malloc(4096);
    int total_read = 0;
    while (true) {
        int read_len = esp_http_client_read(client, (char *)buf, 4096);
        if (read_len <= 0) break;

        err = esp_ota_write(ota_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: 0x%x", err);
            break;
        }
        total_read += read_len;

        if (total_read % (64 * 1024) == 0) {
            ESP_LOGI(TAG, "OTA progress: %d bytes", total_read);
        }
    }
    free(buf);
    esp_http_client_cleanup(client);

    if (err == ESP_OK) {
        err = esp_ota_end(ota_handle);
        if (err == ESP_OK) {
            err = esp_ota_set_boot_partition(update_partition);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "OTA complete (%d bytes). Rebooting...", total_read);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
        }
    }

    ESP_LOGE(TAG, "OTA failed: 0x%x", err);
    delete url;
    vTaskDelete(NULL);
}

void OTAManager::startUpdateFromURL(const std::string &url) {
    if (_state == OTAState::DOWNLOADING) {
        ESP_LOGW(TAG, "OTA already in progress");
        return;
    }
    _state = OTAState::DOWNLOADING;

    // Pass URL to task (heap-allocated, task frees it)
    std::string *urlCopy = new std::string(url);
    xTaskCreate(ota_task, "OTA", 8192, urlCopy, 5, NULL);
}

#else  // NATIVE_BUILD

OTAManager::OTAManager() : _state(OTAState::IDLE) {}
OTAManager::~OTAManager() {}
void OTAManager::markValid() {}
void OTAManager::startUpdateFromURL(const std::string &) {}

#endif
