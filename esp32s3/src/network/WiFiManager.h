/**
 * @file      WiFiManager.h
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 *
 * WiFi connection with hardcoded dual-SSID fallback.
 * Tries primary network first, falls back to backup if primary fails.
 */
#pragma once

#include <stdint.h>
#include <string>
#include <functional>

enum class WiFiState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR
};

typedef std::function<void(const std::string &ip)> OnIPCallback;

class WiFiManager {
public:
    WiFiManager();
    ~WiFiManager();

    // Connect with primary SSID, fall back to backup if primary fails
    bool beginWithFallback(const char *ssid1, const char *pass1,
                           const char *ssid2, const char *pass2);

    WiFiState getState() const { return _state; }
    std::string getIPAddress() const { return _ipAddress; }
    bool isConnected() const { return _state == WiFiState::CONNECTED; }

    void onIPAssigned(OnIPCallback cb) { _onIPCallback = cb; }

    // Public for static event handler
    WiFiState   _state;
    std::string _ipAddress;
    OnIPCallback _onIPCallback;
    uint32_t    _reconnectAttempts;
    const char *_backupSsid;
    const char *_backupPass;
    bool _triedBackup;

private:
    bool connect(const char *ssid, const char *password);
    std::string _ssid;
    std::string _password;
};
