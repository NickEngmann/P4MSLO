/**
 * @file      HttpServer.h
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 *
 * REST API for point-and-shoot camera.
 * Endpoints: status, capture, list photos, download photo, OTA.
 */
#pragma once

#include <stdint.h>
#include <functional>

class CameraManager;
class SDCardManager;
class OTAManager;

#ifndef NATIVE_BUILD
#include "esp_http_server.h"
#endif

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    bool begin(CameraManager *cam, SDCardManager *sd, OTAManager *ota);
    void stop();

    // Callback for remote-triggered captures (called from HTTP handler)
    typedef std::function<void()> CaptureCallback;
    void onCaptureRequest(CaptureCallback cb) { _captureCallback = cb; }

    CameraManager  *_camera;
    SDCardManager  *_sd;
    OTAManager     *_ota;
    CaptureCallback _captureCallback;

private:
    bool _running;

#ifndef NATIVE_BUILD
    httpd_handle_t _serverHandle;
    bool registerRoutes();

    static esp_err_t handleStatus(httpd_req_t *req);
    static esp_err_t handleCapture(httpd_req_t *req);
    static esp_err_t handleListPhotos(httpd_req_t *req);
    static esp_err_t handleGetPhoto(httpd_req_t *req);
    static esp_err_t handleOTA(httpd_req_t *req);
    static esp_err_t handleOTAUpload(httpd_req_t *req);
    static esp_err_t handleSDFormat(httpd_req_t *req);
    static esp_err_t handleFactoryReset(httpd_req_t *req);
#endif
};
