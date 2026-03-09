/**
 * @brief I2C master driver mock
 */
#pragma once

#include "../esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef void* i2c_master_bus_handle_t;
typedef void* i2c_master_dev_handle_t;

typedef int i2c_clock_source_t;
#define I2C_CLK_SRC_DEFAULT 0

typedef struct {
    i2c_clock_source_t clk_source;
    int i2c_port;
    int scl_io_num;
    int sda_io_num;
    struct {
        bool enable_internal_pullup;
    } flags;
} i2c_master_bus_config_t;

static i2c_master_bus_handle_t mock_i2c_bus = (void*)0x1234;

static inline esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *handle) {
    (void)cfg;
    *handle = mock_i2c_bus;
    return ESP_OK;
}

static inline esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t handle) {
    (void)handle;
    return ESP_OK;
}
