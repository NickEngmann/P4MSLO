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
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "soc/spi_periph.h"
#include "hal/gpio_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "spi_slave";

#define CHUNK_SIZE    4096

/* Commands from master — kept as macros for readability inside this file.
 * Public values live in SPISlave.h as SPI_CMD_* constexprs. */
#define CMD_STATUS      SPI_CMD_STATUS
#define CMD_GET_SIZE    SPI_CMD_GET_SIZE
#define CMD_READ_DATA   SPI_CMD_READ_DATA
#define CMD_WIFI_ON     SPI_CMD_WIFI_ON
#define CMD_WIFI_OFF    SPI_CMD_WIFI_OFF
#define CMD_REBOOT      SPI_CMD_REBOOT
#define CMD_IDENTIFY    SPI_CMD_IDENTIFY

/**
 * CS-gated MISO tri-state ISR.
 *
 * On a shared-bus SPI topology, the ESP-IDF SPI slave driver keeps MISO
 * actively driven even when CS is HIGH (deselected). Four slaves all driving
 * MISO simultaneously fight over the 330Ω pulldowns → master reads garbage
 * on any slave that doesn't happen to "win" the voltage divider.
 *
 * This ISR watches our CS pin and reconfigures MISO dynamically:
 *   CS LOW  (selected)   → MISO = OUTPUT, driven by SPI peripheral
 *   CS HIGH (deselected) → MISO = INPUT,  high-Z, releases the bus
 *
 * That way the selected slave is the only one driving at any moment.
 *
 * Latency budget: GPIO ISR on S3 ≈ 2–5 µs with IRAM. At 10 MHz SPI each bit
 * is 100 ns, so the ISR may "miss" a handful of leading clocks. The master
 * configures cs_ena_pretrans to wait a few cycles after CS↓ before the first
 * SCLK edge, which covers us.
 */
static void IRAM_ATTR cs_edge_isr(void *arg)
{
    if (gpio_get_level((gpio_num_t)SPI_SLAVE_CS_PIN) == 0) {
        /* CS asserted → drive MISO via SPI peripheral */
        gpio_ll_output_enable(&GPIO, (gpio_num_t)SPI_SLAVE_MISO_PIN);
    } else {
        /* CS deasserted → release MISO (input / high-Z) */
        gpio_ll_output_disable(&GPIO, (gpio_num_t)SPI_SLAVE_MISO_PIN);
    }
}

static void install_miso_tristate(void)
{
    /* SPI slave driver already owns the CS pin as an input for detecting
     * master's CS edges. We just need to:
     *   1. Release MISO now so other slaves can win the bus at boot
     *   2. Enable CS edge interrupts
     *   3. Hook our ISR to toggle MISO's output-enable on each edge
     */
    gpio_ll_output_disable(&GPIO, (gpio_num_t)SPI_SLAVE_MISO_PIN);

    /* Enable any-edge interrupts on the CS pin (the SPI driver already
     * configured it as an input with pull-up). */
    gpio_set_intr_type((gpio_num_t)SPI_SLAVE_CS_PIN, GPIO_INTR_ANYEDGE);

    /* Use IRAM flag so ISR runs from IRAM (low latency). Level-1 is the default
     * priority and is allocated automatically — explicitly requesting LEVEL1
     * can fail with "No free interrupt inputs" on some ESP-IDF versions. */
    esp_err_t isr_ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service: 0x%x", isr_ret);
    }
    gpio_isr_handler_add((gpio_num_t)SPI_SLAVE_CS_PIN, cs_edge_isr, nullptr);

    ESP_LOGI(TAG, "MISO tri-state ISR installed on CS=%d, MISO=%d",
             SPI_SLAVE_CS_PIN, SPI_SLAVE_MISO_PIN);
}

SPISlave::SPISlave()
    : _jpegData(nullptr), _jpegLen(0), _initialized(false), _statusFlags(0), _controlCb(nullptr)
{
}

void SPISlave::setWifiStatus(bool active, bool connected)
{
    uint8_t flags = 0;
    if (active)    flags |= SPI_STATUS_WIFI_ACTIVE;
    if (connected) flags |= SPI_STATUS_WIFI_CONNECTED;
    _statusFlags = flags;
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

    /* Release MISO to high-Z when CS is HIGH, so idle slaves don't fight
     * the selected one on the shared bus. */
    install_miso_tristate();

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
 * Magic preamble the slave emits right before the JPEG bytes. Master scans
 * incoming data for DEADBEEF to align past any stale IDLE-header bytes that
 * leaked in before the slave's first DATA chunk was queued.
 */
#define PREAMBLE_LEN  12
static const uint8_t PREAMBLE[PREAMBLE_LEN] = {
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    0xDE, 0xAD, 0xBE, 0xEF,
};

/**
 * Stream JPEG data using double-buffered queued transactions. Buffers are
 * PRE-ALLOCATED at task startup and passed in — no per-call allocation, so
 * the window between sync CMD_READ_DATA completing and the first DATA chunk
 * being queued is as short as possible.
 */
static void streamJpegData(const uint8_t *jpeg_data, size_t jpeg_len,
                           uint8_t *tx_bufs[2], uint8_t *rx_bufs[2])
{
    spi_slave_transaction_t trans[2] = {};

    /* Fill a chunk with preamble + JPEG[data_offset..], zero-padded.
     * Preamble emitted only in the very first chunk. */
    auto fill_chunk = [&](uint8_t *tx, size_t data_offset) -> size_t {
        memset(tx, 0, CHUNK_SIZE);
        size_t pos = 0;
        if (data_offset == 0) {
            memcpy(tx, PREAMBLE, PREAMBLE_LEN);
            pos = PREAMBLE_LEN;
        }
        if (data_offset >= jpeg_len) return 0;
        size_t remaining = jpeg_len - data_offset;
        size_t space = CHUNK_SIZE - pos;
        size_t take = remaining > space ? space : remaining;
        memcpy(tx + pos, jpeg_data + data_offset, take);
        return take;
    };

    size_t data_offset = 0;
    int in_flight = 0;

    /* Pre-queue two chunks up front. */
    for (int i = 0; i < 2; i++) {
        size_t consumed = fill_chunk(tx_bufs[i], data_offset);
        data_offset += consumed;
        trans[i].length = CHUNK_SIZE * 8;
        trans[i].tx_buffer = tx_bufs[i];
        trans[i].rx_buffer = rx_bufs[i];
        if (spi_slave_queue_trans(SPI3_HOST, &trans[i], portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "queue_trans failed in DATA init");
            return;
        }
        in_flight++;
        if (data_offset >= jpeg_len && consumed == 0) break;
    }

    /* Drain and refill. Keep emitting zero-padded chunks for one extra
     * iteration past jpeg_len to cover master's over-read. */
    bool padded_tail = false;
    while (in_flight > 0) {
        spi_slave_transaction_t *done = nullptr;
        esp_err_t ret = spi_slave_get_trans_result(SPI3_HOST, &done, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "get_trans_result failed: 0x%x", ret);
            return;
        }
        in_flight--;
        int slot = (done == &trans[0]) ? 0 : 1;

        if (data_offset < jpeg_len) {
            size_t consumed = fill_chunk(tx_bufs[slot], data_offset);
            data_offset += consumed;
            if (spi_slave_queue_trans(SPI3_HOST, &trans[slot], portMAX_DELAY) != ESP_OK) {
                ESP_LOGE(TAG, "re-queue failed");
                return;
            }
            in_flight++;
        } else if (!padded_tail) {
            /* One extra zero-padded chunk so any master over-read gets clean zeros */
            memset(tx_bufs[slot], 0, CHUNK_SIZE);
            if (spi_slave_queue_trans(SPI3_HOST, &trans[slot], portMAX_DELAY) == ESP_OK) {
                in_flight++;
                padded_tail = true;
            }
        }
    }
}

/**
 * SPI slave task — two-phase state machine:
 *
 * IDLE: synchronous spi_slave_transmit with pre-filled header. Handshake
 *   commands from master come with natural gaps (≥50ms between polls) so
 *   the sync call's inter-transaction gap is tolerable.
 *
 * DATA: entered on CMD_READ_DATA. Uses double-buffered queued transactions
 *   with PRE-ALLOCATED DMA buffers (minimizing the setup window) and
 *   prepends a PREAMBLE+MAGIC that the master scans for to re-align past
 *   any IDLE leak.
 */
void SPISlave::spiTask(void *param)
{
    SPISlave *self = (SPISlave *)param;

    /* IDLE single buffer */
    WORD_ALIGNED_ATTR uint8_t *tx_buf = (uint8_t *)heap_caps_calloc(1, CHUNK_SIZE, MALLOC_CAP_DMA);
    WORD_ALIGNED_ATTR uint8_t *rx_buf = (uint8_t *)heap_caps_calloc(1, CHUNK_SIZE, MALLOC_CAP_DMA);

    /* DATA ping-pong buffers — pre-allocated so entering DATA mode is fast */
    uint8_t *data_tx[2] = {
        (uint8_t *)heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DMA),
        (uint8_t *)heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DMA),
    };
    uint8_t *data_rx[2] = {
        (uint8_t *)heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DMA),
        (uint8_t *)heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DMA),
    };

    if (!tx_buf || !rx_buf || !data_tx[0] || !data_tx[1] || !data_rx[0] || !data_rx[1]) {
        ESP_LOGE(TAG, "Failed to alloc SPI DMA buffers");
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "SPI slave task running (sync IDLE + queued DATA with preamble)");

    while (true) {
        /* IDLE: pre-fill header and do one synchronous transaction */
        memset(tx_buf, 0, CHUNK_SIZE);
        uint8_t status = self->_statusFlags;
        if (self->_jpegData) status |= SPI_STATUS_JPEG_READY;
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
        /* Scan the first 8 bytes of rx for control command values —
         * master embeds them at offset 4 to dodge first-byte transmission
         * issues (see spi_camera_send_control on the P4). */
        for (int i = 0; i < 8; i++) {
            uint8_t b = rx_buf[i];
            if (b == CMD_WIFI_ON || b == CMD_WIFI_OFF ||
                b == CMD_REBOOT  || b == CMD_IDENTIFY) {
                cmd = b;
                break;
            }
        }
        if (cmd == CMD_READ_DATA && self->_jpegData) {
            ESP_LOGI(TAG, "CMD_READ_DATA → DATA (%zu bytes)", self->_jpegLen);
            streamJpegData(self->_jpegData, self->_jpegLen, data_tx, data_rx);
            ESP_LOGI(TAG, "DATA transfer complete");
        } else if (cmd == CMD_WIFI_ON || cmd == CMD_WIFI_OFF ||
                   cmd == CMD_REBOOT  || cmd == CMD_IDENTIFY) {
            ESP_LOGI(TAG, "Control cmd 0x%02X", cmd);
            if (self->_controlCb) self->_controlCb(cmd);
        }
    }
}

#else

SPISlave::SPISlave() : _jpegData(nullptr), _jpegLen(0), _initialized(false), _statusFlags(0), _controlCb(nullptr) {}
bool SPISlave::begin() { return false; }
void SPISlave::stop() {}
void SPISlave::setJpegData(const uint8_t *, size_t) {}
void SPISlave::clearJpegData() {}
void SPISlave::setWifiStatus(bool, bool) {}

#endif
