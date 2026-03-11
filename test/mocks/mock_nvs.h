#ifndef MOCK_NVS_H
#define MOCK_NVS_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t nvs_access_mode_t;
typedef uint32_t nvs_handle_t;

#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NVS_TYPE_U8 1
#define NVS_TYPE_U16 2
#define NVS_TYPE_U32 3
#define NVS_TYPE_I32 4
#define NVS_TYPE_U64 5
#define NVS_TYPE_STR 6

typedef struct {
    char key[32];
    uint8_t type;
    union {
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        int32_t i32;
        uint64_t u64;
        char str[256];
        void *ptr;
    } data;
    size_t size;
    bool valid;
} nvs_entry_t;

esp_err_t nvs_open(const char *label, nvs_access_mode_t mode, nvs_handle_t *handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value);
esp_err_t nvs_set_i32(nvs_handle_t handle, const char *key, int32_t value);
esp_err_t nvs_set_u64(nvs_handle_t handle, const char *key, uint64_t value);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value);
esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *value);
esp_err_t nvs_get_i32(nvs_handle_t handle, const char *key, int32_t *value);
esp_err_t nvs_get_u64(nvs_handle_t handle, const char *key, uint64_t *value);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *length);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
size_t nvs_flash_get_partition_size(void);
void mock_nvs_reset(void);

#endif /* MOCK_NVS_H */
