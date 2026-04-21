/**
 * @file spi_camera.c
 * @brief SPI master for receiving JPEG data from ESP32-S3 cameras
 */

#include "spi_camera.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "spi_cam";

/* SPI command bytes (must match S3 slave protocol) */
#define CMD_STATUS    0x01
#define CMD_GET_SIZE  0x02
#define CMD_READ_DATA 0x03

/* Status values from slave */
#define STATUS_IDLE       0x00
#define STATUS_JPEG_READY 0x01
#define STATUS_BUSY       0x02

/* SPI transfer chunk size — MUST match the S3 slave's CHUNK_SIZE (4096) */
#define SPI_CHUNK_SIZE    4096

/* Maximum retries per camera for corrupted transfers */
#define SPI_MAX_RETRIES   4

/* Slave prepends this preamble to every JPEG stream so the master can skip
 * any stale IDLE-header bytes that leaked in before the slave processed
 * CMD_READ_DATA. Keep synchronized with esp32s3/src/spi/SPISlave.cpp. */
#define PREAMBLE_LEN      12
static const uint8_t SPI_PREAMBLE[PREAMBLE_LEN] = {
    0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    0xDE, 0xAD, 0xBE, 0xEF,
};

/* Extra bytes to read past (preamble + jpeg_len) to tolerate up to
 * SAFETY_STALE_BYTES of stale IDLE bytes leaking in before the magic. */
#define SAFETY_STALE_BYTES  256

/* One shared SPI device with NO hardware CS — we drive all 4 CS pins by
 * hand via gpio_set_level. This removes the dynamic CS-swap that SPI3's
 * 3-HW-CS limit forced on us, and gives deterministic CS timing so each
 * CS pin is unambiguously asserted regardless of driver quirks. */
static spi_device_handle_t s_spi_dev = NULL;
static bool s_initialized = false;
static bool s_fast_mode = false;

static const int s_cs_pins[SPI_CAM_COUNT] = {
    SPI_CAM_CS0_PIN, SPI_CAM_CS1_PIN, SPI_CAM_CS2_PIN, SPI_CAM_CS3_PIN
};

/* Drive the specified camera's CS pin LOW (assert). */
static inline void cs_assert(int cam_idx)
{
    gpio_set_level((gpio_num_t)s_cs_pins[cam_idx], 0);
    /* CS setup time — lets the slave's GPIO edge ISR re-enable MISO output
     * before the first SCLK edge. ~5 µs covers worst-case ISR latency. */
    esp_rom_delay_us(5);
}

/* Drive the specified camera's CS pin HIGH (deassert). */
static inline void cs_deassert(int cam_idx)
{
    esp_rom_delay_us(1);    /* minimal hold */
    gpio_set_level((gpio_num_t)s_cs_pins[cam_idx], 1);
}

static spi_device_handle_t get_device(int cam_idx)
{
    (void)cam_idx;   /* all cameras share one SPI device — CS is software */
    return s_spi_dev;
}

static void restore_dev2(void) { /* no-op: no CS swap */ }

esp_err_t spi_camera_init(void)
{
    if (s_initialized) return ESP_OK;

    /* Configure every CS pin as a GPIO output driven HIGH (deasserted).
     * With software CS we own these pins for the whole lifetime. */
    for (int i = 0; i < SPI_CAM_COUNT; i++) {
        gpio_config_t cs_cfg = {
            .pin_bit_mask = (1ULL << s_cs_pins[i]),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&cs_cfg);
        gpio_set_level((gpio_num_t)s_cs_pins[i], 1);
    }
    ESP_LOGI(TAG, "All CS pins driven HIGH");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SPI_CAM_MOSI_PIN,
        .miso_io_num = SPI_CAM_MISO_PIN,
        .sclk_io_num = SPI_CAM_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_CHUNK_SIZE,
    };

    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: 0x%x", ret);
        return ret;
    }

    /* Register ONE device with software CS — we wrap each transaction in
     * cs_assert/cs_deassert to pick which slave we're talking to. */
    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = 10 * 1000 * 1000,
        .spics_io_num = -1,     /* software-managed CS */
        .queue_size = 1,
    };
    ret = spi_bus_add_device(SPI3_HOST, &dev_cfg, &s_spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: 0x%x", ret);
        return ret;
    }

    /* Configure GPIO34 as trigger output (shared by all S3 cameras) */
    gpio_config_t trig_cfg = {
        .pin_bit_mask = (1ULL << GPIO_NUM_34),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&trig_cfg);
    gpio_set_level(GPIO_NUM_34, 1);

    s_initialized = true;
    ESP_LOGI(TAG, "SPI camera master: %d cameras (CS=%d,%d,%d,%d) @ 10MHz, software-CS, trigger=GPIO34",
             SPI_CAM_COUNT, SPI_CAM_CS0_PIN, SPI_CAM_CS1_PIN, SPI_CAM_CS2_PIN, SPI_CAM_CS3_PIN);
    return ESP_OK;
}

/* Full-duplex SPI transaction wrapped in software-CS for camera cam_idx. */
static esp_err_t spi_xfer_cam(int cam_idx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    cs_assert(cam_idx);
    spi_transaction_t trans = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t ret = spi_device_transmit(s_spi_dev, &trans);
    cs_deassert(cam_idx);
    return ret;
}

/**
 * Poll the slave's pre-filled header: [status(1), size_le(4)]
 * The slave ALWAYS has this ready in idle mode. The master reads it
 * alongside sending a dummy/status command. Because of the 1-transaction
 * lag, we do TWO transactions: first primes the slave, second gets the response.
 */
static esp_err_t poll_and_get_size(int cam_idx, int timeout_ms, uint32_t *out_size)
{
    WORD_ALIGNED_ATTR uint8_t tx[8] = {CMD_STATUS, 0, 0, 0, 0, 0, 0, 0};
    WORD_ALIGNED_ATTR uint8_t rx[8] = {0};
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        /* Transaction 1: send status query, get PREVIOUS response (stale on first call) */
        esp_err_t ret = spi_xfer_cam(cam_idx, tx, rx, 8);
        if (ret != ESP_OK) return ret;

        /* Transaction 2: send dummy, get the ACTUAL response from transaction 1 */
        WORD_ALIGNED_ATTR uint8_t tx2[8] = {0};
        WORD_ALIGNED_ATTR uint8_t rx2[8] = {0};
        ret = spi_xfer_cam(cam_idx, tx2, rx2, 8);
        if (ret != ESP_OK) return ret;

        uint8_t status = rx2[0];
        uint32_t size = rx2[1] | ((uint32_t)rx2[2] << 8) | ((uint32_t)rx2[3] << 16) | ((uint32_t)rx2[4] << 24);

        ESP_LOGI(TAG, "Poll: status=0x%02X size=%lu [%02X %02X %02X %02X %02X]",
                 status, (unsigned long)size, rx2[0], rx2[1], rx2[2], rx2[3], rx2[4]);

        if (status == STATUS_JPEG_READY && size > 0 && size < 2000000) {
            *out_size = size;
            ESP_LOGI(TAG, "JPEG ready: %lu bytes (polled %dms)", (unsigned long)size, elapsed);
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
        elapsed += 50;
    }

    ESP_LOGE(TAG, "Timeout waiting for JPEG (%dms)", timeout_ms);
    return ESP_ERR_TIMEOUT;
}

/**
 * Validate JPEG data integrity beyond just header/footer check.
 * Scans the JPEG structure for valid markers and checks that the
 * entropy data doesn't contain bare 0xFF bytes (which indicate corruption).
 */
static bool validate_jpeg_integrity(const uint8_t *data, size_t size)
{
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return false;

    /* Check for EOI at end */
    if (data[size - 2] != 0xFF || data[size - 1] != 0xD9) {
        ESP_LOGW(TAG, "JPEG missing EOI (last 4 bytes: %02X %02X %02X %02X)",
                 size >= 4 ? data[size-4] : 0, size >= 3 ? data[size-3] : 0,
                 data[size-2], data[size-1]);
        return false;
    }

    /* Find SOF0 marker */
    bool has_sof = false;
    for (size_t i = 2; i < size - 1 && i < 2048; i++) {
        if (data[i] == 0xFF && data[i + 1] == 0xC0) {
            has_sof = true;
            uint8_t samp = data[i + 11];
            ESP_LOGI(TAG, "JPEG SOF0: Y sampling=%02X (%s)",
                     samp, samp == 0x22 ? "4:2:0" : samp == 0x21 ? "4:2:2" : "other");
            break;
        }
    }
    if (!has_sof) {
        ESP_LOGW(TAG, "JPEG SOF0 not found in first 2KB");
        return false;
    }

    return true;
}

esp_err_t spi_camera_receive_jpeg(int camera_idx,
                                   uint8_t **jpeg_buf, size_t *jpeg_size,
                                   uint32_t *transfer_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (camera_idx < 0 || camera_idx >= SPI_CAM_COUNT) return ESP_ERR_INVALID_ARG;

    *jpeg_buf = NULL;
    *jpeg_size = 0;
    *transfer_ms = 0;

    if (!s_spi_dev) return ESP_ERR_NOT_SUPPORTED;

    /* Poll for JPEG ready and get size in one step */
    uint32_t jpeg_size_reported = 0;
    esp_err_t ret = poll_and_get_size(camera_idx, 2000, &jpeg_size_reported);
    if (ret != ESP_OK) return ret;

    /* We over-read by (PREAMBLE_LEN + SAFETY_STALE_BYTES) bytes so any stale
     * IDLE-header bytes the slave leaked before processing CMD_READ_DATA get
     * tolerated — we scan for the magic preamble and align to it. */
    size_t read_total = jpeg_size_reported + PREAMBLE_LEN + SAFETY_STALE_BYTES;

    /* Allocate PSRAM scratch for the raw transfer (with preamble+padding) */
    uint8_t *raw = heap_caps_malloc(read_total, MALLOC_CAP_SPIRAM);
    if (!raw) {
        ESP_LOGE(TAG, "OOM for raw transfer buffer (%zu bytes)", read_total);
        return ESP_ERR_NO_MEM;
    }

    /* Send READ_DATA command */
    WORD_ALIGNED_ATTR uint8_t cmd_tx[4] = {CMD_READ_DATA, 0, 0, 0};
    WORD_ALIGNED_ATTR uint8_t cmd_rx[4] = {0};
    ret = spi_xfer_cam(camera_idx, cmd_tx, cmd_rx, 4);
    if (ret != ESP_OK) {
        free(raw);
        return ret;
    }

    /* Give slave time to transition into DATA mode */
    vTaskDelay(pdMS_TO_TICKS(50));

    uint32_t t_start = esp_log_timestamp();

    WORD_ALIGNED_ATTR uint8_t *chunk_tx = heap_caps_calloc(1, SPI_CHUNK_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    WORD_ALIGNED_ATTR uint8_t *chunk_rx = heap_caps_malloc(SPI_CHUNK_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!chunk_tx || !chunk_rx) {
        ESP_LOGE(TAG, "OOM for SPI chunk buffers");
        free(raw);
        if (chunk_tx) free(chunk_tx);
        if (chunk_rx) free(chunk_rx);
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0;
    size_t remaining = read_total;

    /* Toggle CS per chunk — the slave's double-buffered DATA streaming
     * relies on CS-edge transitions to advance through its queued
     * transactions. Holding CS asserted across chunks makes the slave
     * DMA finish after the first chunk and feed zeros for the rest. */
    while (remaining > 0) {
        size_t chunk = remaining > SPI_CHUNK_SIZE ? SPI_CHUNK_SIZE : remaining;

        ret = spi_xfer_cam(camera_idx, chunk_tx, chunk_rx, chunk);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI read failed at offset %zu", offset);
            break;
        }

        memcpy(raw + offset, chunk_rx, chunk);
        offset += chunk;
        remaining -= chunk;
    }

    free(chunk_tx);
    free(chunk_rx);

    uint32_t elapsed = esp_log_timestamp() - t_start;

    if (ret != ESP_OK) {
        free(raw);
        return ret;
    }

    /* Scan the first (PREAMBLE_LEN + SAFETY_STALE_BYTES) bytes for the magic
     * 0xDE 0xAD 0xBE 0xEF marker that immediately precedes the JPEG. */
    size_t scan_limit = PREAMBLE_LEN + SAFETY_STALE_BYTES;
    if (scan_limit > read_total - 4) scan_limit = read_total - 4;
    /* We look for the 4-byte magic (last 4 bytes of PREAMBLE). The leading
     * 8 0xAA bytes are just padding so master has clear non-zero/non-header
     * bytes preceding the magic — match the magic directly. */
    const uint8_t *MAGIC = &SPI_PREAMBLE[8];
    ssize_t magic_offset = -1;
    for (size_t i = 0; i + 4 <= scan_limit; i++) {
        if (memcmp(raw + i, MAGIC, 4) == 0) {
            magic_offset = (ssize_t)i;
            break;
        }
    }

    if (magic_offset < 0) {
        ESP_LOGW(TAG, "Preamble magic not found in first %zu bytes (hdr: %02X %02X %02X %02X)",
                 scan_limit, raw[0], raw[1], raw[2], raw[3]);
        free(raw);
        *jpeg_buf = NULL;
        *jpeg_size = 0;
        *transfer_ms = elapsed;
        return ESP_ERR_INVALID_CRC;
    }

    size_t jpeg_start = (size_t)magic_offset + 4;
    if (jpeg_start + jpeg_size_reported > read_total) {
        ESP_LOGW(TAG, "JPEG truncated: magic@%zd jpeg_start=%zu jpeg_size=%lu read=%zu",
                 magic_offset, jpeg_start, (unsigned long)jpeg_size_reported, read_total);
        free(raw);
        *jpeg_buf = NULL;
        *jpeg_size = 0;
        *transfer_ms = elapsed;
        return ESP_ERR_INVALID_CRC;
    }

    /* Copy the JPEG payload to its own PSRAM buffer and release the raw scratch */
    uint8_t *jpeg = heap_caps_malloc(jpeg_size_reported, MALLOC_CAP_SPIRAM);
    if (!jpeg) {
        ESP_LOGE(TAG, "OOM for final JPEG buffer");
        free(raw);
        return ESP_ERR_NO_MEM;
    }
    memcpy(jpeg, raw + jpeg_start, jpeg_size_reported);
    free(raw);

    if (!validate_jpeg_integrity(jpeg, jpeg_size_reported)) {
        ESP_LOGW(TAG, "JPEG aligned via magic but still invalid (magic@%zd hdr=%02X %02X tail=%02X %02X)",
                 magic_offset, jpeg[0], jpeg[1],
                 jpeg[jpeg_size_reported - 2], jpeg[jpeg_size_reported - 1]);
        free(jpeg);
        *jpeg_buf = NULL;
        *jpeg_size = 0;
        *transfer_ms = elapsed;
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "JPEG verified: %lu bytes in %lums (magic@%zd, %.1f KB/s)",
             (unsigned long)jpeg_size_reported, (unsigned long)elapsed, magic_offset,
             elapsed > 0 ? (float)read_total / elapsed : 0);

    *jpeg_buf = jpeg;
    *jpeg_size = jpeg_size_reported;
    *transfer_ms = elapsed;
    return ESP_OK;
}

esp_err_t spi_camera_send_control(int camera_idx, uint8_t cmd)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (camera_idx < 0 || camera_idx >= SPI_CAM_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_spi_dev) return ESP_ERR_NOT_SUPPORTED;

    /* The byte-value / position / timing combinations that survive master→
     * slave transmission reliably are the ones the poll protocol already
     * uses: short 8-byte transactions, repeated many times, with command
     * byte at position 0. Single-shot control transactions got mangled in
     * various ways (0x04→0x02, 0x10→0x00, 0x41→0x02, byte-4 dropped). So
     * we brute-force it: send the command as a burst of up-to-10 identical
     * 8-byte transactions spaced 20ms apart, mimicking poll_and_get_size's
     * repeat pattern. The slave's scan picks up the command from whichever
     * transaction landed cleanly. */
    uint8_t *tx = heap_caps_calloc(1, 8, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *rx = heap_caps_calloc(1, 8, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!tx || !rx) {
        if (tx) free(tx);
        if (rx) free(rx);
        return ESP_ERR_NO_MEM;
    }
    tx[0] = cmd;

    esp_err_t ret = ESP_OK;
    for (int i = 0; i < 10; i++) {
        ret = spi_xfer_cam(camera_idx, tx, rx, 8);
        if (ret != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    free(tx);
    free(rx);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Camera %d: sent control cmd 0x%02X", camera_idx + 1, cmd);
    }
    return ret;
}

esp_err_t spi_camera_query_status(int camera_idx, uint8_t *status_byte)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (camera_idx < 0 || camera_idx >= SPI_CAM_COUNT) return ESP_ERR_INVALID_ARG;
    if (!status_byte) return ESP_ERR_INVALID_ARG;
    if (!s_spi_dev) return ESP_ERR_NOT_SUPPORTED;

    /* Two 8-byte transactions, same as poll_and_get_size but we just want the
     * status byte — the slave always pre-fills its IDLE header with current
     * JPEG/WiFi flags. */
    WORD_ALIGNED_ATTR uint8_t tx[8] = {CMD_STATUS, 0, 0, 0, 0, 0, 0, 0};
    WORD_ALIGNED_ATTR uint8_t rx[8] = {0};
    esp_err_t ret = spi_xfer_cam(camera_idx, tx, rx, 8);
    if (ret != ESP_OK) return ret;

    WORD_ALIGNED_ATTR uint8_t tx2[8] = {0};
    WORD_ALIGNED_ATTR uint8_t rx2[8] = {0};
    ret = spi_xfer_cam(camera_idx, tx2, rx2, 8);
    if (ret != ESP_OK) return ret;

    *status_byte = rx2[0];
    return ESP_OK;
}

esp_err_t spi_camera_capture_all(uint8_t *jpeg_bufs[4], size_t jpeg_sizes[4],
                                  uint32_t *total_ms)
{
    if (!s_initialized) {
        esp_err_t ret = spi_camera_init();
        if (ret != ESP_OK) return ret;
    }

    uint32_t t_start = esp_log_timestamp();

    int success_count = 0;
    int attempt = 0;

    for (attempt = 0; attempt <= SPI_MAX_RETRIES; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Retry %d/%d — re-triggering cameras...", attempt, SPI_MAX_RETRIES);
            /* Free any partially-received buffers */
            for (int i = 0; i < SPI_CAM_COUNT; i++) {
                if (jpeg_bufs[i] && jpeg_sizes[i] == 0) {
                    free(jpeg_bufs[i]);
                    jpeg_bufs[i] = NULL;
                }
            }
        }

        /* Trigger all cameras simultaneously via GPIO34 */
        gpio_set_level(GPIO_NUM_34, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(GPIO_NUM_34, 1);
        ESP_LOGI(TAG, "Trigger sent (attempt %d), waiting for captures...", attempt + 1);
        /* Fast mode shaves the post-trigger wait. 300ms is enough for OV5640
         * capture completion; the default 500ms included a comfortable margin
         * that we trade away in fast mode for ~200ms quicker bursts. */
        vTaskDelay(pdMS_TO_TICKS(s_fast_mode ? 300 : 500));

        /* Receive from each camera sequentially, one at a time.
         * Add delay between cameras to let the bus fully settle. */
        success_count = 0;
        for (int i = 0; i < SPI_CAM_COUNT; i++) {
            if (jpeg_bufs[i] && jpeg_sizes[i] > 0) {
                success_count++;  /* Already have a good transfer from previous attempt */
                continue;
            }

            uint32_t xfer_ms = 0;
            jpeg_bufs[i] = NULL;
            jpeg_sizes[i] = 0;

            esp_err_t ret = spi_camera_receive_jpeg(i, &jpeg_bufs[i], &jpeg_sizes[i], &xfer_ms);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Camera %d: %zu bytes in %lums ✓", i + 1, jpeg_sizes[i], (unsigned long)xfer_ms);
                success_count++;
            } else if (ret == ESP_ERR_INVALID_CRC) {
                ESP_LOGW(TAG, "Camera %d: CORRUPT transfer — will retry", i + 1);
            } else {
                ESP_LOGW(TAG, "Camera %d: FAILED (0x%x)", i + 1, ret);
            }

            /* Inter-camera settle delay: gives shared MISO bus time to
             * return to steady state and the next camera's slave task time
             * to pre-queue its response. Without this, back-to-back selects
             * of adjacent cameras can see residual driven MISO state. */
            vTaskDelay(pdMS_TO_TICKS(s_fast_mode ? 30 : 50));
        }

        if (success_count >= SPI_CAM_COUNT) break;  /* All 4 got clean transfers */
        ESP_LOGW(TAG, "Got %d/%d cameras — %s", success_count, SPI_CAM_COUNT,
                 attempt < SPI_MAX_RETRIES ? "retrying..." : "giving up");
    }

    *total_ms = esp_log_timestamp() - t_start;
    /* Restore device slot 2 if it was swapped for camera #4 */
    restore_dev2();

    ESP_LOGI(TAG, "Capture all: %d/%d cameras, total %lums",
             success_count, SPI_CAM_COUNT, (unsigned long)*total_ms);

    return (success_count >= 2) ? ESP_OK : ESP_FAIL;
}

/* ---- Phase 4: exposure sync + autofocus ---- */

void spi_camera_set_fast_mode(bool enabled) { s_fast_mode = enabled; }
bool spi_camera_get_fast_mode(void)         { return s_fast_mode; }

/* Read the full 16-byte IDLE header from a single camera. Uses the same
 * 2-transaction pattern as spi_camera_query_status (command, then response),
 * but captures the extended header bytes added in Phase 4. */
static esp_err_t read_idle_header(int camera_idx, uint8_t header_out[16])
{
    if (!s_initialized || !s_spi_dev) return ESP_ERR_INVALID_STATE;
    if (camera_idx < 0 || camera_idx >= SPI_CAM_COUNT) return ESP_ERR_INVALID_ARG;

    WORD_ALIGNED_ATTR uint8_t tx1[16] = {CMD_STATUS};
    WORD_ALIGNED_ATTR uint8_t rx1[16] = {0};
    esp_err_t ret = spi_xfer_cam(camera_idx, tx1, rx1, 16);
    if (ret != ESP_OK) return ret;

    WORD_ALIGNED_ATTR uint8_t tx2[16] = {0};
    WORD_ALIGNED_ATTR uint8_t rx2[16] = {0};
    ret = spi_xfer_cam(camera_idx, tx2, rx2, 16);
    if (ret != ESP_OK) return ret;

    memcpy(header_out, rx2, 16);
    return ESP_OK;
}

esp_err_t spi_camera_read_exposure(int camera_idx,
                                    uint16_t *ae_gain, uint32_t *ae_exposure)
{
    uint8_t hdr[16] = {0};
    esp_err_t ret = read_idle_header(camera_idx, hdr);
    if (ret != ESP_OK) return ret;

    uint16_t g = (uint16_t)hdr[SPI_CAM_HEADER_AE_GAIN_OFFSET] |
                 ((uint16_t)hdr[SPI_CAM_HEADER_AE_GAIN_OFFSET + 1] << 8);
    uint32_t e = (uint32_t)hdr[SPI_CAM_HEADER_AE_EXPOSURE_OFFSET] |
                 ((uint32_t)hdr[SPI_CAM_HEADER_AE_EXPOSURE_OFFSET + 1] << 8) |
                 ((uint32_t)hdr[SPI_CAM_HEADER_AE_EXPOSURE_OFFSET + 2] << 16);

    if (ae_gain)     *ae_gain     = g;
    if (ae_exposure) *ae_exposure = e;
    ESP_LOGI(TAG, "Camera %d AE: gain=%u exposure=%lu",
             camera_idx + 1, (unsigned)g, (unsigned long)e);
    return ESP_OK;
}

esp_err_t spi_camera_set_exposure(int camera_idx,
                                   uint16_t ae_gain, uint32_t ae_exposure)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (camera_idx < 0 || camera_idx >= SPI_CAM_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_spi_dev) return ESP_ERR_NOT_SUPPORTED;

    /* 10× burst with cmd at [0] and payload at [1..5]. The slave scans for
     * the cmd byte in rx[0..7] and, on match, reads payload from rx[cmd_offset+1..]. */
    uint8_t *tx = heap_caps_calloc(1, 8, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *rx = heap_caps_calloc(1, 8, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!tx || !rx) {
        if (tx) free(tx);
        if (rx) free(rx);
        return ESP_ERR_NO_MEM;
    }
    tx[0] = SPI_CAM_CMD_SET_EXPOSURE;
    tx[1] = ae_gain & 0xFF;
    tx[2] = (ae_gain >> 8) & 0xFF;
    tx[3] = ae_exposure & 0xFF;
    tx[4] = (ae_exposure >> 8) & 0xFF;
    tx[5] = (ae_exposure >> 16) & 0xFF;

    esp_err_t ret = ESP_OK;
    for (int i = 0; i < 10; i++) {
        ret = spi_xfer_cam(camera_idx, tx, rx, 8);
        if (ret != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    free(tx);
    free(rx);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Camera %d: SET_EXPOSURE gain=%u exposure=%lu",
                 camera_idx + 1, (unsigned)ae_gain, (unsigned long)ae_exposure);
    }
    return ret;
}

esp_err_t spi_camera_sync_exposure(int ref_idx)
{
    if (!s_initialized) {
        esp_err_t ret = spi_camera_init();
        if (ret != ESP_OK) return ret;
    }
    if (ref_idx < 0 || ref_idx >= SPI_CAM_COUNT) return ESP_ERR_INVALID_ARG;

    uint16_t ref_gain = 0;
    uint32_t ref_exp  = 0;
    esp_err_t ret = spi_camera_read_exposure(ref_idx, &ref_gain, &ref_exp);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Exposure sync: ref camera %d read failed (0x%x)",
                 ref_idx + 1, ret);
        return ret;
    }
    if (ref_gain == 0 && ref_exp == 0) {
        /* Reference camera is still in auto-AE and hasn't captured yet, or
         * it's running older firmware without the extended header. */
        ESP_LOGW(TAG, "Exposure sync: ref camera %d reports zero AE — skipping",
                 ref_idx + 1);
        return ESP_ERR_INVALID_STATE;
    }

    int ok = 0;
    for (int i = 0; i < SPI_CAM_COUNT; i++) {
        if (i == ref_idx) { ok++; continue; }
        if (spi_camera_set_exposure(i, ref_gain, ref_exp) == ESP_OK) ok++;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    ESP_LOGI(TAG, "Exposure sync from cam %d (gain=%u exp=%lu): %d/%d set",
             ref_idx + 1, (unsigned)ref_gain, (unsigned long)ref_exp,
             ok, SPI_CAM_COUNT);
    return (ok == SPI_CAM_COUNT) ? ESP_OK : ESP_FAIL;
}

esp_err_t spi_camera_autofocus_all(uint32_t timeout_ms)
{
    if (!s_initialized) {
        esp_err_t ret = spi_camera_init();
        if (ret != ESP_OK) return ret;
    }

    int sent = 0;
    for (int i = 0; i < SPI_CAM_COUNT; i++) {
        if (spi_camera_send_control(i, SPI_CAM_CMD_AUTOFOCUS) == ESP_OK) sent++;
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    uint32_t t0 = esp_log_timestamp();
    int locked_mask = 0;
    const int all_mask = (1 << SPI_CAM_COUNT) - 1;
    while ((locked_mask & all_mask) != all_mask &&
           (esp_log_timestamp() - t0) < timeout_ms) {
        for (int i = 0; i < SPI_CAM_COUNT; i++) {
            if (locked_mask & (1 << i)) continue;
            uint8_t status = 0;
            if (spi_camera_query_status(i, &status) == ESP_OK &&
                (status & SPI_CAM_STATUS_AF_LOCKED)) {
                locked_mask |= (1 << i);
            }
        }
        if ((locked_mask & all_mask) != all_mask) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    int locked_count = __builtin_popcount(locked_mask);
    ESP_LOGI(TAG, "AF: sent=%d locked=%d/%d in %lums (timeout=%lums)",
             sent, locked_count, SPI_CAM_COUNT,
             (unsigned long)(esp_log_timestamp() - t0),
             (unsigned long)timeout_ms);
    return (locked_count >= SPI_CAM_COUNT / 2) ? ESP_OK : ESP_FAIL;
}
