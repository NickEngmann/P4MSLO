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

    /* --- Phase 4: exposure sync + autofocus ---
     *
     * Reads the OV5640 AE state directly from sensor regs so a PIMSLO master
     * can copy one camera's exposure onto the others (cross-camera brightness
     * sync). Register layout per OV5640 datasheet:
     *   gain     : 10-bit, regs 0x350A[1:0] : 0x350B[7:0]
     *   exposure : 20-bit, regs 0x3500[3:0] : 0x3501[7:0] : 0x3502[7:4]
     * We return them as plain uint16/uint32 — callers don't need to care about
     * the packed bit layout. */
    bool getExposure(uint16_t *gain_out, uint32_t *exposure_out);

    /* Apply an explicit exposure + gain, switching the sensor to manual AEC/AGC
     * so the values stick. Call setAutoExposure(true) to re-engage the AE loop. */
    bool setExposure(uint16_t gain, uint32_t exposure);

    /* Toggle auto-exposure / auto-gain. AEC+AGC are controlled by bits [1:0]
     * of register 0x3503 (0 = auto, 1 = manual). */
    bool setAutoExposure(bool enabled);

    /* Trigger OV5640 autofocus.
     *
     * STUB — requires loading the OV5640 AF firmware blob to register 0x8000+
     * via I2C, which the esp32-camera library doesn't ship. Currently just
     * logs and returns true immediately so the PIMSLO pipeline doesn't stall.
     * The AF firmware blob + I2C load sequence is Phase 4.5 work. */
    bool autofocus(uint32_t timeout_ms = 2000);

private:
    bool     _initialized;
    uint16_t _width;
    uint16_t _height;
    std::string _sensorName;

#ifndef NATIVE_BUILD
    void *_fb;  // camera_fb_t* held during capture
#endif
};
