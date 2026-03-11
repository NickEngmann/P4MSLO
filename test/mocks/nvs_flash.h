#ifndef NVS_FLASH_H
#define NVS_FLASH_H

#include "esp_err.h"

// NVS Flash API for non-volatile storage
esp_err_t nvs_open(const char *tag, nvs_open_mode_t mode, nvs_handle_t *handle);
void nvs_close(nvs_handle_t handle);

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *val);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *val);
esp_err_t nvs_get_u64(nvs_handle_t handle, const char *key, uint64_t *val);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *val, size_t *len);

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t val);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t val);
esp_err_t nvs_set_u64(nvs_handle_t handle, const char *key, uint64_t val);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *val);

esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_erase_all(nvs_handle_t handle);

#endif /* NVS_FLASH_H */
