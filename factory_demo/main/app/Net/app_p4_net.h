/**
 * @file app_p4_net.h
 * @brief WiFi (via onboard ESP32-C6 coprocessor) + HTTP + OTA for the P4.
 *
 * Phase 5 (MVP). WiFi is OFF by default at boot so it cannot destabilize the
 * PIMSLO SPI capture flow (analogous to DISABLE_WIFI=1 on the S3 slaves).
 * The user brings WiFi up via the `wifi_start` serial command when they need
 * OTA or the status endpoint.
 *
 * HTTP endpoints (mirrors the S3 slave API style):
 *   GET  /api/v1/status       — firmware_version, uptime_ms, free_heap, IP
 *   POST /api/v1/ota/upload   — firmware.bin in body → flash → reboot
 *
 * Discovery: mDNS hostname `pimslo-p4.local`.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enable the C6 coprocessor and bring up WiFi STA + HTTP + mDNS.
 *
 * Drives BSP_C6_EN_PIN high to release the C6 from reset, then initializes
 * esp_netif + esp_wifi (routed through esp_wifi_remote → esp_hosted), waits
 * briefly for an IP, and starts the HTTP server + mDNS service.
 *
 * Safe to call multiple times — second call is a no-op if already started.
 */
esp_err_t app_p4_net_start(void);

/**
 * @brief Tear down HTTP, disconnect WiFi, disable the C6.
 */
esp_err_t app_p4_net_stop(void);

/**
 * @brief Is WiFi currently connected (has an IP)?
 */
bool app_p4_net_is_connected(void);

/**
 * @brief Copy the current IP (empty string if not connected).
 */
void app_p4_net_get_ip(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif
