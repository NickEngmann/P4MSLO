/**
 * @file      SPISlave.h
 * @brief     SPI slave for JPEG transfer to ESP32-P4 master
 *
 * Protocol:
 *   Master sends 1-byte command, slave responds:
 *   0x01 → status byte (0=idle, 1=jpeg_ready, 2=busy)
 *   0x02 → 4-byte JPEG size (little-endian)
 *   0x03 → JPEG data bytes (master clocks out N bytes)
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

class SPISlave {
public:
    SPISlave();

    bool begin();
    void stop();

    /** Set the JPEG data available for transfer */
    void setJpegData(const uint8_t *data, size_t len);

    /** Clear JPEG data (after transfer or release) */
    void clearJpegData();

    /** Check if JPEG data is available */
    bool hasJpegData() const { return _jpegData != nullptr; }

private:
    const uint8_t *_jpegData;
    size_t _jpegLen;
    bool _initialized;

    static void spiTask(void *param);
};
