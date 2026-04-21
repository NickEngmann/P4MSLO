/**
 * @file wifi_config.h
 * @brief Hardcoded WiFi credentials + mDNS config for the P4 coprocessor link.
 *
 * MVP scope — a real product would read these from NVS or show an AP-mode
 * captive portal. For now they match the PIMSLO cameras' primary SSID so the
 * P4 joins the same LAN as the S3 slaves.
 */
#pragma once

#define P4_WIFI_SSID       "The Garden"
#define P4_WIFI_PSK        "SanDiego2019!"
#define P4_MDNS_HOSTNAME   "pimslo-p4"
#define P4_MDNS_INSTANCE   "PIMSLO P4 Controller"
#define P4_HTTP_PORT       80
