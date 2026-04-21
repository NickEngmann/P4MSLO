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

    /* Per-frame buffered LZW data for the read_next_frame / decode_read_frame
     * two-step API (used by the frame-cache dedup path). Allocated lazily,
     * freed in close(). */
    uint8_t *pending_lzw;
    size_t  pending_lzw_cap;   /* allocated size of pending_lzw */
    size_t  pending_lzw_len;   /* bytes of valid data */
    int     pending_min_code_size;
    int     pending_delay_cs;
    bool    pending_valid;     /* true between read_next_frame and
                                  decode_read_frame/discard_read_frame */

    /* Per-frame offset map, built up on the first pass through the GIF.
     * On subsequent loop iterations, if the current file offset matches
     * a recorded frame start, we fseek straight to its end_offset and
     * return the cached hash — no need to read+hash the ~1 MB of
     * compressed data again. This is what lets playback actually hit
     * the GIF's native framerate on cache-hit loops; otherwise the SD
     * read-back of each frame's LZW bytes dominates the frame period. */
#define GIF_MAX_FRAMES 16
    struct {
        long start_offset;   /* file offset at frame header start */
        long end_offset;     /* file offset just past the 0-terminator */
        uint32_t hash;
        int delay_cs;
        bool used;
    } frame_map[GIF_MAX_FRAMES];
    int frame_map_n;
};

/* FNV-1a 32-bit hash, used to key the frame cache. Fast, good-enough
 * distribution for our use case (dedup matched against a handful of
 * frames per GIF). */
static uint32_t fnv1a_update(uint32_t h, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}
#define FNV1A_INIT 0x811c9dc5u

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
    /* Drop any pending frame buffered by a prior read_next_frame that the
     * caller didn't finalize — otherwise a reset + decode_read_frame would
     * decode last-loop's last frame instead of the first. */
    dec->pending_valid = false;
    dec->pending_lzw_len = 0;
    return ESP_OK;
}

void gif_decoder_close(gif_decoder_t *dec)
{
    if (!dec) return;
    if (dec->fp) fclose(dec->fp);
    if (dec->path) free(dec->path);
    if (dec->pixel_indices) heap_caps_free(dec->pixel_indices);
    if (dec->pending_lzw) heap_caps_free(dec->pending_lzw);
    free(dec);
}

/* Ensure the pending_lzw buffer has at least `need` bytes of capacity.
 * Grows by doubling. Allocated in PSRAM since it can exceed 1 MB. */
static esp_err_t ensure_pending_cap(gif_decoder_t *dec, size_t need)
{
    if (dec->pending_lzw_cap >= need) return ESP_OK;

    /* Grow to at least `need`, but cap the overshoot. Pure doubling
     * jumped from 2 MB to 4 MB even when only 2.2 MB was required,
     * and under PSRAM fragmentation 4 MB contiguous often wasn't
     * available. Use the smaller of {double, need + 25% slack}. */
    size_t want_double = dec->pending_lzw_cap ? dec->pending_lzw_cap * 2 : 16384;
    size_t want_slack  = need + (need / 4);
    size_t new_cap = (want_double < want_slack) ? want_double : want_slack;
    if (new_cap < need) new_cap = need;

    /* When there's no valid content to preserve (between frames we reset
     * pending_lzw_len=0), free the old block before allocating the new
     * one. heap_caps_realloc keeps both alive transiently; under PSRAM
     * fragmentation the target contiguous region may not exist if the
     * old block pins a hole. Freeing first turns a copy-and-grow into a
     * plain malloc. */
    if (dec->pending_lzw_len == 0 && dec->pending_lzw) {
        heap_caps_free(dec->pending_lzw);
        dec->pending_lzw = NULL;
        dec->pending_lzw_cap = 0;
    }

    uint8_t *nb = heap_caps_realloc(dec->pending_lzw, new_cap, MALLOC_CAP_SPIRAM);
    if (!nb) {
        ESP_LOGE(TAG, "Failed to grow pending_lzw to %zu bytes", new_cap);
        return ESP_ERR_NO_MEM;
    }
    dec->pending_lzw = nb;
    dec->pending_lzw_cap = new_cap;
    return ESP_OK;
}

/* Read the next image block (skipping extensions / trailers) from the
 * file, buffer its LZW sub-blocks into dec->pending_lzw, compute a hash
 * of those bytes. File cursor is left just past this frame's data.
 *
 * Fast path: if we've seen a frame starting at the current offset before
 * (and recorded its hash + end offset on that pass), seek directly to
 * the end and return the recorded hash without reading the LZW bytes.
 * This makes cache-hit playback I/O-free and lets us actually hit the
 * GIF's 150 ms framerate instead of being stuck at SD-read speed. */
esp_err_t gif_decoder_read_next_frame(gif_decoder_t *dec,
                                       uint32_t *hash_out,
                                       int *delay_cs_out)
{
    if (!dec || !dec->fp) return ESP_ERR_INVALID_STATE;

    long current_offset = ftell(dec->fp);
    /* Fast path — already-scanned frame starting here. */
    for (int i = 0; i < dec->frame_map_n; i++) {
        if (dec->frame_map[i].used &&
            dec->frame_map[i].start_offset == current_offset) {
            fseek(dec->fp, dec->frame_map[i].end_offset, SEEK_SET);
            if (hash_out) *hash_out = dec->frame_map[i].hash;
            if (delay_cs_out) *delay_cs_out = dec->frame_map[i].delay_cs;
            dec->pending_valid = false;   /* no pending data on fast path */
            return ESP_OK;
        }
    }

    int delay_cs = 10;  /* default 100 ms */

    while (1) {
        uint8_t block_type;
        if (fread(&block_type, 1, 1, dec->fp) != 1) return ESP_ERR_NOT_FOUND;

        if (block_type == 0x3B) return ESP_ERR_NOT_FOUND;  /* trailer */

        if (block_type == 0x21) {
            uint8_t label;
            fread(&label, 1, 1, dec->fp);
            if (label == 0xF9) {
                uint8_t size;
                fread(&size, 1, 1, dec->fp);
                if (size >= 4) {
                    uint8_t gce[4];
                    fread(gce, 1, 4, dec->fp);
                    delay_cs = gce[1] | ((int)gce[2] << 8);
                    if (delay_cs == 0) delay_cs = 10;
                    if (size > 4) fseek(dec->fp, size - 4, SEEK_CUR);
                }
                uint8_t term; fread(&term, 1, 1, dec->fp);
            } else {
                while (1) {
                    uint8_t bs;
                    if (fread(&bs, 1, 1, dec->fp) != 1 || bs == 0) break;
                    fseek(dec->fp, bs, SEEK_CUR);
                }
            }
            continue;
        }

        if (block_type == 0x2C) {
            /* Image Descriptor — skip the position/size/LCT stuff but
             * remember min_code_size so decode_read_frame can replay it. */
            fseek(dec->fp, 8, SEEK_CUR);  /* left, top, width, height */
            uint8_t img_packed;
            fread(&img_packed, 1, 1, dec->fp);
            bool has_lct = (img_packed & 0x80) != 0;
            bool interlaced = (img_packed & 0x40) != 0;
            if (interlaced) {
                ESP_LOGE(TAG, "Interlaced GIFs not supported");
                return ESP_FAIL;
            }
            if (has_lct) {
                int lct_size = 1 << ((img_packed & 0x07) + 1);
                fseek(dec->fp, lct_size * 3, SEEK_CUR);
            }
            uint8_t min_code_size;
            fread(&min_code_size, 1, 1, dec->fp);

            /* Buffer every sub-block into pending_lzw in order (with the
             * 1-byte sub-block size prefix so decode can replay it
             * identically). Accumulate hash as we go. */
            dec->pending_lzw_len = 0;
            uint32_t hash = FNV1A_INIT;
            hash = fnv1a_update(hash, &min_code_size, 1);

            while (1) {
                uint8_t bs;
                if (fread(&bs, 1, 1, dec->fp) != 1) return ESP_FAIL;
                esp_err_t r = ensure_pending_cap(dec, dec->pending_lzw_len + 1 + bs);
                if (r != ESP_OK) return r;
                dec->pending_lzw[dec->pending_lzw_len++] = bs;
                if (bs == 0) break;
                fread(&dec->pending_lzw[dec->pending_lzw_len], 1, bs, dec->fp);
                hash = fnv1a_update(hash, &dec->pending_lzw[dec->pending_lzw_len], bs);
                dec->pending_lzw_len += bs;
            }

            dec->pending_min_code_size = min_code_size;
            dec->pending_delay_cs = delay_cs;
            dec->pending_valid = true;

            /* Record (start, end, hash, delay) for this frame so the
             * next loop iteration can skip the SD read. */
            if (dec->frame_map_n < GIF_MAX_FRAMES) {
                int i = dec->frame_map_n++;
                dec->frame_map[i].start_offset = current_offset;
                dec->frame_map[i].end_offset   = ftell(dec->fp);
                dec->frame_map[i].hash         = hash;
                dec->frame_map[i].delay_cs     = delay_cs;
                dec->frame_map[i].used         = true;
            }

            if (hash_out) *hash_out = hash;
            if (delay_cs_out) *delay_cs_out = delay_cs;
            return ESP_OK;
        }

        /* Unknown block — skip */
        ESP_LOGW(TAG, "Unknown block type 0x%02X (skipping)", block_type);
    }
}

esp_err_t gif_decoder_decode_read_frame(gif_decoder_t *dec,
                                         uint16_t *target_rgb565,
                                         int target_w, int target_h)
{
    if (!dec || !dec->pending_valid) return ESP_ERR_INVALID_STATE;

    gif_lzw_dec_t *lzw = NULL;
    esp_err_t ret = gif_lzw_dec_create(dec->pending_min_code_size, &lzw);
    if (ret != ESP_OK) return ret;

    const int src_w = dec->width;
    const int src_h = dec->height;
    int decoded_pixels = 0;
    memset(dec->pixel_indices, 0, dec->pixel_count);

    /* Replay sub-blocks from pending_lzw into the LZW decoder. Each
     * sub-block is prefixed by its 1-byte size; size 0 terminates. */
    size_t pos = 0;
    while (pos < dec->pending_lzw_len) {
        uint8_t bs = dec->pending_lzw[pos++];
        if (bs == 0) break;
        int out_len = 0;
        gif_lzw_dec_feed(lzw, &dec->pending_lzw[pos], bs,
                         dec->pixel_indices + decoded_pixels,
                         dec->pixel_count - decoded_pixels,
                         &out_len);
        decoded_pixels += out_len;
        pos += bs;
    }
    gif_lzw_dec_destroy(lzw);

    /* Nearest-neighbor downscale indices → target_rgb565, same as
     * gif_decoder_next_frame. */
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

    /* Free the transient buffer — realloc on next read. Keeping it around
     * would pin ~1 MB of PSRAM per open decoder for no benefit. */
    if (dec->pending_lzw) {
        heap_caps_free(dec->pending_lzw);
        dec->pending_lzw = NULL;
        dec->pending_lzw_cap = 0;
    }
    dec->pending_lzw_len = 0;
    dec->pending_valid = false;
    return ESP_OK;
}

void gif_decoder_discard_read_frame(gif_decoder_t *dec)
{
    if (!dec) return;
    if (dec->pending_lzw) {
        heap_caps_free(dec->pending_lzw);
        dec->pending_lzw = NULL;
        dec->pending_lzw_cap = 0;
    }
    dec->pending_lzw_len = 0;
    dec->pending_valid = false;
}
