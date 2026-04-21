/**
 * @file      HttpServer.cpp
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 *
 * Point-and-shoot REST API.
 */

#include "HttpServer.h"
#include "../config/Config.h"
#include "../storage/SDCardManager.h"
#include "../camera/CameraManager.h"
#include "../ota/OTAManager.h"

#ifndef NATIVE_BUILD
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "esp_ota_ops.h"

static const char *TAG = "httpd";

#define GET_SERVER(req) ((HttpServer *)(req)->user_ctx)

static esp_err_t send_json(httpd_req_t *req, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t send_error(httpd_req_t *req, int code, const char *msg) {
    const char *status = code == 400 ? "400 Bad Request" :
                         code == 404 ? "404 Not Found" :
                         code == 503 ? "503 Service Unavailable" :
                         "500 Internal Server Error";
    httpd_resp_set_status(req, status);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    return send_json(req, root);
}

HttpServer::HttpServer()
    : _camera(nullptr), _sd(nullptr), _ota(nullptr), _running(false)
#ifndef NATIVE_BUILD
    , _serverHandle(NULL)
#endif
{}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::begin(CameraManager *cam, SDCardManager *sd, OTAManager *ota) {
    _camera = cam;
    _sd = sd;
    _ota = ota;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_SERVER_PORT;
    config.max_uri_handlers = 12;
    config.stack_size = 16384;
    config.recv_wait_timeout = 30;   // OTA uploads need time
    config.send_wait_timeout = 30;  // Large photos need time to send
    config.max_open_sockets = 4;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&_serverHandle, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: 0x%x", err);
        return false;
    }

    if (!registerRoutes()) {
        httpd_stop(_serverHandle);
        return false;
    }

    _running = true;
    ESP_LOGI(TAG, "HTTP server started on port %d", HTTP_SERVER_PORT);
    return true;
}

void HttpServer::stop() {
    if (_running && _serverHandle) {
        httpd_stop(_serverHandle);
        _serverHandle = NULL;
        _running = false;
    }
}

bool HttpServer::registerRoutes() {
    httpd_uri_t routes[] = {
        { "/api/v1/status",   HTTP_GET,  handleStatus,     this },
        { "/api/v1/capture",  HTTP_POST, handleCapture,    this },
        { "/api/v1/photos",   HTTP_GET,  handleListPhotos, this },
        { "/api/v1/photo/*",  HTTP_GET,  handleGetPhoto,   this },
        { "/api/v1/ota",          HTTP_POST, handleOTA,          this },
        { "/api/v1/ota/upload",   HTTP_POST, handleOTAUpload,    this },
        { "/api/v1/sd/format",    HTTP_POST, handleSDFormat,     this },
        { "/api/v1/factory-reset",HTTP_POST, handleFactoryReset, this },
        { "/api/v1/config/position", HTTP_POST, handleSetPosition, this },
        { "/api/v1/latest-photo",    HTTP_GET,  handleGetLatestPhoto, this },
        { "/api/v1/reboot",       HTTP_POST, handleReboot,       this },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(_serverHandle, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: 0x%x", routes[i].uri, err);
            return false;
        }
    }
    return true;
}

// ─── GET /api/v1/status ─────────────────────────────────────

esp_err_t HttpServer::handleStatus(httpd_req_t *req) {
    HttpServer *srv = GET_SERVER(req);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", MOMENT_VERSION);
#ifdef P4MSLO_FIRMWARE_VERSION
    cJSON_AddStringToObject(root, "firmware_version", P4MSLO_FIRMWARE_VERSION);
#endif
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

    if (srv->_camera && srv->_camera->isInitialized()) {
        cJSON_AddBoolToObject(root, "camera_ok", true);
        cJSON_AddStringToObject(root, "sensor", srv->_camera->getSensorName().c_str());
        cJSON_AddNumberToObject(root, "resolution_w", srv->_camera->getWidth());
        cJSON_AddNumberToObject(root, "resolution_h", srv->_camera->getHeight());
    } else {
        cJSON_AddBoolToObject(root, "camera_ok", false);
    }

    if (srv->_sd && srv->_sd->isMounted()) {
        cJSON_AddBoolToObject(root, "sd_ok", true);
        cJSON_AddNumberToObject(root, "sd_free_mb", (double)(srv->_sd->getFreeBytes() / (1024 * 1024)));
        cJSON_AddNumberToObject(root, "sd_total_mb", (double)(srv->_sd->getTotalBytes() / (1024 * 1024)));
        auto photos = srv->_sd->listPhotos();
        cJSON_AddNumberToObject(root, "photo_count", (double)photos.size());
    } else {
        cJSON_AddBoolToObject(root, "sd_ok", false);
    }

    // PIMSLO camera position (1-4, 0=unset)
    extern uint8_t cameraPosition;
    cJSON_AddNumberToObject(root, "camera_position", (double)cameraPosition);

    return send_json(req, root);
}

// ─── POST /api/v1/capture ───────────────────────────────────

esp_err_t HttpServer::handleCapture(httpd_req_t *req) {
    HttpServer *srv = GET_SERVER(req);

    if (srv->_captureCallback) {
        srv->_captureCallback();

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", "capture_triggered");
        return send_json(req, root);
    }

    return send_error(req, 503, "Capture not available");
}

// ─── GET /api/v1/photos ─────────────────────────────────────

esp_err_t HttpServer::handleListPhotos(httpd_req_t *req) {
    HttpServer *srv = GET_SERVER(req);
    if (!srv->_sd || !srv->_sd->isMounted()) {
        return send_error(req, 503, "SD card not mounted");
    }

    auto photos = srv->_sd->listPhotos();

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (const auto &name : photos) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(name.c_str()));
    }
    cJSON_AddItemToObject(root, "photos", arr);
    cJSON_AddNumberToObject(root, "count", (double)photos.size());
    return send_json(req, root);
}

// ─── GET /api/v1/photo/<filename> ───────────────────────────

esp_err_t HttpServer::handleGetPhoto(httpd_req_t *req) {
    HttpServer *srv = GET_SERVER(req);
    if (!srv->_sd || !srv->_sd->isMounted()) {
        return send_error(req, 503, "SD card not mounted");
    }

    // Extract filename from URI: /api/v1/photo/IMG_0001.jpg
    const char *uri = req->uri;
    const char *prefix = "/api/v1/photo/";
    if (strncmp(uri, prefix, strlen(prefix)) != 0) {
        return send_error(req, 400, "Invalid photo path");
    }
    const char *filename = uri + strlen(prefix);
    if (strlen(filename) == 0 || strlen(filename) > 64) {
        return send_error(req, 400, "Invalid filename");
    }

    std::string path = srv->_sd->getPhotoPath(filename);
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return send_error(req, 404, "Photo not found");
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    httpd_resp_set_type(req, "image/jpeg");

    // Stream in 8KB chunks
    const size_t CHUNK_SIZE = 8192;
    uint8_t *chunk = (uint8_t *)malloc(CHUNK_SIZE);
    if (!chunk) {
        fclose(f);
        return send_error(req, 500, "Out of memory");
    }

    ESP_LOGI(TAG, "Serving %s (%ld bytes)", filename, fileSize);

    while (true) {
        size_t n = fread(chunk, 1, CHUNK_SIZE, f);
        if (n == 0) break;
        esp_err_t err = httpd_resp_send_chunk(req, (const char *)chunk, n);
        if (err != ESP_OK) break;
    }

    free(chunk);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ─── POST /api/v1/ota ───────────────────────────────────────

esp_err_t HttpServer::handleOTA(httpd_req_t *req) {
    HttpServer *srv = GET_SERVER(req);

    char body[256] = {};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        return send_error(req, 400, "Empty body");
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_error(req, 400, "Invalid JSON");
    }

    cJSON *urlItem = cJSON_GetObjectItem(root, "url");
    if (!urlItem || !urlItem->valuestring) {
        cJSON_Delete(root);
        return send_error(req, 400, "Missing 'url' field");
    }

    std::string url(urlItem->valuestring);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "OTA requested from: %s", url.c_str());

    if (srv->_ota) {
        srv->_ota->startUpdateFromURL(url);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ota_started");
    cJSON_AddStringToObject(resp, "url", url.c_str());
    return send_json(req, resp);
}

// ─── POST /api/v1/ota/upload ────────────────────────────────
// Receives firmware binary directly in the POST body and flashes it.
// Usage: curl -X POST http://<ip>/api/v1/ota/upload --data-binary @firmware.bin

esp_err_t HttpServer::handleOTAUpload(httpd_req_t *req) {
    ESP_LOGI(TAG, "OTA upload: receiving %d bytes", req->content_len);

    if (req->content_len <= 0 || req->content_len > 2 * 1024 * 1024) {
        return send_error(req, 400, "Invalid firmware size (must be 1B - 2MB)");
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        return send_error(req, 500, "No OTA partition available");
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: 0x%x", err);
        return send_error(req, 500, "OTA begin failed");
    }

    // Receive and write in 4KB chunks
    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        return send_error(req, 500, "Out of memory");
    }

    int total_received = 0;
    bool write_ok = true;
    while (total_received < req->content_len) {
        int to_read = req->content_len - total_received;
        if (to_read > 4096) to_read = 4096;

        int received = httpd_req_recv(req, (char *)buf, to_read);
        if (received <= 0) {
            ESP_LOGE(TAG, "OTA recv failed at %d bytes", total_received);
            write_ok = false;
            break;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed at %d bytes: 0x%x", total_received, err);
            write_ok = false;
            break;
        }

        total_received += received;
        if (total_received % (64 * 1024) == 0 || total_received == req->content_len) {
            ESP_LOGI(TAG, "OTA progress: %d / %d bytes (%d%%)",
                     total_received, req->content_len,
                     total_received * 100 / req->content_len);
        }
    }
    free(buf);

    if (!write_ok) {
        esp_ota_abort(ota_handle);
        return send_error(req, 500, "OTA write failed");
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: 0x%x", err);
        return send_error(req, 500, "OTA verification failed");
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: 0x%x", err);
        return send_error(req, 500, "Failed to set boot partition");
    }

    ESP_LOGI(TAG, "OTA upload complete: %d bytes. Rebooting...", total_received);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ota_complete");
    cJSON_AddNumberToObject(resp, "bytes", total_received);
    cJSON_AddStringToObject(resp, "message", "Firmware flashed. Rebooting in 2 seconds...");
    esp_err_t ret = send_json(req, resp);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ret;
}

// ─── POST /api/v1/sd/format ─────────────────────────────────

esp_err_t HttpServer::handleSDFormat(httpd_req_t *req) {
    char body[64] = {};
    httpd_req_recv(req, body, sizeof(body) - 1);
    cJSON *root = cJSON_Parse(body);
    if (!root || !cJSON_IsTrue(cJSON_GetObjectItem(root, "confirm"))) {
        if (root) cJSON_Delete(root);
        return send_error(req, 400, "Send {\"confirm\": true} to format SD card");
    }
    cJSON_Delete(root);

    // TODO: unmount, format, remount. For now just acknowledge.
    ESP_LOGW("httpd", "SD format requested — not yet implemented for FAT32");
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "format_not_yet_implemented");
    return send_json(req, resp);
}

// ─── POST /api/v1/factory-reset ─────────────────────────────

esp_err_t HttpServer::handleFactoryReset(httpd_req_t *req) {
    char body[64] = {};
    httpd_req_recv(req, body, sizeof(body) - 1);
    cJSON *root = cJSON_Parse(body);
    if (!root || !cJSON_IsTrue(cJSON_GetObjectItem(root, "confirm"))) {
        if (root) cJSON_Delete(root);
        return send_error(req, 400, "Send {\"confirm\": true} to factory reset");
    }
    cJSON_Delete(root);

    // Clear NVS
    nvs_flash_erase();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "reset");
    cJSON_AddStringToObject(resp, "message", "NVS cleared. Rebooting...");
    esp_err_t ret = send_json(req, resp);

    ESP_LOGW("httpd", "Factory reset — rebooting in 2 seconds");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ret;
}

// ─── POST /api/v1/config/position ──────────────────────────

esp_err_t HttpServer::handleSetPosition(httpd_req_t *req) {
    char body[128] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return send_error(req, 400, "No body");

    cJSON *json = cJSON_Parse(body);
    if (!json) return send_error(req, 400, "Invalid JSON");

    cJSON *pos = cJSON_GetObjectItem(json, "position");
    if (!pos || !cJSON_IsNumber(pos) || pos->valueint < 1 || pos->valueint > 4) {
        cJSON_Delete(json);
        return send_error(req, 400, "position must be 1-4");
    }

    extern uint8_t cameraPosition;
    cameraPosition = (uint8_t)pos->valueint;

    // Persist to NVS
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, NVS_KEY_CAMERA_POS, cameraPosition);
        nvs_commit(handle);
        nvs_close(handle);
    }

    cJSON_Delete(json);
    ESP_LOGI(TAG, "Camera position set to %d", cameraPosition);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNumberToObject(root, "position", (double)cameraPosition);
    return send_json(req, root);
}

// ─── GET /api/v1/latest-photo ──────────────────────────────

esp_err_t HttpServer::handleGetLatestPhoto(httpd_req_t *req) {
    HttpServer *srv = GET_SERVER(req);

    if (!srv->_sd || !srv->_sd->isMounted())
        return send_error(req, 503, "SD card not ready");

    auto photos = srv->_sd->listPhotos();
    if (photos.empty())
        return send_error(req, 404, "No photos");

    // Photos are sorted — last one is the latest
    std::string latest = photos.back();
    std::string path = std::string(PHOTO_DIR) + "/" + latest;

    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return send_error(req, 404, "File not found");

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    httpd_resp_set_type(req, "image/jpeg");
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "attachment; filename=\"%s\"", latest.c_str());
    httpd_resp_set_hdr(req, "Content-Disposition", hdr);

    // Stream in 4KB chunks
    char *buf = (char *)malloc(4096);
    if (!buf) { fclose(f); return send_error(req, 500, "OOM"); }

    while (fsize > 0) {
        int to_read = fsize > 4096 ? 4096 : (int)fsize;
        int n = fread(buf, 1, to_read, f);
        if (n <= 0) break;
        httpd_resp_send_chunk(req, buf, n);
        fsize -= n;
    }
    httpd_resp_send_chunk(req, NULL, 0);

    free(buf);
    fclose(f);
    return ESP_OK;
}

// ─── POST /api/v1/reboot ───────────────────────────────
// Reboots the ESP32-S3 after a brief delay
esp_err_t HttpServer::handleReboot(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "rebooting");

    esp_err_t rc = send_json(req, root);

    // Give the HTTP response time to be sent before rebooting
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return rc;
}

#else  // NATIVE_BUILD

HttpServer::HttpServer() : _camera(nullptr), _sd(nullptr), _ota(nullptr), _running(false) {}
HttpServer::~HttpServer() {}
bool HttpServer::begin(CameraManager*, SDCardManager*, OTAManager*) { return true; }
void HttpServer::stop() {}

#endif
