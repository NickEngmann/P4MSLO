/**
 * @file gif_decoder.c
 * @brief Streaming GIF decoder for playback
 *
 * Reads GIF file blocks sequentially, decodes LZW data frame by frame,
 * and converts palette-indexed pixels to RGB565 for LVGL display.
 */

#include "gif_decoder.h"
#include "gif_lzw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "gif_dec";

struct gif_decoder {
    FILE *fp;
    char *path;
    int width, height;
    gif_palette_t palette;
    long first_frame_offset;  /* File offset after header/GCT for reset */

    /* Per-frame state. LZW output lands in the first pixel_count bytes of
     * the caller-owned rgb565 buffer (see gif_decoder_next_frame). */
    int pixel_count;
};

/* ---- Helpers ---- */

static uint16_t read_le16(FILE *fp)
{
    uint8_t buf[2];
    fread(buf, 1, 2, fp);
    return buf[0] | ((uint16_t)buf[1] << 8);
}

static inline uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    /* Standard RGB565: RRRRRGGGGGGBBBBB, byte-swapped for LVGL */
    uint16_t px = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (px >> 8) | (px << 8);  /* Byte swap for LV_COLOR_16_SWAP */
}

/* ---- Parse GIF header ---- */

static esp_err_t parse_header(gif_decoder_t *dec)
{
    uint8_t sig[6];
    if (fread(sig, 1, 6, dec->fp) != 6) return ESP_FAIL;
    if (memcmp(sig, "GIF87a", 6) != 0 && memcmp(sig, "GIF89a", 6) != 0) {
        ESP_LOGE(TAG, "Not a GIF file");
        return ESP_FAIL;
    }

    /* Logical Screen Descriptor */
    dec->width = read_le16(dec->fp);
    dec->height = read_le16(dec->fp);

    uint8_t packed;
    fread(&packed, 1, 1, dec->fp);
    bool has_gct = (packed & 0x80) != 0;
    int gct_size = 1 << ((packed & 0x07) + 1);

    uint8_t bg, aspect;
    fread(&bg, 1, 1, dec->fp);
    fread(&aspect, 1, 1, dec->fp);

    /* Read Global Color Table */
    dec->palette.count = 0;
    if (has_gct) {
        for (int i = 0; i < gct_size; i++) {
            uint8_t rgb[3];
            fread(rgb, 1, 3, dec->fp);
            if (i < 256) {
                dec->palette.entries[i].r = rgb[0];
                dec->palette.entries[i].g = rgb[1];
                dec->palette.entries[i].b = rgb[2];
            }
        }
        dec->palette.count = (gct_size < 256) ? gct_size : 256;
    }

    dec->first_frame_offset = ftell(dec->fp);
    return ESP_OK;
}

/* Skip extension blocks */
static void skip_extension(FILE *fp)
{
    uint8_t label;
    fread(&label, 1, 1, fp);

    /* Skip sub-blocks */
    while (1) {
        uint8_t block_size;
        if (fread(&block_size, 1, 1, fp) != 1 || block_size == 0) break;
        fseek(fp, block_size, SEEK_CUR);
    }
}

/* ---- Public API ---- */

esp_err_t gif_decoder_open(const char *path, gif_decoder_t **out)
{
    gif_decoder_t *dec = calloc(1, sizeof(gif_decoder_t));
    if (!dec) return ESP_ERR_NO_MEM;

    dec->path = strdup(path);
    dec->fp = fopen(path, "rb");
    if (!dec->fp) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        free(dec->path);
        free(dec);
        return ESP_FAIL;
    }

    esp_err_t ret = parse_header(dec);
    if (ret != ESP_OK) {
        gif_decoder_close(dec);
        return ret;
    }

    /* No separate pixel-index buffer: gif_decoder_next_frame now decodes
     * LZW output (1 byte / pixel) directly into the first pixel_count bytes
     * of the caller's rgb565 output buffer, then converts in place backwards.
     * Saves 3.3 MB of PSRAM per open decoder for 1824×1920 GIFs, which is
     * what let full-res PIMSLO GIFs actually play on hardware. */
    dec->pixel_count = dec->width * dec->height;

    ESP_LOGI(TAG, "Opened GIF: %dx%d, %d palette colors", dec->width, dec->height, dec->palette.count);

    *out = dec;
    return ESP_OK;
}

int gif_decoder_get_width(gif_decoder_t *dec)  { return dec->width; }
int gif_decoder_get_height(gif_decoder_t *dec) { return dec->height; }

esp_err_t gif_decoder_next_frame(gif_decoder_t *dec, uint16_t *rgb565, int *delay_cs)
{
    *delay_cs = 10;  /* Default 100ms */

    while (1) {
        uint8_t block_type;
        if (fread(&block_type, 1, 1, dec->fp) != 1)
            return ESP_ERR_NOT_FOUND;  /* EOF */

        if (block_type == 0x3B) {
            /* Trailer — end of GIF */
            return ESP_ERR_NOT_FOUND;
        }

        if (block_type == 0x21) {
            /* Extension */
            uint8_t label;
            fread(&label, 1, 1, dec->fp);

            if (label == 0xF9) {
                /* Graphic Control Extension */
                uint8_t size;
                fread(&size, 1, 1, dec->fp);
                if (size >= 4) {
                    uint8_t gce[4];
                    fread(gce, 1, 4, dec->fp);
                    *delay_cs = gce[1] | ((int)gce[2] << 8);
                    if (*delay_cs == 0) *delay_cs = 10;
                    /* Skip remaining */
                    if (size > 4) fseek(dec->fp, size - 4, SEEK_CUR);
                }
                /* Block terminator */
                uint8_t term;
                fread(&term, 1, 1, dec->fp);
            } else {
                /* Skip other extensions */
                while (1) {
                    uint8_t bs;
                    if (fread(&bs, 1, 1, dec->fp) != 1 || bs == 0) break;
                    fseek(dec->fp, bs, SEEK_CUR);
                }
            }
            continue;
        }

        if (block_type == 0x2C) {
            /* Image Descriptor */
            uint16_t left = read_le16(dec->fp);
            uint16_t top = read_le16(dec->fp);
            uint16_t w = read_le16(dec->fp);
            uint16_t h = read_le16(dec->fp);

            uint8_t img_packed;
            fread(&img_packed, 1, 1, dec->fp);
            bool has_lct = (img_packed & 0x80) != 0;
            bool interlaced = (img_packed & 0x40) != 0;

            /* Skip local color table if present */
            if (has_lct) {
                int lct_size = 1 << ((img_packed & 0x07) + 1);
                fseek(dec->fp, lct_size * 3, SEEK_CUR);
            }

            /* Read LZW minimum code size */
            uint8_t min_code_size;
            fread(&min_code_size, 1, 1, dec->fp);

            /* Create LZW decoder */
            gif_lzw_dec_t *lzw = NULL;
            esp_err_t ret = gif_lzw_dec_create(min_code_size, &lzw);
            if (ret != ESP_OK) return ret;

            /* Decode LZW sub-blocks directly into the caller's rgb565 buffer,
             * treating it as a byte array for now (1 byte per palette index).
             * The caller allocated pixel_count * 2 bytes — we use the first
             * pixel_count bytes as the LZW scratch, then convert in place. */
            uint8_t *indices = (uint8_t *)rgb565;
            int decoded_pixels = 0;
            memset(indices, 0, dec->pixel_count);

            while (1) {
                uint8_t sub_block_size;
                if (fread(&sub_block_size, 1, 1, dec->fp) != 1 || sub_block_size == 0)
                    break;

                uint8_t sub_block[255];
                fread(sub_block, 1, sub_block_size, dec->fp);

                int out_len = 0;
                gif_lzw_dec_feed(lzw, sub_block, sub_block_size,
                                 indices + decoded_pixels,
                                 dec->pixel_count - decoded_pixels,
                                 &out_len);
                decoded_pixels += out_len;
            }

            gif_lzw_dec_destroy(lzw);

            /* In-place palette → RGB565 conversion, processing BACKWARDS so
             * the 2-byte write at position i doesn't clobber indices we
             * haven't read yet at positions 0..i-1.
             *
             * Correctness: at iteration i we read indices[i] (1 byte at
             * offset i) and write rgb565[i] (2 bytes at offsets 2i, 2i+1).
             * For any i ≥ 1, 2i > i, so the write sits strictly past the
             * current read position; future reads at i-1, i-2, ..., 0 all
             * happen at offsets < 2i, untouched. At i = 0 we've already
             * consumed indices[0] before overwriting bytes 0 and 1. */
            for (int i = dec->pixel_count - 1; i >= 0; i--) {
                uint8_t idx = indices[i];
                gif_color_t *c = &dec->palette.entries[idx];
                rgb565[i] = rgb888_to_rgb565(c->r, c->g, c->b);
            }

            return ESP_OK;
        }

        /* Unknown block type — skip */
        ESP_LOGW(TAG, "Unknown block type 0x%02X", block_type);
    }
}

esp_err_t gif_decoder_reset(gif_decoder_t *dec)
{
    if (!dec->fp) return ESP_FAIL;
    fseek(dec->fp, dec->first_frame_offset, SEEK_SET);
    return ESP_OK;
}

void gif_decoder_close(gif_decoder_t *dec)
{
    if (!dec) return;
    if (dec->fp) fclose(dec->fp);
    if (dec->path) free(dec->path);
    /* (pixel_indices no longer owned here — it lives in the caller's
     *  rgb565 buffer, re-used via in-place backwards conversion.) */
    free(dec);
}
