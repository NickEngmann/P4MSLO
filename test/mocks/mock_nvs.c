#include "mock_nvs.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// In-memory NVS implementation with 64-entry store
static struct {
    char key[32];
    uint8_t type;
    union {
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        int32_t i32;
        uint64_t u64;
        int64_t i64;
        char str[256];
        void *ptr;
    } data;
    size_t size;
    bool valid;
} nvs_store[64];

static int nvs_count = 0;
static const char *current_namespace = "p4mslo";

static int find_entry(const char *key) {
    for (int i = 0; i < nvs_count; i++) {
        if (strcmp(nvs_store[i].key, key) == 0 && nvs_store[i].valid) {
            return i;
        }
    }
    return -1;
}

esp_err_t nvs_open(const char *label, nvs_access_mode_t mode, nvs_handle_t *handle) {
    if (!label || !handle) return ESP_ERR_INVALID_ARG;
    if (strcmp(label, current_namespace) != 0) return ESP_ERR_NVS_NOT_INITIALIZED;
    *handle = 1; // Simple handle
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
    // No-op for mock
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    int idx = find_entry(key);
    if (idx >= 0) {
        nvs_store[idx].data.u8 = value;
        nvs_store[idx].size = sizeof(uint8_t);
        return ESP_OK;
    }
    if (nvs_count >= 64) return ESP_ERR_NVS_NO_FREE_SPACE;
    int new_idx = nvs_count++;
    strncpy(nvs_store[new_idx].key, key, 31);
    nvs_store[new_idx].key[31] = '\0';
    nvs_store[new_idx].data.u8 = value;
    nvs_store[new_idx].size = sizeof(uint8_t);
    nvs_store[new_idx].type = NVS_TYPE_U8;
    nvs_store[new_idx].valid = true;
    return ESP_OK;
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value) {
    int idx = find_entry(key);
    if (idx >= 0) {
        nvs_store[idx].data.u16 = value;
        nvs_store[idx].size = sizeof(uint16_t);
        return ESP_OK;
    }
    if (nvs_count >= 64) return ESP_ERR_NVS_NO_FREE_SPACE;
    int new_idx = nvs_count++;
    strncpy(nvs_store[new_idx].key, key, 31);
    nvs_store[new_idx].key[31] = '\0';
    nvs_store[new_idx].data.u16 = value;
    nvs_store[new_idx].size = sizeof(uint16_t);
    nvs_store[new_idx].type = NVS_TYPE_U16;
    nvs_store[new_idx].valid = true;
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value) {
    int idx = find_entry(key);
    if (idx >= 0) {
        nvs_store[idx].data.u32 = value;
        nvs_store[idx].size = sizeof(uint32_t);
        return ESP_OK;
    }
    if (nvs_count >= 64) return ESP_ERR_NVS_NO_FREE_SPACE;
    int new_idx = nvs_count++;
    strncpy(nvs_store[new_idx].key, key, 31);
    nvs_store[new_idx].key[31] = '\0';
    nvs_store[new_idx].data.u32 = value;
    nvs_store[new_idx].size = sizeof(uint32_t);
    nvs_store[new_idx].type = NVS_TYPE_U32;
    nvs_store[new_idx].valid = true;
    return ESP_OK;
}

esp_err_t nvs_set_i32(nvs_handle_t handle, const char *key, int32_t value) {
    int idx = find_entry(key);
    if (idx >= 0) {
        nvs_store[idx].data.i32 = value;
        nvs_store[idx].size = sizeof(int32_t);
        return ESP_OK;
    }
    if (nvs_count >= 64) return ESP_ERR_NVS_NO_FREE_SPACE;
    int new_idx = nvs_count++;
    strncpy(nvs_store[new_idx].key, key, 31);
    nvs_store[new_idx].key[31] = '\0';
    nvs_store[new_idx].data.i32 = value;
    nvs_store[new_idx].size = sizeof(int32_t);
    nvs_store[new_idx].type = NVS_TYPE_I32;
    nvs_store[new_idx].valid = true;
    return ESP_OK;
}

esp_err_t nvs_set_u64(nvs_handle_t handle, const char *key, uint64_t value) {
    int idx = find_entry(key);
    if (idx >= 0) {
        nvs_store[idx].data.u64 = value;
        nvs_store[idx].size = sizeof(uint64_t);
        return ESP_OK;
    }
    if (nvs_count >= 64) return ESP_ERR_NVS_NO_FREE_SPACE;
    int new_idx = nvs_count++;
    strncpy(nvs_store[new_idx].key, key, 31);
    nvs_store[new_idx].key[31] = '\0';
    nvs_store[new_idx].data.u64 = value;
    nvs_store[new_idx].size = sizeof(uint64_t);
    nvs_store[new_idx].type = NVS_TYPE_U64;
    nvs_store[new_idx].valid = true;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value) {
    int idx = find_entry(key);
    if (idx >= 0) {
        strncpy(nvs_store[idx].data.str, value, 255);
        nvs_store[idx].data.str[255] = '\0';
        nvs_store[idx].size = strlen(value) + 1;
        return ESP_OK;
    }
    if (nvs_count >= 64) return ESP_ERR_NVS_NO_FREE_SPACE;
    int new_idx = nvs_count++;
    strncpy(nvs_store[new_idx].key, key, 31);
    nvs_store[new_idx].key[31] = '\0';
    strncpy(nvs_store[new_idx].data.str, value, 255);
    nvs_store[new_idx].data.str[255] = '\0';
    nvs_store[new_idx].size = strlen(value) + 1;
    nvs_store[new_idx].type = NVS_TYPE_STR;
    nvs_store[new_idx].valid = true;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value) {
    int idx = find_entry(key);
    if (idx < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (nvs_store[idx].type != NVS_TYPE_U8) return ESP_ERR_NVS_TYPE_MISMATCH;
    *value = nvs_store[idx].data.u8;
    return ESP_OK;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value) {
    int idx = find_entry(key);
    if (idx < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (nvs_store[idx].type != NVS_TYPE_U16) return ESP_ERR_NVS_TYPE_MISMATCH;
    *value = nvs_store[idx].data.u16;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *value) {
    int idx = find_entry(key);
    if (idx < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (nvs_store[idx].type != NVS_TYPE_U32) return ESP_ERR_NVS_TYPE_MISMATCH;
    *value = nvs_store[idx].data.u32;
    return ESP_OK;
}

esp_err_t nvs_get_i32(nvs_handle_t handle, const char *key, int32_t *value) {
    int idx = find_entry(key);
    if (idx < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (nvs_store[idx].type != NVS_TYPE_I32) return ESP_ERR_NVS_TYPE_MISMATCH;
    *value = nvs_store[idx].data.i32;
    return ESP_OK;
}

esp_err_t nvs_get_u64(nvs_handle_t handle, const char *key, uint64_t *value) {
    int idx = find_entry(key);
    if (idx < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (nvs_store[idx].type != NVS_TYPE_U64) return ESP_ERR_NVS_TYPE_MISMATCH;
    *value = nvs_store[idx].data.u64;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *length) {
    int idx = find_entry(key);
    if (idx < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (nvs_store[idx].type != NVS_TYPE_STR) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (value) {
        strncpy(value, nvs_store[idx].data.str, *length ? *length - 1 : 255);
        if (*length) value[*length - 1] = '\0';
        else strncpy(value, nvs_store[idx].data.str, 255);
    }
    if (length) *length = strlen(nvs_store[idx].data.str) + 1;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    return ESP_OK; // No-op for mock
}

void nvs_flash_init(void) {
    // No-op for mock
}

esp_err_t nvs_flash_erase(void) {
    nvs_count = 0;
    memset(nvs_store, 0, sizeof(nvs_store));
    return ESP_OK;
}

size_t nvs_flash_get_partition_size(void) {
    return 1024 * 1024; // 1MB
}
