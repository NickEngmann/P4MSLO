/**
 * @file      SPISlave.h
 * @brief     SPI slave for JPEG transfer to ESP32-P4 master
 *
 * Protocol:
 *   Master sends 1-byte command byte in the first rx position:
 *   0x01  CMD_STATUS      → slave responds with IDLE header:
 *                             byte 0: status flags (see below)
 *                             bytes 1-4: JPEG size (little-endian)
 *   0x02  CMD_GET_SIZE    → same as status; size is already in the header
 *   0x03  CMD_READ_DATA   → slave transitions to DATA streaming mode and
 *                           sends PREAMBLE + JPEG bytes in 4KB chunks
 *
 *   Control commands (no data transfer, slave fires callback then returns):
 *   0x10  CMD_WIFI_ON     → start WiFi + HTTP server
 *   0x11  CMD_WIFI_OFF    → stop HTTP server + WiFi
 *   0x12  CMD_REBOOT      → esp_restart()
 *   0x13  CMD_IDENTIFY    → blink the NeoPixel for a few seconds so the
 *                           user can physically find this camera
 *
 *   Status byte bit layout:
 *     bit 0 (0x01): JPEG_READY
 *     bit 1 (0x02): WIFI_ACTIVE   (radio on, HTTP listening)
 *     bit 2 (0x04): WIFI_CONNECTED (has an IP address)
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <functional>

/** Command bytes (also used by master-side helpers).
 *
 * Keep ALL command values in the 0x01–0x07 range — empirically only low-bit
 * values (0x01, 0x03) transmit reliably through the master's SPI HW to the
 * slave's rx_buf. Higher-bit patterns like 0x10 or 0x41 get mangled during
 * short single-transaction sends. Values 0x04–0x07 are adjacent to the
 * proven-working 0x03 (CMD_READ_DATA) and use the same low-byte wire
 * patterns, so they transmit just as reliably. */
static constexpr uint8_t SPI_CMD_STATUS     = 0x01;
static constexpr uint8_t SPI_CMD_GET_SIZE   = 0x02;
static constexpr uint8_t SPI_CMD_READ_DATA  = 0x03;
static constexpr uint8_t SPI_CMD_WIFI_ON    = 0x04;
static constexpr uint8_t SPI_CMD_WIFI_OFF   = 0x05;
static constexpr uint8_t SPI_CMD_REBOOT     = 0x06;
static constexpr uint8_t SPI_CMD_IDENTIFY   = 0x07;

/** Status byte bit flags */
static constexpr uint8_t SPI_STATUS_JPEG_READY      = 0x01;
static constexpr uint8_t SPI_STATUS_WIFI_ACTIVE     = 0x02;
static constexpr uint8_t SPI_STATUS_WIFI_CONNECTED  = 0x04;

/** Callback type for control commands — invoked from the SPI task context */
typedef std::function<void(uint8_t cmd)> ControlCallback;

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

    /** Register a handler that runs when the master sends a control command
     *  (CMD_WIFI_ON/OFF, CMD_REBOOT, CMD_IDENTIFY). Called from the SPI task,
     *  so the handler should offload heavy work to a separate task/queue. */
    void setControlCallback(ControlCallback cb) { _controlCb = cb; }

    /** Update the WiFi status flags that appear in every IDLE header response.
     *  Master sees them via CMD_STATUS polling. */
    void setWifiStatus(bool active, bool connected);

private:
    const uint8_t *_jpegData;
    size_t _jpegLen;
    bool _initialized;
    uint8_t _statusFlags;      // extra bits OR'd into the IDLE header status byte
    ControlCallback _controlCb;

    static void spiTask(void *param);
};
