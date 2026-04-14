/**
 * @file      StatusLED.cpp
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 * @date      2026-03-15
 *
 * External NeoPixel (WS2812) control via htcw_rmt_led_strip library.
 * Uses the RMT peripheral for hardware-timed WS2812 protocol.
 */

#include "StatusLED.h"

#ifndef NATIVE_BUILD
#include "rmt_led_strip.hpp"
#include "esp_log.h"

static const char *TAG = "led";
static htcw::ws2812 *s_strip = nullptr;
#endif

StatusLED::StatusLED()
    : _state(LEDState::OFF)
    , _tickCount(0)
    , _lastUpdateMs(0)
    , _initialized(false) {}

StatusLED::~StatusLED() {
    stop();
}

#ifndef NATIVE_BUILD

bool StatusLED::begin() {
    s_strip = new htcw::ws2812(NEOPIXEL_PIN, 1);
    if (!s_strip) {
        ESP_LOGE(TAG, "Failed to allocate LED strip");
        return false;
    }

    if (!s_strip->initialize()) {
        ESP_LOGE(TAG, "LED strip init failed");
        delete s_strip;
        s_strip = nullptr;
        return false;
    }

    s_strip->color(0, 0, 0, 0);
    s_strip->update();

    _initialized = true;
    ESP_LOGI(TAG, "NeoPixel initialized on GPIO %d", NEOPIXEL_PIN);
    return true;
}

void StatusLED::stop() {
    if (_initialized) {
        setOff();
        if (s_strip) {
            s_strip->deinitialize();
            delete s_strip;
            s_strip = nullptr;
        }
        _initialized = false;
    }
}

void StatusLED::setColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!_initialized || !s_strip) return;
    s_strip->color(0, r, g, b);
    s_strip->update();
}

void StatusLED::setOff() {
    if (!_initialized || !s_strip) return;
    s_strip->color(0, 0, 0, 0);
    s_strip->update();
}

#else  // NATIVE_BUILD

bool StatusLED::begin() {
    _initialized = true;
    return true;
}

void StatusLED::stop() {
    _initialized = false;
}

void StatusLED::setColor(uint8_t r, uint8_t g, uint8_t b) {
    (void)r; (void)g; (void)b;
}

void StatusLED::setOff() {}

#endif  // NATIVE_BUILD

#ifndef NATIVE_BUILD
static const char *ledStateNames[] = {
    "OFF", "BOOTING", "READY", "CAPTURING", "NO_SD_CARD",
    "WIFI_CONNECTING", "OTA_UPDATE"
};
#endif

void StatusLED::setState(LEDState state) {
    if (state != _state) {
#ifndef NATIVE_BUILD
        int idx = (int)state;
        const char *name = (idx >= 0 && idx < 7) ? ledStateNames[idx] : "UNKNOWN";
        ESP_LOGI("led", "LED state: %s -> %s", ledStateNames[(int)_state], name);
#endif
    }
    _state = state;
    _tickCount = 0;
}

void StatusLED::update() {
    if (!_initialized) return;
    _tickCount++;

    switch (_state) {
    case LEDState::OFF:
        setOff();
        break;

    case LEDState::BOOTING:
        setColor(LED_BRIGHTNESS, LED_BRIGHTNESS, 0);  // Yellow
        break;

    case LEDState::READY: {
        // Slow pulse green, 2s period
        uint8_t phase = _tickCount % 40;
        uint8_t b = (phase < 20) ? (phase * LED_BRIGHTNESS / 20)
                                 : ((40 - phase) * LED_BRIGHTNESS / 20);
        setColor(0, b, 0);
        break;
    }

    case LEDState::CAPTURING:
        // White flash
        setColor(LED_BRIGHTNESS, LED_BRIGHTNESS, LED_BRIGHTNESS);
        break;

    case LEDState::NO_SD_CARD:
        if ((_tickCount % 4) < 2)
            setColor(LED_BRIGHTNESS, 0, 0);
        else
            setOff();
        break;

    case LEDState::WIFI_CONNECTING:
        if ((_tickCount % 2) == 0)
            setColor(0, 0, LED_BRIGHTNESS);
        else
            setOff();
        break;

    case LEDState::OTA_UPDATE: {
        uint8_t phase = _tickCount % 40;
        uint8_t b = (phase < 20) ? (phase * LED_BRIGHTNESS / 20)
                                 : ((40 - phase) * LED_BRIGHTNESS / 20);
        setColor(b, b / 2, 0);
        break;
    }
    }
}
