/**
 * @file      CameraManager.h
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 *
 * Point-and-shoot camera driver. Auto-detects OV5640 or OV3660,
 * captures at maximum resolution with highest JPEG quality.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>

class CameraManager {
public:
    CameraManager();
    ~CameraManager();

    bool begin();
    void stop();

    // Capture a high-quality JPEG photo. Returns JPEG data in outBuf/outLen.
    // Caller must call releasePhoto() when done with the data.
    // Handles waking the OV5640 from standby and putting it back to sleep.
    bool capturePhoto(uint8_t **outBuf, size_t *outLen);
    void releasePhoto();

    // Put the OV5640 into software standby (register 0x3008 = 0x42).
    // Reduces idle current and thermal buildup between captures.
    void enterStandby();

    // Wake the OV5640 from standby (register 0x3008 = 0x02).
    // Safe to call even if already awake.
    void wakeFromStandby();

    bool isInitialized() const { return _initialized; }
    uint16_t getWidth() const { return _width; }
    uint16_t getHeight() const { return _height; }
    std::string getSensorName() const { return _sensorName; }

private:
    bool     _initialized;
    uint16_t _width;
    uint16_t _height;
    std::string _sensorName;

#ifndef NATIVE_BUILD
    void *_fb;  // camera_fb_t* held during capture
#endif
};
