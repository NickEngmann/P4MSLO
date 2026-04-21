/**
 * @file app_p4_net.c
 * @brief WiFi via C6 + HTTP status + OTA endpoint.
 *
 * The hardware layer: BSP_C6_EN_PIN gates power to the C6 coprocessor. The
 * esp_wifi_remote layer (pulled in via idf_component.yml) routes calls to the
 * C6 over whatever transport esp_hosted selects in menuconfig. By default the
 * P4-EYE BSP does NOT define the esp_hosted transport pins — if WiFi doesn't
 * associate, verify the coprocessor pin config against the P4-EYE schematic.
 */

#include "app_p4_net.h"
#include "wifi_config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/rtc_io.h"
#include "mdns.h"
#include "bsp/esp32_p4_eye.h"

#ifndef P4MSLO_FIRMWARE_VERSION
#define P4MSLO_FIRMWARE_VERSION "unknown"
#endif

static const char *TAG = "p4_net";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static bool s_started = false;
static esp_netif_t *s_netif = NULL;
static httpd_handle_t s_httpd = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;
static char s_ip_str[16] = {0};

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected — retrying");
        s_ip_str[0] = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "got IP: %s", s_ip_str);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ---- HTTP handlers ---- */

static esp_err_t status_handler(httpd_req_t *req) {
    char body[256];
    uint64_t uptime_ms = esp_timer_get_time() / 1000;
    int n = snprintf(body, sizeof(body),
        "{\"firmware_version\":\"%s\","
        "\"uptime_ms\":%llu,"
        "\"free_heap\":%u,"
        "\"ip\":\"%s\","
        "\"wifi_connected\":%s}",
        P4MSLO_FIRMWARE_VERSION,
        (unsigned long long)uptime_ms,
        (unsigned)esp_get_free_heap_size(),
        s_ip_str,
        app_p4_net_is_connected() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

static esp_err_t ota_upload_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "OTA upload starting (content length=%d)", req->content_len);

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    char buf[4096];
    int received = 0, total = 0;
    while ((received = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        err = esp_ota_write(handle, buf, received);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write failed");
            return ESP_FAIL;
        }
        total += received;
    }
    if (received < 0) {
        esp_ota_abort(handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_FAIL;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ota_end failed");
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot failed");
        return ESP_FAIL;
    }

    char reply[128];
    int n = snprintf(reply, sizeof(reply),
        "{\"ok\":true,\"bytes\":%d,\"rebooting\":true}", total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, reply, n);

    ESP_LOGW(TAG, "OTA complete (%d bytes), rebooting in 500ms", total);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  // unreachable
}

static esp_err_t start_httpd(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = P4_HTTP_PORT;
    cfg.max_uri_handlers = 4;
    cfg.stack_size = 8192;
    cfg.max_open_sockets = 3;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) return err;

    httpd_uri_t status_uri = {
        .uri = "/api/v1/status", .method = HTTP_GET,
        .handler = status_handler, .user_ctx = NULL,
    };
    httpd_uri_t ota_uri = {
        .uri = "/api/v1/ota/upload", .method = HTTP_POST,
        .handler = ota_upload_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &status_uri);
    httpd_register_uri_handler(s_httpd, &ota_uri);
    return ESP_OK;
}

static void enable_c6(void) {
    /* C6 enable pin is an RTC GPIO. It may be held by deep-sleep config; the
     * BSP's bsp_p4_eye_init already sets it up with hold disabled, but call
     * rtc_gpio_hold_dis defensively so we don't fight it. */
    rtc_gpio_hold_dis(BSP_C6_EN_PIN);
    rtc_gpio_init(BSP_C6_EN_PIN);
    rtc_gpio_set_direction(BSP_C6_EN_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(BSP_C6_EN_PIN, 1);  // HIGH = enabled
    ESP_LOGI(TAG, "C6 enabled (GPIO%d HIGH)", BSP_C6_EN_PIN);
}

static void disable_c6(void) {
    rtc_gpio_set_level(BSP_C6_EN_PIN, 0);
    ESP_LOGI(TAG, "C6 disabled");
}

esp_err_t app_p4_net_start(void) {
    if (s_started) return ESP_OK;

    ESP_LOGI(TAG, "starting — SSID=\"%s\"", P4_WIFI_SSID);
    enable_c6();
    vTaskDelay(pdMS_TO_TICKS(500));  // give C6 time to boot

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (!s_netif) s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s (C6 transport likely unconfigured)",
                 esp_err_to_name(err));
        disable_c6();
        return err;
    }

    s_wifi_event_group = xEventGroupCreate();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);

    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid,     P4_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, P4_WIFI_PSK,  sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        disable_c6();
        return err;
    }

    /* Wait up to 10 s for the first IP. If it doesn't land by then, return
     * OK anyway — the event handler will keep retrying in the background. */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi up, IP=%s", s_ip_str);
    } else {
        ESP_LOGW(TAG, "WiFi not connected after 10s — will retry in background");
    }

    /* mDNS — best effort, advertises `pimslo-p4.local` */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(P4_MDNS_HOSTNAME);
        mdns_instance_name_set(P4_MDNS_INSTANCE);
        mdns_service_add(NULL, "_http", "_tcp", P4_HTTP_PORT, NULL, 0);
        ESP_LOGI(TAG, "mDNS: %s.local", P4_MDNS_HOSTNAME);
    }

    err = start_httpd();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "HTTP listening on port %d", P4_HTTP_PORT);
    }

    s_started = true;
    return ESP_OK;
}

esp_err_t app_p4_net_stop(void) {
    if (!s_started) return ESP_OK;
    ESP_LOGI(TAG, "stopping");

    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    mdns_free();

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_ip_str[0] = 0;

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    disable_c6();
    s_started = false;
    return ESP_OK;
}

bool app_p4_net_is_connected(void) {
    return s_started && s_ip_str[0] != 0;
}

void app_p4_net_get_ip(char *buf, size_t buflen) {
    if (!buf || buflen == 0) return;
    strncpy(buf, s_ip_str, buflen - 1);
    buf[buflen - 1] = 0;
}
