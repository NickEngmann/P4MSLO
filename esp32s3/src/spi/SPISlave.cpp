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
    slave_cfg.queue_size = 3;
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
 * Stream JPEG data using double-buffered queued transactions.
 *
 * The synchronous `spi_slave_transmit()` has a race window between when one
 * transaction completes and the next is set up — if the master clocks during
 * that window it reads stale FIFO contents, corrupting entropy data. Queuing
 * two transactions up front means the HW always has the next chunk pre-loaded
 * in DMA the moment CS re-asserts.
 */
static esp_err_t streamJpegData(const uint8_t *jpeg_data, size_t jpeg_len,
                                uint8_t *rx_buf_scratch)
{
    /* Two DMA-capable TX buffers for ping-pong */
    uint8_t *tx_bufs[2] = {
        (uint8_t *)heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DMA),
        (uint8_t *)heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DMA),
    };
    uint8_t *rx_bufs[2] = {
        (uint8_t *)heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DMA),
        (uint8_t *)heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DMA),
    };
    spi_slave_transaction_t trans[2] = {};

    if (!tx_bufs[0] || !tx_bufs[1] || !rx_bufs[0] || !rx_bufs[1]) {
        ESP_LOGE(TAG, "OOM in streamJpegData");
        for (int i = 0; i < 2; i++) {
            if (tx_bufs[i]) free(tx_bufs[i]);
            if (rx_bufs[i]) free(rx_bufs[i]);
        }
        return ESP_ERR_NO_MEM;
    }

    size_t offset_next = 0;
    int in_flight = 0;

    /* Pre-queue both slots with the first two chunks */
    for (int i = 0; i < 2 && offset_next < jpeg_len; i++) {
        size_t remaining = jpeg_len - offset_next;
        size_t chunk = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;
        memcpy(tx_bufs[i], jpeg_data + offset_next, chunk);
        if (chunk < CHUNK_SIZE) memset(tx_bufs[i] + chunk, 0, CHUNK_SIZE - chunk);
        offset_next += chunk;

        trans[i].length = CHUNK_SIZE * 8;
        trans[i].tx_buffer = tx_bufs[i];
        trans[i].rx_buffer = rx_bufs[i];
        esp_err_t ret = spi_slave_queue_trans(SPI3_HOST, &trans[i], portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "queue_trans failed at init: 0x%x", ret);
            goto cleanup;
        }
        in_flight++;
    }

    /* Drain completed transactions and re-queue with next chunk until done */
    while (in_flight > 0) {
        spi_slave_transaction_t *done = NULL;
        esp_err_t ret = spi_slave_get_trans_result(SPI3_HOST, &done, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "get_trans_result failed: 0x%x", ret);
            goto cleanup;
        }
        in_flight--;

        /* Identify which ping-pong slot completed so we re-use the right TX buffer */
        int slot = (done == &trans[0]) ? 0 : 1;

        if (offset_next < jpeg_len) {
            size_t remaining = jpeg_len - offset_next;
            size_t chunk = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;
            memcpy(tx_bufs[slot], jpeg_data + offset_next, chunk);
            if (chunk < CHUNK_SIZE) memset(tx_bufs[slot] + chunk, 0, CHUNK_SIZE - chunk);
            offset_next += chunk;

            ret = spi_slave_queue_trans(SPI3_HOST, &trans[slot], portMAX_DELAY);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "re-queue failed: 0x%x", ret);
                goto cleanup;
            }
            in_flight++;
        }
    }

cleanup:
    for (int i = 0; i < 2; i++) {
        free(tx_bufs[i]);
        free(rx_bufs[i]);
    }
    return ESP_OK;
}

/**
 * SPI slave task — two-phase state machine:
 *
 * IDLE (synchronous): slave pre-fills TX with [status, sizeLE[4]].
 *   Handshake commands arrive with ≥50ms spacing from master polling, so the
 *   sync call's inter-transaction gap is harmless here.
 *
 * DATA (queued double-buffered): entered when master sends CMD_READ_DATA.
 *   See streamJpegData() — keeps one chunk pre-loaded in DMA at all times
 *   to eliminate the race that corrupted entropy data.
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

    ESP_LOGI(TAG, "SPI slave task running (queued DATA mode)");

    while (true) {
        /* IDLE: pre-fill header and do one synchronous transaction */
        memset(tx_buf, 0, CHUNK_SIZE);
        uint8_t status = self->_jpegData ? 0x01 : 0x00;
        uint32_t size = (uint32_t)self->_jpegLen;
        tx_buf[0] = status;
        tx_buf[1] = (size >> 0) & 0xFF;
        tx_buf[2] = (size >> 8) & 0xFF;
        tx_buf[3] = (size >> 16) & 0xFF;
        tx_buf[4] = (size >> 24) & 0xFF;

        spi_slave_transaction_t trans = {};
        trans.length = CHUNK_SIZE * 8;
        trans.tx_buffer = tx_buf;
        trans.rx_buffer = rx_buf;

        esp_err_t ret = spi_slave_transmit(SPI3_HOST, &trans, portMAX_DELAY);
        if (ret != ESP_OK) continue;

        uint8_t cmd = rx_buf[0];
        if (cmd == CMD_READ_DATA && self->_jpegData) {
            ESP_LOGI(TAG, "Entering DATA mode (%zu bytes)", self->_jpegLen);
            streamJpegData(self->_jpegData, self->_jpegLen, rx_buf);
            ESP_LOGI(TAG, "DATA transfer complete");
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
