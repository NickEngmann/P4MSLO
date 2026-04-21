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
 * Historical wire-reliability note: individual byte values like 0x10 or 0x41
 * tended to get mangled in single-shot transmission. The master mitigates this
 * with a 10× burst of identical 8-byte transactions (spi_camera_send_control),
 * and the slave scans the whole rx_buf[0..7] for any known command value.
 * With that redundancy, values 0x08-0x0F transmit reliably too — the scan
 * will catch at least one intact copy out of ten.
 *
 * Commands 0x01-0x07 are the original "core" set. 0x08-0x0F are Phase 4
 * extensions for PIMSLO image-quality features. */
static constexpr uint8_t SPI_CMD_STATUS        = 0x01;
static constexpr uint8_t SPI_CMD_GET_SIZE      = 0x02;
static constexpr uint8_t SPI_CMD_READ_DATA     = 0x03;
static constexpr uint8_t SPI_CMD_WIFI_ON       = 0x04;
static constexpr uint8_t SPI_CMD_WIFI_OFF      = 0x05;
static constexpr uint8_t SPI_CMD_REBOOT        = 0x06;
static constexpr uint8_t SPI_CMD_IDENTIFY      = 0x07;
static constexpr uint8_t SPI_CMD_AUTOFOCUS     = 0x08;  /* Trigger OV5640 AF */
static constexpr uint8_t SPI_CMD_SET_EXPOSURE  = 0x09;  /* Apply master-supplied AE values */

/** Status byte bit flags */
static constexpr uint8_t SPI_STATUS_JPEG_READY      = 0x01;
static constexpr uint8_t SPI_STATUS_WIFI_ACTIVE     = 0x02;
static constexpr uint8_t SPI_STATUS_WIFI_CONNECTED  = 0x04;
static constexpr uint8_t SPI_STATUS_AF_LOCKED       = 0x08;  /* AF convergence */

/** Extended IDLE header layout (slave → master, part of every status poll).
 *
 *   byte 0       : status flags
 *   bytes 1-4    : jpeg_size  (uint32_t little-endian)
 *   bytes 5-6    : ae_gain    (uint16_t LE) — OV5640 register 0x350A/0x350B
 *   bytes 7-9    : ae_exposure (24-bit LE) — OV5640 regs 0x3500/0x3501/0x3502
 *
 * Older masters that only read 5 bytes continue to work unchanged. */
#define SPI_IDLE_HEADER_AE_GAIN_OFFSET       5
#define SPI_IDLE_HEADER_AE_EXPOSURE_OFFSET   7

/** SPI_CMD_SET_EXPOSURE payload layout (master → slave, in tx_buf[1..6]).
 *
 *   bytes 1-2    : ae_gain     (uint16_t LE)
 *   bytes 3-5    : ae_exposure (24-bit LE)  */
#define SPI_SET_EXPOSURE_PAYLOAD_OFFSET  1
#define SPI_SET_EXPOSURE_PAYLOAD_LEN     5

/** Callback type for control commands — invoked from the SPI task context.
 *  For commands without a payload (WIFI_ON/OFF, REBOOT, IDENTIFY, AUTOFOCUS),
 *  `payload` is nullptr and `payload_len` is 0. For SET_EXPOSURE, `payload`
 *  points to SPI_SET_EXPOSURE_PAYLOAD_LEN bytes of AE data from the master. */
typedef std::function<void(uint8_t cmd, const uint8_t *payload, size_t payload_len)> ControlCallback;

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

    /** Set/clear the AF_LOCKED flag in the IDLE header. Called by CameraManager
     *  when an autofocus cycle finishes. */
    void setAfLocked(bool locked);

    /** Update the exposure fields in the IDLE header (sent to master on every
     *  status poll). Master reads these to build a cross-camera AE reference. */
    void setExposureHeader(uint16_t ae_gain, uint32_t ae_exposure);

private:
    const uint8_t *_jpegData;
    size_t _jpegLen;
    bool _initialized;
    uint8_t _statusFlags;      // extra bits OR'd into the IDLE header status byte
    uint16_t _aeGain;          // latest sensor gain (OV5640 0x350A/0x350B)
    uint32_t _aeExposure;      // latest sensor exposure (OV5640 0x3500-0x3502, 24-bit)
    ControlCallback _controlCb;

    static void spiTask(void *param);
};
