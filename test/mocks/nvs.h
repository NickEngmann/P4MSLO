/**
 * @brief NVS (Non-Volatile Storage) mock for host-based testing
 *
 * Provides an in-memory key-value store that simulates ESP-IDF NVS behavior.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY,
    NVS_READWRITE
} nvs_open_mode_t;

/* In-memory NVS storage */
#define MOCK_NVS_MAX_ENTRIES 64
#define MOCK_NVS_MAX_KEY_LEN 16
#define MOCK_NVS_MAX_NS_LEN 16

typedef enum {
    NVS_TYPE_U8,
    NVS_TYPE_U16,
    NVS_TYPE_U32,
    NVS_TYPE_I8,
    NVS_TYPE_I16,
    NVS_TYPE_I32,
    NVS_TYPE_STR,
    NVS_TYPE_BLOB
} nvs_type_t;

typedef struct {
    char namespace_name[MOCK_NVS_MAX_NS_LEN];
    char key[MOCK_NVS_MAX_KEY_LEN];
    nvs_type_t type;
    union {
        uint8_t  u8;
        uint16_t u16;
        uint32_t u32;
        int8_t   i8;
        int16_t  i16;
        int32_t  i32;
    } value;
    bool in_use;
} mock_nvs_entry_t;

/* Global mock NVS state */
static mock_nvs_entry_t mock_nvs_store[MOCK_NVS_MAX_ENTRIES];
static int mock_nvs_initialized = 0;
static char mock_nvs_current_ns[MOCK_NVS_MAX_NS_LEN] = {0};
static nvs_open_mode_t mock_nvs_current_mode = NVS_READONLY;
static uint32_t mock_nvs_handle_counter = 1;

static inline void mock_nvs_reset(void) {
    memset(mock_nvs_store, 0, sizeof(mock_nvs_store));
    mock_nvs_initialized = 0;
    mock_nvs_handle_counter = 1;
}

static inline mock_nvs_entry_t* mock_nvs_find(const char *ns, const char *key) {
    for (int i = 0; i < MOCK_NVS_MAX_ENTRIES; i++) {
        if (mock_nvs_store[i].in_use &&
            strcmp(mock_nvs_store[i].namespace_name, ns) == 0 &&
            strcmp(mock_nvs_store[i].key, key) == 0) {
            return &mock_nvs_store[i];
        }
    }
    return NULL;
}

static inline mock_nvs_entry_t* mock_nvs_alloc(const char *ns, const char *key) {
    mock_nvs_entry_t *entry = mock_nvs_find(ns, key);
    if (entry) return entry;
    for (int i = 0; i < MOCK_NVS_MAX_ENTRIES; i++) {
        if (!mock_nvs_store[i].in_use) {
            strncpy(mock_nvs_store[i].namespace_name, ns, MOCK_NVS_MAX_NS_LEN - 1);
            strncpy(mock_nvs_store[i].key, key, MOCK_NVS_MAX_KEY_LEN - 1);
            mock_nvs_store[i].in_use = true;
            return &mock_nvs_store[i];
        }
    }
    return NULL;
}

/* NVS API */
static inline esp_err_t nvs_flash_init(void) {
    mock_nvs_initialized = 1;
    return ESP_OK;
}

static inline esp_err_t nvs_flash_erase(void) {
    mock_nvs_reset();
    mock_nvs_initialized = 1;
    return ESP_OK;
}

static inline esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle) {
    strncpy(mock_nvs_current_ns, namespace_name, MOCK_NVS_MAX_NS_LEN - 1);
    mock_nvs_current_mode = open_mode;
    *out_handle = mock_nvs_handle_counter++;
    return ESP_OK;
}

static inline void nvs_close(nvs_handle_t handle) {
    (void)handle;
}

static inline esp_err_t nvs_commit(nvs_handle_t handle) {
    (void)handle;
    return ESP_OK;
}

/* Setters */
static inline esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    (void)handle;
    mock_nvs_entry_t *e = mock_nvs_alloc(mock_nvs_current_ns, key);
    if (!e) return ESP_ERR_NO_MEM;
    e->type = NVS_TYPE_U8;
    e->value.u8 = value;
    return ESP_OK;
}

static inline esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value) {
    (void)handle;
    mock_nvs_entry_t *e = mock_nvs_alloc(mock_nvs_current_ns, key);
    if (!e) return ESP_ERR_NO_MEM;
    e->type = NVS_TYPE_U16;
    e->value.u16 = value;
    return ESP_OK;
}

static inline esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value) {
    (void)handle;
    mock_nvs_entry_t *e = mock_nvs_alloc(mock_nvs_current_ns, key);
    if (!e) return ESP_ERR_NO_MEM;
    e->type = NVS_TYPE_U32;
    e->value.u32 = value;
    return ESP_OK;
}

/* Getters */
static inline esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value) {
    (void)handle;
    mock_nvs_entry_t *e = mock_nvs_find(mock_nvs_current_ns, key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->type != NVS_TYPE_U8) return ESP_ERR_NVS_TYPE_MISMATCH;
    *out_value = e->value.u8;
    return ESP_OK;
}

static inline esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *out_value) {
    (void)handle;
    mock_nvs_entry_t *e = mock_nvs_find(mock_nvs_current_ns, key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->type != NVS_TYPE_U16) return ESP_ERR_NVS_TYPE_MISMATCH;
    *out_value = e->value.u16;
    return ESP_OK;
}

static inline esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value) {
    (void)handle;
    mock_nvs_entry_t *e = mock_nvs_find(mock_nvs_current_ns, key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->type != NVS_TYPE_U32) return ESP_ERR_NVS_TYPE_MISMATCH;
    *out_value = e->value.u32;
    return ESP_OK;
}
