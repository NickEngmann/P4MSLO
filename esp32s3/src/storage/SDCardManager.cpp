/**
 * @file      SDCardManager.cpp
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 */

#include "SDCardManager.h"
#include "../config/Config.h"

#ifndef NATIVE_BUILD
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <algorithm>

static const char *TAG = "sd";

SDCardManager::SDCardManager() : _mounted(false) {}
SDCardManager::~SDCardManager() {}

bool SDCardManager::begin() {
    // Configure SPI bus
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = SD_MOSI_PIN;
    bus_cfg.miso_io_num = SD_MISO_PIN;
    bus_cfg.sclk_io_num = SD_SCK_PIN;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 16384;

    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        // INVALID_STATE means already initialized (OK)
        ESP_LOGE(TAG, "SPI bus init failed: 0x%x", err);
        return false;
    }

    // Mount FAT32 filesystem
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = SDMMC_FREQ_PROBING;  // Start slow for old/small cards

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = (gpio_num_t)SD_CS_PIN;
    slot_config.host_id = SPI3_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = true;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 4 * 1024;  // Small allocation for 128MB cards

    sdmmc_card_t *card = nullptr;

    for (int attempt = 1; attempt <= 3; attempt++) {
        err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "SD mount attempt %d/3 failed: 0x%x", attempt, err);
        if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed after 3 attempts: 0x%x", err);
        return false;
    }

    _mounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, card);

    // Create DCIM directory if it doesn't exist
    struct stat st;
    if (stat(PHOTO_DIR, &st) != 0) {
        if (mkdir(PHOTO_DIR, 0775) != 0) {
            ESP_LOGW(TAG, "Failed to create %s, saving to root", PHOTO_DIR);
        } else {
            ESP_LOGI(TAG, "Created %s", PHOTO_DIR);
        }
    }

    return true;
}

std::string SDCardManager::savePhoto(const uint8_t *jpegData, size_t len, uint32_t counter) {
    if (!_mounted || !jpegData || len == 0) return "";

    char filename[32];
    snprintf(filename, sizeof(filename), "%s%04lu.jpg", PHOTO_PREFIX, (unsigned long)counter);

    char filepath[64];
    snprintf(filepath, sizeof(filepath), "%s/%s", PHOTO_DIR, filename);

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", filepath);
        return "";
    }

    size_t written = fwrite(jpegData, 1, len, f);
    fflush(f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "Write incomplete: %zu / %zu bytes", written, len);
        return "";
    }

    ESP_LOGI(TAG, "Saved %s (%zu bytes)", filepath, len);
    return std::string(filename);
}

std::vector<std::string> SDCardManager::listPhotos() {
    std::vector<std::string> photos;
    if (!_mounted) return photos;

    DIR *dir = opendir(PHOTO_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open %s", PHOTO_DIR);
        return photos;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        // Only list JPEG files
        if (name.size() > 4 && name.substr(name.size() - 4) == ".jpg") {
            photos.push_back(name);
        }
    }
    closedir(dir);

    // Sort alphabetically (IMG_0001, IMG_0002, ...)
    std::sort(photos.begin(), photos.end());
    return photos;
}

std::string SDCardManager::getPhotoPath(const std::string &filename) {
    return std::string(PHOTO_DIR) + "/" + filename;
}

uint64_t SDCardManager::getFreeBytes() {
    if (!_mounted) return 0;
    FATFS *fs;
    DWORD fre_clust;
    if (f_getfree("0:", &fre_clust, &fs) != FR_OK) return 0;
    return (uint64_t)fre_clust * fs->csize * 512;
}

uint64_t SDCardManager::getTotalBytes() {
    if (!_mounted) return 0;
    FATFS *fs;
    DWORD fre_clust;
    if (f_getfree("0:", &fre_clust, &fs) != FR_OK) return 0;
    return (uint64_t)(fs->n_fatent - 2) * fs->csize * 512;
}

#else  // NATIVE_BUILD

SDCardManager::SDCardManager() : _mounted(false) {}
SDCardManager::~SDCardManager() {}
bool SDCardManager::begin() { return false; }
std::string SDCardManager::savePhoto(const uint8_t*, size_t, uint32_t) { return ""; }
std::vector<std::string> SDCardManager::listPhotos() { return {}; }
std::string SDCardManager::getPhotoPath(const std::string &f) { return f; }
uint64_t SDCardManager::getFreeBytes() { return 0; }
uint64_t SDCardManager::getTotalBytes() { return 0; }

#endif
