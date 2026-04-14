/**
 * @file      StatusLED.h
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 * @date      2026-03-15
 *
 * NeoPixel LED state machine for visual status indication.
 * Driven by a low-priority RTOS task on Core 0.
 */
#pragma once

#include <stdint.h>
#include "../config/Config.h"

enum class LEDState {
    OFF,
    BOOTING,              // Solid yellow
    READY,                // Slow pulse green — idle, ready to capture
    CAPTURING,            // Brief white flash
    NO_SD_CARD,           // Fast blink red
    WIFI_CONNECTING,      // Rapid blink blue
    OTA_UPDATE            // Breathe orange
};

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} RGBColor;

class StatusLED {
public:
    StatusLED();
    ~StatusLED();

    bool begin();
    void stop();

    void setState(LEDState state);
    LEDState getState() const { return _state; }

    // Called by RTOS task to advance animation
    void update();

private:
    void setColor(uint8_t r, uint8_t g, uint8_t b);
    void setOff();

    LEDState _state;
    uint32_t _tickCount;
    uint32_t _lastUpdateMs;
    bool     _initialized;
};
