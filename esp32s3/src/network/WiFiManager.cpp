/**
 * @file      WiFiManager.cpp
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 *
 * Dual-SSID WiFi with automatic fallback.
 */

#include "WiFiManager.h"
#include "../config/Config.h"

#ifndef NATIVE_BUILD
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"

static const char *TAG = "wifi";
static WiFiManager *s_instance = nullptr;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (!s_instance) return;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_instance->_reconnectAttempts++;
        s_instance->_state = WiFiState::CONNECTING;
        ESP_LOGW(TAG, "WiFi disconnected (attempt %lu) — will reconnect",
                 (unsigned long)s_instance->_reconnectAttempts);
        // Reconnect immediately like the working main branch code
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_instance->_ipAddress = ip_str;
        s_instance->_state = WiFiState::CONNECTED;
        s_instance->_reconnectAttempts = 0;
        // Disable power save — keeps WiFi active for HTTP server
        esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "WiFi connected, IP: %s (power save disabled)", ip_str);
        if (s_instance->_onIPCallback) {
            s_instance->_onIPCallback(ip_str);
        }
    }
}

WiFiManager::WiFiManager()
    : _state(WiFiState::DISCONNECTED)
    , _reconnectAttempts(0)
    , _backupSsid(nullptr)
    , _backupPass(nullptr)
    , _triedBackup(false) {}

WiFiManager::~WiFiManager() {}

bool WiFiManager::beginWithFallback(const char *ssid1, const char *pass1,
                                     const char *ssid2, const char *pass2) {
    _backupSsid = ssid2;
    _backupPass = pass2;
    _triedBackup = false;
    return connect(ssid1, pass1);
}

bool WiFiManager::connect(const char *ssid, const char *password) {
    _ssid = ssid;
    _password = password;
    _state = WiFiState::CONNECTING;
    _reconnectAttempts = 0;
    s_instance = this;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi connecting to %s", ssid);
    return true;
}

#else  // NATIVE_BUILD

WiFiManager::WiFiManager()
    : _state(WiFiState::DISCONNECTED), _reconnectAttempts(0),
      _backupSsid(nullptr), _backupPass(nullptr), _triedBackup(false) {}
WiFiManager::~WiFiManager() {}
bool WiFiManager::beginWithFallback(const char*, const char*, const char*, const char*) {
    _state = WiFiState::CONNECTED;
    _ipAddress = "192.168.1.100";
    return true;
}
bool WiFiManager::connect(const char*, const char*) { return true; }

#endif
