/**
 * @file      SPISlave.cpp
 * @brief     SPI slave for JPEG transfer to ESP32-P4 master
 *
 * Protocol: The slave pre-loads a response buffer before each transaction.
 * Since SPI is full-duplex and synchronous, the slave can't react to
 * the master's command during the same transaction — it responds in the
 * NEXT transaction. The master must account for this one-transaction lag.
 *
 * Master protocol:
 *   1. Send CMD_STATUS (dummy read) → slave pre-loads status for next
 *   2. Send CMD_GET_SIZE → read back status from step 1
 *   3. Send CMD_READ_DATA → read back size from step 2
 *   4. Send dummy bytes → read back JPEG data chunks
 *
 * Simplified: slave always pre-fills TX with a header struct containing
 * status + size. For data, slave enters a streaming mode.
 */

#include "SPISlave.h"
#include "../config/Config.h"

#if ENABLE_SPI_SLAVE

#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "spi_slave";

#define CHUNK_SIZE    4096

/* Commands from master */
#define CMD_STATUS      0x01
#define CMD_GET_SIZE    0x02
#define CMD_READ_DATA   0x03

SPISlave::SPISlave()
    : _jpegData(nullptr), _jpegLen(0), _initialized(false)
{
}

bool SPISlave::begin()
{
    if (_initialized) return true;

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = SPI_SLAVE_MOSI_PIN;
    bus_cfg.miso_io_num = SPI_SLAVE_MISO_PIN;
    bus_cfg.sclk_io_num = SPI_SLAVE_CLK_PIN;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = CHUNK_SIZE;

    spi_slave_interface_config_t slave_cfg = {};
    slave_cfg.mode = 0;
    slave_cfg.spics_io_num = SPI_SLAVE_CS_PIN;
    slave_cfg.queue_size = 2;
    slave_cfg.flags = 0;

    esp_err_t ret = spi_slave_initialize(SPI3_HOST, &bus_cfg, &slave_cfg, SPI_SLAVE_DMA_CHAN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI slave init failed: 0x%x", ret);
        return false;
    }

    gpio_set_pull_mode((gpio_num_t)SPI_SLAVE_CLK_PIN, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)SPI_SLAVE_CS_PIN, GPIO_PULLUP_ONLY);

    _initialized = true;

    xTaskCreatePinnedToCore(spiTask, "spi_slave", 4096, this, 10, nullptr, 1);

    ESP_LOGI(TAG, "SPI slave initialized (CLK=%d MOSI=%d MISO=%d CS=%d)",
             SPI_SLAVE_CLK_PIN, SPI_SLAVE_MOSI_PIN, SPI_SLAVE_MISO_PIN, SPI_SLAVE_CS_PIN);
    return true;
}

void SPISlave::stop()
{
    if (_initialized) {
        spi_slave_free(SPI3_HOST);
        _initialized = false;
    }
}

void SPISlave::setJpegData(const uint8_t *data, size_t len)
{
    _jpegData = data;
    _jpegLen = len;
    ESP_LOGI(TAG, "JPEG ready: %zu bytes", len);
}

void SPISlave::clearJpegData()
{
    _jpegData = nullptr;
    _jpegLen = 0;
}

/**
 * SPI slave task — uses a simple state machine:
 *
 * IDLE state: slave pre-fills TX with [status, sizeLE[4]]
 *   Master sends any command → slave responds with current state
 *   If master sent CMD_READ_DATA → transition to DATA state
 *
 * DATA state: slave pre-fills TX with JPEG data chunks
 *   Master clocks out dummy bytes → slave sends data
 *   After all data sent → transition back to IDLE
 */
void SPISlave::spiTask(void *param)
{
    SPISlave *self = (SPISlave *)param;

    WORD_ALIGNED_ATTR uint8_t *tx_buf = (uint8_t *)heap_caps_calloc(1, CHUNK_SIZE, MALLOC_CAP_DMA);
    WORD_ALIGNED_ATTR uint8_t *rx_buf = (uint8_t *)heap_caps_calloc(1, CHUNK_SIZE, MALLOC_CAP_DMA);

    if (!tx_buf || !rx_buf) {
        ESP_LOGE(TAG, "Failed to alloc SPI DMA buffers");
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "SPI slave task running");

    enum { STATE_IDLE, STATE_DATA } state = STATE_IDLE;
    size_t data_offset = 0;

    while (true) {
        /* Pre-fill TX buffer based on state */
        memset(tx_buf, 0, CHUNK_SIZE);

        if (state == STATE_IDLE) {
            /* Header: [status(1), size_le(4)] = 5 bytes */
            uint8_t status = self->_jpegData ? 0x01 : 0x00;
            uint32_t size = (uint32_t)self->_jpegLen;
            tx_buf[0] = status;
            tx_buf[1] = (size >> 0) & 0xFF;
            tx_buf[2] = (size >> 8) & 0xFF;
            tx_buf[3] = (size >> 16) & 0xFF;
            tx_buf[4] = (size >> 24) & 0xFF;
        } else if (state == STATE_DATA) {
            /* Fill with JPEG data chunk */
            if (self->_jpegData && data_offset < self->_jpegLen) {
                size_t remaining = self->_jpegLen - data_offset;
                size_t chunk = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;
                memcpy(tx_buf, self->_jpegData + data_offset, chunk);
            }
        }

        spi_slave_transaction_t trans = {};
        trans.length = CHUNK_SIZE * 8;
        trans.tx_buffer = tx_buf;
        trans.rx_buffer = rx_buf;

        esp_err_t ret = spi_slave_transmit(SPI3_HOST, &trans, portMAX_DELAY);
        if (ret != ESP_OK) continue;

        /* Process received command */
        uint8_t cmd = rx_buf[0];

        if (state == STATE_IDLE) {
            if (cmd == CMD_READ_DATA && self->_jpegData) {
                /* Transition to data streaming mode */
                state = STATE_DATA;
                data_offset = 0;
                ESP_LOGI(TAG, "Entering DATA mode (%zu bytes)", self->_jpegLen);
            }
            /* CMD_STATUS and CMD_GET_SIZE are handled by the pre-filled header */
        } else if (state == STATE_DATA) {
            /* Advance offset after master read this chunk */
            size_t remaining = self->_jpegLen - data_offset;
            size_t chunk = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;
            data_offset += chunk;

            if (data_offset >= self->_jpegLen) {
                /* All data sent */
                state = STATE_IDLE;
                ESP_LOGI(TAG, "DATA transfer complete");
            }
        }
    }

    free(tx_buf);
    free(rx_buf);
    vTaskDelete(nullptr);
}

#else

SPISlave::SPISlave() : _jpegData(nullptr), _jpegLen(0), _initialized(false) {}
bool SPISlave::begin() { return false; }
void SPISlave::stop() {}
void SPISlave::setJpegData(const uint8_t *, size_t) {}
void SPISlave::clearJpegData() {}

#endif
