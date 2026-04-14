/**
 * @file      SDCardManager.h
 * @author    Moment
 * @license   MIT
 * @copyright Copyright (c) 2026 Moment
 *
 * FAT32 SD card manager for saving JPEG photos.
 */
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

class SDCardManager {
public:
    SDCardManager();
    ~SDCardManager();

    bool begin();
    bool isMounted() const { return _mounted; }

    // Save JPEG data to SD card. Returns the filename (e.g., "IMG_0001.jpg")
    std::string savePhoto(const uint8_t *jpegData, size_t len, uint32_t counter);

    // List all photos on SD card
    std::vector<std::string> listPhotos();

    // Get full path for a photo filename
    std::string getPhotoPath(const std::string &filename);

    // SD card stats
    uint64_t getFreeBytes();
    uint64_t getTotalBytes();

private:
    bool _mounted;
};
