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

/* SPI transfer chunk size */
#define SPI_CHUNK_SIZE    4096

/* SPI3 on ESP32-P4 supports max 3 hardware CS pins.
 * We register 3 devices for cameras 0-2, and for camera 3 we
 * remove device 2 temporarily and re-add with camera 3's CS pin. */
#define SPI_MAX_HW_CS  3

static spi_device_handle_t s_spi_dev[SPI_MAX_HW_CS];
static bool s_initialized = false;

static const int s_cs_pins[SPI_CAM_COUNT] = {
    SPI_CAM_CS0_PIN, SPI_CAM_CS1_PIN, SPI_CAM_CS2_PIN, SPI_CAM_CS3_PIN
};

/* For camera 3 (index 3), we need to swap device 2's CS pin */
static int s_dev2_current_cs = -1;

static spi_device_handle_t get_device(int cam_idx)
{
    if (cam_idx < SPI_MAX_HW_CS) {
        return s_spi_dev[cam_idx];
    }
    /* Camera 3 — reuse slot 2 by swapping CS */
    if (s_dev2_current_cs != s_cs_pins[cam_idx]) {
        spi_bus_remove_device(s_spi_dev[2]);
        spi_device_interface_config_t cfg = {
            .mode = 0,
            .clock_speed_hz = 10 * 1000 * 1000,
            .spics_io_num = s_cs_pins[cam_idx],
            .queue_size = 1,
        };
        spi_bus_add_device(SPI3_HOST, &cfg, &s_spi_dev[2]);
        s_dev2_current_cs = s_cs_pins[cam_idx];
    }
    return s_spi_dev[2];
}

static void restore_dev2(void)
{
    /* Restore device 2 to camera 2's CS pin if it was swapped */
    if (s_dev2_current_cs != s_cs_pins[2] && s_dev2_current_cs >= 0) {
        spi_bus_remove_device(s_spi_dev[2]);
        spi_device_interface_config_t cfg = {
            .mode = 0,
            .clock_speed_hz = 10 * 1000 * 1000,
            .spics_io_num = s_cs_pins[2],
            .queue_size = 1,
        };
        spi_bus_add_device(SPI3_HOST, &cfg, &s_spi_dev[2]);
        s_dev2_current_cs = s_cs_pins[2];
    }
}

esp_err_t spi_camera_init(void)
{
    if (s_initialized) return ESP_OK;

    /* Pre-drive ALL CS pins HIGH before any SPI init to prevent
     * bus contention from cameras with floating CS lines */
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

    /* Register 3 devices (SPI3 max) for cameras 0-2 */
    for (int i = 0; i < SPI_MAX_HW_CS; i++) {
        spi_device_interface_config_t dev_cfg = {
            .mode = 0,
            .clock_speed_hz = 10 * 1000 * 1000,
            .spics_io_num = s_cs_pins[i],
            .queue_size = 1,
        };
        ret = spi_bus_add_device(SPI3_HOST, &dev_cfg, &s_spi_dev[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI device %d (CS=%d) failed: 0x%x", i, s_cs_pins[i], ret);
            return ret;
        }
    }
    s_dev2_current_cs = s_cs_pins[2];

    s_initialized = true;
    ESP_LOGI(TAG, "SPI camera master: %d cameras (CS=%d,%d,%d,%d) @ 40MHz",
             SPI_CAM_COUNT, SPI_CAM_CS0_PIN, SPI_CAM_CS1_PIN, SPI_CAM_CS2_PIN, SPI_CAM_CS3_PIN);
    return ESP_OK;
}

/* Send a command and receive response in one full-duplex transaction */
static esp_err_t spi_xfer_dev(spi_device_handle_t dev, const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t trans = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_transmit(dev, &trans);
}

/**
 * Poll the slave's pre-filled header: [status(1), size_le(4)]
 * The slave ALWAYS has this ready in idle mode. The master reads it
 * alongside sending a dummy/status command. Because of the 1-transaction
 * lag, we do TWO transactions: first primes the slave, second gets the response.
 */
static esp_err_t poll_and_get_size(spi_device_handle_t dev, int timeout_ms, uint32_t *out_size)
{
    WORD_ALIGNED_ATTR uint8_t tx[8] = {CMD_STATUS, 0, 0, 0, 0, 0, 0, 0};
    WORD_ALIGNED_ATTR uint8_t rx[8] = {0};
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        /* Transaction 1: send status query, get PREVIOUS response (stale on first call) */
        esp_err_t ret = spi_xfer_dev(dev, tx, rx, 8);
        if (ret != ESP_OK) return ret;

        /* Transaction 2: send dummy, get the ACTUAL response from transaction 1 */
        WORD_ALIGNED_ATTR uint8_t tx2[8] = {0};
        WORD_ALIGNED_ATTR uint8_t rx2[8] = {0};
        ret = spi_xfer_dev(dev, tx2, rx2, 8);
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

esp_err_t spi_camera_receive_jpeg(int camera_idx,
                                   uint8_t **jpeg_buf, size_t *jpeg_size,
                                   uint32_t *transfer_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (camera_idx < 0 || camera_idx >= SPI_CAM_COUNT) return ESP_ERR_INVALID_ARG;

    *jpeg_buf = NULL;
    *jpeg_size = 0;
    *transfer_ms = 0;

    /* Get the SPI device handle for this camera */
    spi_device_handle_t dev = get_device(camera_idx);

    /* Poll for JPEG ready and get size in one step */
    uint32_t size = 0;
    esp_err_t ret = poll_and_get_size(dev, 5000, &size);
    if (ret != ESP_OK) return ret;

    /* Allocate PSRAM buffer for the JPEG */
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "OOM for JPEG buffer (%lu bytes)", (unsigned long)size);
        return ESP_ERR_NO_MEM;
    }

    /* Send READ_DATA command */
    WORD_ALIGNED_ATTR uint8_t cmd_tx[4] = {CMD_READ_DATA, 0, 0, 0};
    WORD_ALIGNED_ATTR uint8_t cmd_rx[4] = {0};
    ret = spi_xfer_dev(dev, cmd_tx, cmd_rx, 4);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }

    /* Give slave time to enter data transmit loop and queue first chunk.
     * Without this, the master clocks before the slave has data ready. */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Read JPEG data in chunks */
    uint32_t t_start = esp_log_timestamp();

    WORD_ALIGNED_ATTR uint8_t *chunk_tx = heap_caps_calloc(1, SPI_CHUNK_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    WORD_ALIGNED_ATTR uint8_t *chunk_rx = heap_caps_malloc(SPI_CHUNK_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!chunk_tx || !chunk_rx) {
        ESP_LOGE(TAG, "OOM for SPI chunk buffers");
        free(buf);
        if (chunk_tx) free(chunk_tx);
        if (chunk_rx) free(chunk_rx);
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0;
    size_t remaining = size;

    while (remaining > 0) {
        size_t chunk = remaining > SPI_CHUNK_SIZE ? SPI_CHUNK_SIZE : remaining;

        spi_transaction_t trans = {
            .length = chunk * 8,
            .tx_buffer = chunk_tx,  /* Dummy bytes to clock data out */
            .rx_buffer = chunk_rx,
        };

        ret = spi_device_transmit(dev, &trans);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI read failed at offset %zu", offset);
            break;
        }

        memcpy(buf + offset, chunk_rx, chunk);
        offset += chunk;
        remaining -= chunk;
    }

    free(chunk_tx);
    free(chunk_rx);

    uint32_t elapsed = esp_log_timestamp() - t_start;

    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }

    /* Validate JPEG header */
    if (size >= 2 && buf[0] == 0xFF && buf[1] == 0xD8) {
        ESP_LOGI(TAG, "JPEG received: %lu bytes in %lums (%.1f KB/s)",
                 (unsigned long)size, (unsigned long)elapsed,
                 elapsed > 0 ? (float)size / elapsed : 0);
    } else {
        ESP_LOGW(TAG, "Received %lu bytes but NOT a valid JPEG (header: 0x%02X 0x%02X)",
                 (unsigned long)size, buf[0], buf[1]);
    }

    *jpeg_buf = buf;
    *jpeg_size = size;
    *transfer_ms = elapsed;
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

    /* Trigger all cameras simultaneously via GPIO34 */
    gpio_set_level(GPIO_NUM_34, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(GPIO_NUM_34, 1);
    ESP_LOGI(TAG, "Trigger sent, waiting for captures...");
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Receive from each camera sequentially (they share the SPI bus) */
    int success_count = 0;
    for (int i = 0; i < SPI_CAM_COUNT; i++) {
        uint32_t xfer_ms = 0;
        jpeg_bufs[i] = NULL;
        jpeg_sizes[i] = 0;

        esp_err_t ret = spi_camera_receive_jpeg(i, &jpeg_bufs[i], &jpeg_sizes[i], &xfer_ms);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Camera %d: %zu bytes in %lums", i + 1, jpeg_sizes[i], (unsigned long)xfer_ms);
            success_count++;
        } else {
            ESP_LOGW(TAG, "Camera %d: FAILED (0x%x)", i + 1, ret);
        }
    }

    *total_ms = esp_log_timestamp() - t_start;
    ESP_LOGI(TAG, "Capture all: %d/%d cameras, total %lums",
             success_count, SPI_CAM_COUNT, (unsigned long)*total_ms);

    return (success_count == SPI_CAM_COUNT) ? ESP_OK : ESP_FAIL;
}
