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
    int pixel_count;

    /* Full-frame palette-index buffer (width × height bytes). Allocated
     * once at open time so we don't hit fragmentation on every frame.
     * The decoder fills this from the LZW stream with unbounded out_cap
     * per feed() call (which the existing LZW decoder requires — out_cap
     * truncation silently drops bytes). Downscaling to the caller's
     * target happens after LZW is done.
     *
     * For 1824×1920 PIMSLO GIFs that's 3.5 MB — down from the old
     * 3.3 MB pixel_indices + 6.7 MB rgb565 = 10 MB peak. */
    uint8_t *pixel_indices;
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

    /* Full-frame indices buffer, allocated at open time (before any per-
     * frame PSRAM churn) so it's less likely to hit fragmentation than
     * allocating on the first play. Dropping the old 6.7 MB full-res
     * RGB565 buffer and keeping only indices + scaling-at-emit saves
     * ~6.7 MB off the peak. */
    dec->pixel_count = dec->width * dec->height;
    dec->pixel_indices = heap_caps_malloc(dec->pixel_count, MALLOC_CAP_SPIRAM);
    if (!dec->pixel_indices) {
        ESP_LOGE(TAG, "Failed to allocate %d byte indices buffer",
                 dec->pixel_count);
        gif_decoder_close(dec);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Opened GIF: %dx%d, %d palette colors", dec->width, dec->height, dec->palette.count);

    *out = dec;
    return ESP_OK;
}

int gif_decoder_get_width(gif_decoder_t *dec)  { return dec->width; }
int gif_decoder_get_height(gif_decoder_t *dec) { return dec->height; }

esp_err_t gif_decoder_next_frame(gif_decoder_t *dec,
                                  uint16_t *target_rgb565,
                                  int target_w, int target_h,
                                  int *delay_cs)
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
            (void)left; (void)top; (void)w; (void)h;  // not currently used

            uint8_t img_packed;
            fread(&img_packed, 1, 1, dec->fp);
            bool has_lct = (img_packed & 0x80) != 0;
            bool interlaced = (img_packed & 0x40) != 0;

            if (interlaced) {
                /* PIMSLO encoder never produces interlaced output; supporting
                 * it here would require buffering the 8-row passes, which
                 * brings back the full-frame allocation we're getting rid of.
                 * Log and bail if ever encountered. */
                ESP_LOGE(TAG, "Interlaced GIFs not supported by streaming decoder");
                return ESP_FAIL;
            }

            /* Skip local color table if present */
            if (has_lct) {
                int lct_size = 1 << ((img_packed & 0x07) + 1);
                fseek(dec->fp, lct_size * 3, SEEK_CUR);
            }

            /* Read LZW minimum code size */
            uint8_t min_code_size;
            fread(&min_code_size, 1, 1, dec->fp);

            gif_lzw_dec_t *lzw = NULL;
            esp_err_t ret = gif_lzw_dec_create(min_code_size, &lzw);
            if (ret != ESP_OK) return ret;

            /* Decode the full LZW stream into `dec->pixel_indices` at full
             * source resolution. Each feed() gets unlimited out_cap (the
             * remaining tail of the indices buffer), matching the legacy
             * flow — avoids the LZW out_cap truncation hazard entirely.
             *
             * After decode, nearest-neighbor downscale directly from
             * indices to the caller's target_rgb565 buffer. No full-res
             * RGB565 intermediate needed. Peak memory is indices (~3.3 MB
             * for 1824×1920) + target (115 KB for 240×240) instead of
             * the legacy path's ~10 MB. */
            const int src_w = dec->width;
            const int src_h = dec->height;
            int decoded_pixels = 0;
            memset(dec->pixel_indices, 0, dec->pixel_count);

            while (1) {
                uint8_t sub_block_size;
                if (fread(&sub_block_size, 1, 1, dec->fp) != 1 ||
                    sub_block_size == 0) break;
                uint8_t sub_block[255];
                fread(sub_block, 1, sub_block_size, dec->fp);

                int out_len = 0;
                gif_lzw_dec_feed(lzw, sub_block, sub_block_size,
                                 dec->pixel_indices + decoded_pixels,
                                 dec->pixel_count - decoded_pixels,
                                 &out_len);
                decoded_pixels += out_len;
            }

            gif_lzw_dec_destroy(lzw);

            /* Nearest-neighbor downscale indices → target_rgb565. For each
             * output pixel we pick the representative source index and
             * convert via palette to RGB565. */
            for (int out_y = 0; out_y < target_h; out_y++) {
                int sy = (out_y * src_h) / target_h;
                if (sy >= src_h) sy = src_h - 1;
                const uint8_t *src_row = &dec->pixel_indices[sy * src_w];
                uint16_t *dst_row = &target_rgb565[out_y * target_w];
                for (int out_x = 0; out_x < target_w; out_x++) {
                    int sx = (out_x * src_w) / target_w;
                    if (sx >= src_w) sx = src_w - 1;
                    const gif_color_t *c = &dec->palette.entries[src_row[sx]];
                    dst_row[out_x] = rgb888_to_rgb565(c->r, c->g, c->b);
                }
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
    if (dec->pixel_indices) heap_caps_free(dec->pixel_indices);
    free(dec);
}
