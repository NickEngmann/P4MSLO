/**
 * @brief esp_vfs_fat.h mock — just enough for the simulator to link
 *        ui_extra.c's format BG task. The simulator never actually
 *        formats anything; this is a no-op stub.
 */
#pragma once

#include "esp_err.h"
#include "bsp/esp32_p4_eye.h"   /* sdmmc_card_t */

static inline esp_err_t esp_vfs_fat_sdcard_format(const char *base_path,
                                                   sdmmc_card_t *card) {
    (void)base_path; (void)card;
    return ESP_OK;
}
