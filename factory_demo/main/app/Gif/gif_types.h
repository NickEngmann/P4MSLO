/**
 * @file gif_types.h
 * @brief Common types for GIF encoder/decoder
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t r, g, b;
} gif_color_t;

typedef struct {
    gif_color_t entries[256];
    int count;
} gif_palette_t;

#ifdef __cplusplus
}
#endif
