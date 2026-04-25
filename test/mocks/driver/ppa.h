/**
 * @brief ESP-IDF PPA driver mock.
 *
 * The encoder allocates a PPA client at create time but the decode
 * path uses tjpgd (PPA never actually drives a transform on this
 * pipeline). Stub everything to ESP_OK.
 */
#pragma once

#include "esp_err.h"

typedef void *ppa_client_handle_t;

#define PPA_OPERATION_SRM 0

typedef struct {
    int oper_type;
} ppa_client_config_t;

static inline esp_err_t ppa_register_client(const ppa_client_config_t *cfg, ppa_client_handle_t *out) {
    (void)cfg; *out = (ppa_client_handle_t)1; return ESP_OK;
}
static inline esp_err_t ppa_unregister_client(ppa_client_handle_t h) { (void)h; return ESP_OK; }
