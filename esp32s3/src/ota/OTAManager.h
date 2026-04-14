/**
 * @file      OTAManager.h
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 *
 * OTA firmware update via HTTP URL.
 */
#pragma once

#include <stdint.h>
#include <string>

enum class OTAState {
    IDLE,
    DOWNLOADING,
    REBOOTING,
    ERROR
};

class OTAManager {
public:
    OTAManager();
    ~OTAManager();

    // Mark current firmware as valid (call within 30s of boot)
    void markValid();

    // Download firmware from URL and flash to inactive OTA partition
    void startUpdateFromURL(const std::string &url);

    OTAState getState() const { return _state; }

private:
    OTAState _state;
};
