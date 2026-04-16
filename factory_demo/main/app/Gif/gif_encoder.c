/**
 * @file gif_encoder.c
 * @brief Streaming GIF89a encoder for ESP32-P4
 *
 * Uses the hardware JPEG decoder and PPA scaler to process frames,
 * then quantizes and LZW-compresses each frame to the output GIF file.
 */

#include "gif_encoder.h"
#include "gif_quantize.h"
#include "gif_lzw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "bsp/display.h"
#include "gif_tjpgd.h"

/* SIMD-accelerated functions (gif_simd.S) */
extern void gif_simd_distribute_below(int16_t *err_nxt, const int16_t *errors, int count);
extern void gif_simd_memzero_s16(int16_t *buf, int count);

static const char *TAG = "gif_enc";

/* Align a value up to the given alignment */
#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

struct gif_encoder {
    gif_encoder_config_t config;
    gif_palette_t palette;
    gif_quantize_ctx_t *quantizer;

    /* JPEG decoder / PPA handles (created per-encoder) */
    jpeg_decoder_handle_t jpeg_handle;
    ppa_client_handle_t ppa_handle;

    /* Buffers (PSRAM) */
    void *jpeg_buf;         /* Raw JPEG data */
    size_t jpeg_buf_size;   /* Current jpeg_buf allocation size */
    void *decode_buf;       /* JPEG decode output (RGB565) */
    size_t decode_buf_size;
    uint16_t *scaled_buf;   /* PPA output (RGB565, target resolution) */
    size_t scaled_buf_size;

    /* Output */
    FILE *fp;
    int frame_count;
    int width, height;      /* Resolved target dimensions */
    size_t cache_line_size;

    /* RGB565 → palette index LUT (65536 entries, built after pass 1) */
    uint8_t *pixel_lut;

    /* Progress */
    gif_encoder_progress_cb_t progress_cb;
    void *progress_user;
    int total_frames;
    int current_pass;
};

/* ---- Helpers ---- */

/* The JPEG decoder + PPA output is standard RGB565 regardless of the
 * JPEG_DEC_RGB_ELEMENT_ORDER_BGR flag (verified via host-side testing).
 * RGB565: bits [15:11]=R, [10:5]=G, [4:0]=B */
static inline void rgb565_to_rgb888(uint16_t px, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t r5 = (px >> 11) & 0x1F;
    uint8_t g6 = (px >> 5)  & 0x3F;
    uint8_t b5 =  px        & 0x1F;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

/* ---- tjpgd software JPEG decoder (fallback for 4:2:2 JPEGs) ---- */

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    uint16_t *out_buf;
    int out_width;      /* Output buffer stride (pixels) */
    int crop_x;         /* Crop region (0,0 = no crop) */
    int crop_y;
    int crop_w;
    int crop_h;
} tjpgd_ctx_t;

static size_t tjpgd_input(JDEC *jd, uint8_t *buff, size_t ndata)
{
    tjpgd_ctx_t *ctx = (tjpgd_ctx_t *)jd->device;
    size_t avail = ctx->size - ctx->pos;
    if (ndata > avail) ndata = avail;
    if (buff) memcpy(buff, ctx->data + ctx->pos, ndata);
    ctx->pos += ndata;
    return ndata;
}

static int tjpgd_output(JDEC *jd, void *bitmap, JRECT *rect)
{
    tjpgd_ctx_t *ctx = (tjpgd_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    int bw = rect->right - rect->left + 1;
    int bh = rect->bottom - rect->top + 1;

    for (int y = 0; y < bh; y++) {
        int src_y = rect->top + y;

        /* Apply crop if active */
        if (ctx->crop_w > 0) {
            if (src_y < ctx->crop_y || src_y >= ctx->crop_y + ctx->crop_h) continue;
            int dst_y = src_y - ctx->crop_y;

            const uint8_t *src_row = &src[y * bw * 3];
            for (int x = 0; x < bw; x++) {
                int src_x = rect->left + x;
                if (src_x < ctx->crop_x || src_x >= ctx->crop_x + ctx->crop_w) continue;
                int dst_x = src_x - ctx->crop_x;
                uint8_t r = src_row[x * 3], g = src_row[x * 3 + 1], b = src_row[x * 3 + 2];
                ctx->out_buf[dst_y * ctx->out_width + dst_x] =
                    ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            }
        } else {
            /* No crop — write directly */
            uint16_t *dst_row = &ctx->out_buf[src_y * ctx->out_width + rect->left];
            const uint8_t *src_row = &src[y * bw * 3];
            for (int x = 0; x < bw; x++) {
                uint8_t r = src_row[x * 3], g = src_row[x * 3 + 1], b = src_row[x * 3 + 2];
                dst_row[x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            }
        }
    }
    return 1;
}

/* Decode JPEG data in memory, apply optional crop, and scale to target size.
 * Output is in enc->scaled_buf as RGB565.
 * If crop is NULL, uses the full image. */
static esp_err_t decode_and_scale_jpeg(gif_encoder_t *enc,
                                        const uint8_t *jpeg_data, size_t jpeg_size,
                                        const gif_crop_rect_t *crop)
{
    esp_err_t ret = ESP_OK;

    /* Use tjpgd for header parsing AND decoding — it handles both 4:2:0
     * and 4:2:2 subsampling. The ESP32-P4 HW decoder can't handle 4:2:2
     * JPEGs from the OV3660 cameras. */
    tjpgd_ctx_t tjctx = {
        .data = jpeg_data, .size = jpeg_size, .pos = 0,
    };
    JDEC jdec;
    static uint8_t tjwork[32768] __attribute__((aligned(4)));

    JRESULT jres = gif_jd_prepare(&jdec, tjpgd_input, tjwork, sizeof(tjwork), &tjctx);
    if (jres != JDR_OK) {
        ESP_LOGE(TAG, "JPEG header parse failed: %d", jres);
        return ESP_FAIL;
    }

    /* Use tjpgd's parsed dimensions */
    uint32_t img_w = jdec.width, img_h = jdec.height;

    ESP_LOGI(TAG, "JPEG: %ux%u (%zu bytes, msx=%d msy=%d)%s",
             img_w, img_h, jpeg_size, jdec.msx, jdec.msy,
             crop ? " [cropped]" : "");

    /* Determine source region (full or cropped) */
    int src_x = crop ? crop->x : 0;
    int src_y = crop ? crop->y : 0;
    int src_w = crop ? crop->w : (int)img_w;
    int src_h = crop ? crop->h : (int)img_h;

    /* Set target dimensions from first frame if not specified */
    if (enc->width == 0 || enc->height == 0) {
        if (enc->config.target_width > 0 && enc->config.target_height > 0) {
            enc->width = enc->config.target_width;
            enc->height = enc->config.target_height;
        } else {
            enc->width = src_w;
            enc->height = src_h;
        }
        ESP_LOGI(TAG, "GIF dimensions: %dx%d", enc->width, enc->height);
    }

    /* Allocate the scaled output buffer (crop-sized, not full source).
     * tjpgd decodes into this directly via the crop-aware output callback,
     * so we never need a full-source-resolution intermediate buffer. */
    if (!enc->scaled_buf) {
        enc->scaled_buf_size = ALIGN_UP(enc->width * enc->height * 2, enc->cache_line_size);
        enc->scaled_buf = heap_caps_aligned_calloc(enc->cache_line_size, 1,
                                                    enc->scaled_buf_size, MALLOC_CAP_SPIRAM);
        if (!enc->scaled_buf) {
            ESP_LOGE(TAG, "Failed to allocate %zu byte scaled buffer", enc->scaled_buf_size);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Allocated output buffer: %zu bytes (%dx%d)",
                 enc->scaled_buf_size, enc->width, enc->height);
    }

    /* Decode JPEG → RGB565 directly into scaled_buf with crop applied.
     * tjpgd outputs MCU blocks via callback — the callback applies
     * the crop and converts to RGB565 in one step. No PPA needed. */
    tjctx.pos = 0;
    tjctx.out_buf = enc->scaled_buf;
    tjctx.out_width = enc->width;
    tjctx.crop_x = src_x;
    tjctx.crop_y = src_y;
    tjctx.crop_w = crop ? src_w : 0;  /* 0 = no crop */
    tjctx.crop_h = crop ? src_h : 0;

    jres = gif_jd_prepare(&jdec, tjpgd_input, tjwork, sizeof(tjwork), &tjctx);
    if (jres == JDR_OK) {
        jres = gif_jd_decomp(&jdec, tjpgd_output, 0);
    }
    if (jres != JDR_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: tjpgd error %d", jres);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Decoded %ux%u → %dx%d RGB565", img_w, img_h, enc->width, enc->height);
    return ESP_OK;
}

/* Read a JPEG file from SD card into enc->jpeg_buf */
static esp_err_t load_jpeg_from_file(gif_encoder_t *enc, const char *jpeg_path,
                                      const uint8_t **out_data, size_t *out_size)
{
    FILE *f = fopen(jpeg_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", jpeg_path);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (!enc->jpeg_buf || enc->jpeg_buf_size < file_size) {
        if (enc->jpeg_buf) free(enc->jpeg_buf);
        enc->jpeg_buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
        enc->jpeg_buf_size = file_size;
        if (!enc->jpeg_buf) {
            enc->jpeg_buf_size = 0;
            fclose(f);
            return ESP_ERR_NO_MEM;
        }
    }

    fread(enc->jpeg_buf, 1, file_size, f);
    fclose(f);

    *out_data = (const uint8_t *)enc->jpeg_buf;
    *out_size = file_size;
    return ESP_OK;
}

/* Convenience: load from file + decode (backward compatible) */
static esp_err_t load_and_decode_jpeg(gif_encoder_t *enc, const char *jpeg_path)
{
    const uint8_t *data;
    size_t size;
    esp_err_t ret = load_jpeg_from_file(enc, jpeg_path, &data, &size);
    if (ret != ESP_OK) return ret;
    return decode_and_scale_jpeg(enc, data, size, NULL);
}

/* ---- GIF file format writers ---- */

static void write_le16(FILE *fp, uint16_t v)
{
    uint8_t buf[2] = { v & 0xFF, (v >> 8) & 0xFF };
    fwrite(buf, 1, 2, fp);
}

static void write_gif_header(gif_encoder_t *enc)
{
    /* GIF89a signature */
    fwrite("GIF89a", 1, 6, enc->fp);

    /* Logical Screen Descriptor */
    write_le16(enc->fp, (uint16_t)enc->width);
    write_le16(enc->fp, (uint16_t)enc->height);

    /* Packed field: GCT flag=1, color res=7 (8 bits), sort=0, GCT size=7 (256 colors) */
    uint8_t packed = 0x80 | (7 << 4) | 7;
    fwrite(&packed, 1, 1, enc->fp);

    uint8_t bg_color = 0;
    fwrite(&bg_color, 1, 1, enc->fp);

    uint8_t aspect = 0;
    fwrite(&aspect, 1, 1, enc->fp);

    /* Global Color Table (256 entries = 768 bytes) */
    for (int i = 0; i < 256; i++) {
        uint8_t rgb[3];
        if (i < enc->palette.count) {
            rgb[0] = enc->palette.entries[i].r;
            rgb[1] = enc->palette.entries[i].g;
            rgb[2] = enc->palette.entries[i].b;
        } else {
            rgb[0] = rgb[1] = rgb[2] = 0;
        }
        fwrite(rgb, 1, 3, enc->fp);
    }

    /* Netscape Application Extension (infinite loop) */
    uint8_t nab[] = {
        0x21, 0xFF,       /* Extension introducer + app extension label */
        0x0B,             /* Block size = 11 */
        'N','E','T','S','C','A','P','E','2','.','0',
        0x03,             /* Sub-block size = 3 */
        0x01,             /* Sub-block ID */
        0x00, 0x00,       /* Loop count (0 = infinite) */
        0x00              /* Block terminator */
    };
    /* Set loop count */
    nab[16] = enc->config.loop_count & 0xFF;
    nab[17] = (enc->config.loop_count >> 8) & 0xFF;
    fwrite(nab, 1, sizeof(nab), enc->fp);
}

static void write_frame_header(gif_encoder_t *enc)
{
    /* Graphic Control Extension */
    uint8_t gce[] = {
        0x21, 0xF9,   /* Extension introducer + GCE label */
        0x04,          /* Block size = 4 */
        0x00,          /* Packed: disposal=0, no user input, no transparent */
        0x00, 0x00,    /* Delay time (centiseconds, little-endian) */
        0x00,          /* Transparent color index (unused) */
        0x00           /* Block terminator */
    };
    gce[4] = enc->config.frame_delay_cs & 0xFF;
    gce[5] = (enc->config.frame_delay_cs >> 8) & 0xFF;
    fwrite(gce, 1, sizeof(gce), enc->fp);

    /* Image Descriptor */
    uint8_t sep = 0x2C;
    fwrite(&sep, 1, 1, enc->fp);
    write_le16(enc->fp, 0);  /* left */
    write_le16(enc->fp, 0);  /* top */
    write_le16(enc->fp, (uint16_t)enc->width);
    write_le16(enc->fp, (uint16_t)enc->height);
    uint8_t img_packed = 0;  /* No local color table, not interlaced */
    fwrite(&img_packed, 1, 1, enc->fp);
}

/* ---- Public API ---- */

esp_err_t gif_encoder_create(const gif_encoder_config_t *config, gif_encoder_t **out)
{
    gif_encoder_t *enc = calloc(1, sizeof(gif_encoder_t));
    if (!enc) return ESP_ERR_NO_MEM;

    enc->config = *config;
    if (enc->config.frame_delay_cs <= 0)
        enc->config.frame_delay_cs = 50;  /* Default 500ms */

    /* Create quantizer */
    esp_err_t ret = gif_quantize_create(&enc->quantizer);
    if (ret != ESP_OK) {
        free(enc);
        return ret;
    }

    /* Create JPEG decoder engine */
    jpeg_decode_engine_cfg_t dec_cfg = { .timeout_ms = 100 };
    ret = jpeg_new_decoder_engine(&dec_cfg, &enc->jpeg_handle);
    if (ret != ESP_OK) {
        gif_quantize_destroy(enc->quantizer);
        free(enc);
        return ret;
    }

    /* Create PPA client */
    ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM };
    ret = ppa_register_client(&ppa_cfg, &enc->ppa_handle);
    if (ret != ESP_OK) {
        jpeg_del_decoder_engine(enc->jpeg_handle);
        gif_quantize_destroy(enc->quantizer);
        free(enc);
        return ret;
    }

    /* Get cache alignment for PSRAM buffers */
    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &enc->cache_line_size);

    /* Decode buffer is allocated lazily in load_and_decode_jpeg() based on
     * the actual JPEG dimensions, to avoid wasting memory on a full-res buffer
     * when the camera pipeline is running. */
    ESP_LOGI(TAG, "Free PSRAM: %zu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    *out = enc;
    return ESP_OK;
}

void gif_encoder_set_progress_cb(gif_encoder_t *enc, gif_encoder_progress_cb_t cb, void *user_data)
{
    enc->progress_cb = cb;
    enc->progress_user = user_data;
}

esp_err_t gif_encoder_pass1_add_frame(gif_encoder_t *enc, const char *jpeg_path)
{
    esp_err_t ret = load_and_decode_jpeg(enc, jpeg_path);
    if (ret != ESP_OK) return ret;

    /* Subsample pixels into quantizer (every 4th pixel to save time) */
    ret = gif_quantize_accumulate_rgb565(enc->quantizer, enc->scaled_buf,
                                         enc->width, enc->height, 4);

    enc->total_frames++;
    if (enc->progress_cb)
        enc->progress_cb(enc->total_frames, 0, 1, enc->progress_user);

    return ret;
}

esp_err_t gif_encoder_pass1_finalize(gif_encoder_t *enc)
{
    esp_err_t ret = gif_quantize_build_palette(enc->quantizer, &enc->palette);
    if (ret != ESP_OK) return ret;

    /* Build RGB565 → palette index LUT for O(1) pixel mapping in pass 2 */
    enc->pixel_lut = heap_caps_malloc(65536, MALLOC_CAP_SPIRAM);
    if (!enc->pixel_lut) {
        ESP_LOGE(TAG, "Failed to allocate 64KB pixel LUT");
        return ESP_ERR_NO_MEM;
    }
    gif_quantize_build_lut(&enc->palette, enc->pixel_lut);

    return ESP_OK;
}

esp_err_t gif_encoder_pass2_begin(gif_encoder_t *enc, const char *output_path)
{
    enc->fp = fopen(output_path, "w+b"); /* w+b: read+write for frame replay */
    if (!enc->fp) {
        ESP_LOGE(TAG, "Cannot create %s", output_path);
        return ESP_FAIL;
    }
    /* Use 32KB write buffer to reduce SD card syscalls */
    static char file_buf[32768];
    setvbuf(enc->fp, file_buf, _IOFBF, sizeof(file_buf));

    write_gif_header(enc);
    enc->frame_count = 0;
    enc->current_pass = 2;

    return ESP_OK;
}

esp_err_t gif_encoder_pass2_add_frame(gif_encoder_t *enc, const char *jpeg_path)
{
    uint32_t t_start = esp_log_timestamp();

    esp_err_t ret = load_and_decode_jpeg(enc, jpeg_path);
    if (ret != ESP_OK) return ret;

    uint32_t t_decode = esp_log_timestamp();

    write_frame_header(enc);

    /* LZW encode with Floyd-Steinberg dithering for smooth gradients.
     * Uses precomputed RGB565→palette LUT for O(1) pixel mapping. */
    gif_lzw_enc_t *lzw = NULL;
    ret = gif_lzw_enc_create(8, enc->fp, &lzw);  /* 8 = min code size for 256 colors */
    if (ret != ESP_OK) return ret;

    int w = enc->width, h = enc->height;

    /* Copy LUT to internal RAM for fast access (64KB) */
    uint8_t *lut = heap_caps_malloc(65536, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bool lut_internal = (lut != NULL);
    if (lut) {
        memcpy(lut, enc->pixel_lut, 65536);
        ESP_LOGI(TAG, "LUT in internal RAM (64KB)");
    } else {
        lut = enc->pixel_lut;
        ESP_LOGW(TAG, "LUT in PSRAM");
    }

    /* Cache palette RGB values in local array for fast access */
    uint8_t pal_r[256], pal_g[256], pal_b[256];
    for (int i = 0; i < 256; i++) {
        pal_r[i] = enc->palette.entries[i].r;
        pal_g[i] = enc->palette.entries[i].g;
        pal_b[i] = enc->palette.entries[i].b;
    }

    /* Error diffusion buffers — try internal RAM first for speed.
     * Each: 1920 * 3 * 2 = 11,520 bytes. Two buffers = 23KB. */
    size_t err_size = w * 3 * sizeof(int16_t);
    int16_t *err_cur = heap_caps_calloc(1, err_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *err_nxt = heap_caps_calloc(1, err_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!err_cur || !err_nxt) {
        if (err_cur) { free(err_cur); err_cur = NULL; }
        if (err_nxt) { free(err_nxt); err_nxt = NULL; }
        err_cur = heap_caps_calloc(1, err_size, MALLOC_CAP_SPIRAM);
        err_nxt = heap_caps_calloc(1, err_size, MALLOC_CAP_SPIRAM);
        ESP_LOGW(TAG, "Error buffers in PSRAM");
    } else {
        ESP_LOGI(TAG, "Error buffers in internal RAM (%zu bytes each)", err_size);
    }

    /* Prefetch pixel row into internal SRAM for fast access (3840 bytes for 1920px) */
    uint16_t *row_cache = heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bool use_row_cache = (row_cache != NULL);
    if (use_row_cache) {
        ESP_LOGI(TAG, "Row prefetch buffer in internal RAM (%d bytes)", w * 2);
    }

    /* Allocate row buffers for multi-pass processing */
    uint8_t *row_indices = heap_caps_malloc(w, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!row_indices) row_indices = heap_caps_malloc(w, MALLOC_CAP_SPIRAM);

    int16_t *row_errors = NULL; /* unused — kept for future SIMD vectorization */

    for (int y = 0; y < h; y++) {
        /* Prefetch entire pixel row from PSRAM to internal SRAM */
        const uint16_t *row_px;
        if (use_row_cache) {
            memcpy(row_cache, &enc->scaled_buf[y * w], w * sizeof(uint16_t));
            row_px = row_cache;
        } else {
            row_px = &enc->scaled_buf[y * w];
        }
        int16_t *restrict ec = err_cur;
        int16_t *restrict en = err_nxt;
        const int last_row = (y + 1 >= h);

        /* Pass A: Dither + LUT → palette indices */
        for (int x = 0; x < w; x++) {
            uint16_t px = row_px[x];
            int r = ((((px >> 11) & 0x1F) * 527 + 23) >> 6) + ec[x*3];
            int g = ((((px >> 5) & 0x3F) * 259 + 33) >> 6) + ec[x*3+1];
            int b = (((px & 0x1F) * 527 + 23) >> 6)        + ec[x*3+2];

            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);

            uint16_t d16 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            uint8_t idx = lut[d16];
            row_indices[x] = idx;

            int er = r - pal_r[idx];
            int eg = g - pal_g[idx];
            int eb = b - pal_b[idx];

            if (x + 1 < w) {
                ec[(x+1)*3]   += er * 7 >> 4;
                ec[(x+1)*3+1] += eg * 7 >> 4;
                ec[(x+1)*3+2] += eb * 7 >> 4;
            }
            if (!last_row) {
                if (x > 0) {
                    en[(x-1)*3]   += er * 3 >> 4;
                    en[(x-1)*3+1] += eg * 3 >> 4;
                    en[(x-1)*3+2] += eb * 3 >> 4;
                }
                en[x*3]   += er * 5 >> 4;
                en[x*3+1] += eg * 5 >> 4;
                en[x*3+2] += eb * 5 >> 4;
                if (x + 1 < w) {
                    en[(x+1)*3]   += er >> 4;
                    en[(x+1)*3+1] += eg >> 4;
                    en[(x+1)*3+2] += eb >> 4;
                }
            }
        }

        /* Pass B: Feed row of indices to LZW encoder */
        for (int x = 0; x < w; x++) {
            gif_lzw_enc_pixel(lzw, row_indices[x]);
        }

        /* Swap row buffers and zero */
        int16_t *tmp = err_cur;
        err_cur = err_nxt;
        err_nxt = tmp;
        gif_simd_memzero_s16(err_nxt, w * 3);
    }

    heap_caps_free(err_cur);
    heap_caps_free(err_nxt);
    if (row_cache) free(row_cache);
    if (row_indices) free(row_indices);
    if (row_errors) free(row_errors);
    if (lut_internal) free(lut);

    uint32_t t_dither_lzw = esp_log_timestamp();

    gif_lzw_enc_finish(lzw);
    gif_lzw_enc_destroy(lzw);

    uint32_t t_done = esp_log_timestamp();
    ESP_LOGI(TAG, "Frame timing: decode=%lums dither+lzw=%lums flush=%lums total=%lums",
             t_decode - t_start, t_dither_lzw - t_decode,
             t_done - t_dither_lzw, t_done - t_start);

    enc->frame_count++;
    if (enc->progress_cb)
        enc->progress_cb(enc->frame_count, enc->total_frames, 2, enc->progress_user);

    return ESP_OK;
}

long gif_encoder_get_file_pos(gif_encoder_t *enc)
{
    if (!enc || !enc->fp) return -1;
    fflush(enc->fp);
    return ftell(enc->fp);
}

esp_err_t gif_encoder_pass2_replay_frame(gif_encoder_t *enc,
                                          long src_offset, size_t length)
{
    if (!enc || !enc->fp) return ESP_ERR_INVALID_STATE;
    if (length == 0) return ESP_ERR_INVALID_ARG;

    /* Remember where we need to write (current end of file) */
    fflush(enc->fp);
    long write_pos = ftell(enc->fp);

    /* Copy frame data using the largest buffer we can allocate.
     * Each seek+read+seek+write cycle has SD overhead, so bigger = faster. */
    size_t buf_size = length;  /* Try full frame first */
    uint8_t *buf = NULL;
    while (buf_size >= 32768 && !buf) {
        buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (!buf) buf_size /= 2;
    }
    if (!buf) {
        buf_size = 32768;
        buf = malloc(buf_size);  /* Internal RAM fallback */
    }
    if (!buf) {
        ESP_LOGE(TAG, "Cannot allocate replay buffer");
        return ESP_ERR_NO_MEM;
    }

    size_t remaining = length;
    long rpos = src_offset;
    while (remaining > 0) {
        size_t n = remaining > buf_size ? buf_size : remaining;
        fseek(enc->fp, rpos, SEEK_SET);
        size_t got = fread(buf, 1, n, enc->fp);
        if (got == 0) break;
        fseek(enc->fp, write_pos, SEEK_SET);
        fwrite(buf, 1, got, enc->fp);
        write_pos += got;
        rpos += got;
        remaining -= got;
    }
    free(buf);

    enc->frame_count++;
    ESP_LOGI(TAG, "Replayed frame: %zu bytes from offset %ld", length, src_offset);
    return ESP_OK;
}

esp_err_t gif_encoder_pass2_write_raw_frame(gif_encoder_t *enc,
                                             const void *data, size_t length)
{
    if (!enc || !enc->fp || !data) return ESP_ERR_INVALID_STATE;
    fwrite(data, 1, length, enc->fp);
    enc->frame_count++;
    return ESP_OK;
}

esp_err_t gif_encoder_read_back(gif_encoder_t *enc, long offset, void *buf, size_t length)
{
    if (!enc || !enc->fp || !buf) return ESP_ERR_INVALID_STATE;
    fflush(enc->fp);
    long save_pos = ftell(enc->fp);
    fseek(enc->fp, offset, SEEK_SET);
    size_t got = fread(buf, 1, length, enc->fp);
    fseek(enc->fp, save_pos, SEEK_SET);
    return (got == length) ? ESP_OK : ESP_FAIL;
}

esp_err_t gif_encoder_pass2_finish(gif_encoder_t *enc)
{
    if (!enc->fp) return ESP_ERR_INVALID_STATE;

    /* GIF Trailer */
    uint8_t trailer = 0x3B;
    fwrite(&trailer, 1, 1, enc->fp);

    fclose(enc->fp);
    enc->fp = NULL;

    ESP_LOGI(TAG, "GIF complete: %d frames", enc->frame_count);
    return ESP_OK;
}

esp_err_t gif_encoder_pass1_add_frame_from_buffer(gif_encoder_t *enc,
    const uint8_t *jpeg_data, size_t jpeg_size, const gif_crop_rect_t *crop)
{
    esp_err_t ret = decode_and_scale_jpeg(enc, jpeg_data, jpeg_size, crop);
    if (ret != ESP_OK) return ret;

    ret = gif_quantize_accumulate_rgb565(enc->quantizer, enc->scaled_buf,
                                         enc->width, enc->height, 4);
    enc->total_frames++;
    if (enc->progress_cb)
        enc->progress_cb(enc->total_frames, 0, 1, enc->progress_user);
    return ret;
}

esp_err_t gif_encoder_pass2_add_frame_from_buffer(gif_encoder_t *enc,
    const uint8_t *jpeg_data, size_t jpeg_size, const gif_crop_rect_t *crop)
{
    uint32_t t_start = esp_log_timestamp();

    esp_err_t ret = decode_and_scale_jpeg(enc, jpeg_data, jpeg_size, crop);
    if (ret != ESP_OK) return ret;

    uint32_t t_decode = esp_log_timestamp();

    write_frame_header(enc);

    /* Reuse the same dithering + LZW encoding as pass2_add_frame */
    gif_lzw_enc_t *lzw = NULL;
    ret = gif_lzw_enc_create(8, enc->fp, &lzw);
    if (ret != ESP_OK) return ret;

    int w = enc->width, h = enc->height;

    uint8_t *lut = heap_caps_malloc(65536, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bool lut_internal = (lut != NULL);
    if (lut) {
        memcpy(lut, enc->pixel_lut, 65536);
    } else {
        lut = enc->pixel_lut;
    }

    uint8_t pal_r[256], pal_g[256], pal_b[256];
    for (int i = 0; i < 256; i++) {
        pal_r[i] = enc->palette.entries[i].r;
        pal_g[i] = enc->palette.entries[i].g;
        pal_b[i] = enc->palette.entries[i].b;
    }

    size_t err_size = w * 3 * sizeof(int16_t);
    int16_t *err_cur = heap_caps_calloc(1, err_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *err_nxt = heap_caps_calloc(1, err_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!err_cur || !err_nxt) {
        if (err_cur) { free(err_cur); err_cur = NULL; }
        if (err_nxt) { free(err_nxt); err_nxt = NULL; }
        err_cur = heap_caps_calloc(1, err_size, MALLOC_CAP_SPIRAM);
        err_nxt = heap_caps_calloc(1, err_size, MALLOC_CAP_SPIRAM);
    }

    uint16_t *row_cache = heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *row_indices = heap_caps_malloc(w, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!row_indices) row_indices = heap_caps_malloc(w, MALLOC_CAP_SPIRAM);

    for (int y = 0; y < h; y++) {
        const uint16_t *row_px;
        if (row_cache) {
            memcpy(row_cache, &enc->scaled_buf[y * w], w * sizeof(uint16_t));
            row_px = row_cache;
        } else {
            row_px = &enc->scaled_buf[y * w];
        }
        int16_t *restrict ec = err_cur;
        int16_t *restrict en = err_nxt;
        const int last_row = (y + 1 >= h);

        for (int x = 0; x < w; x++) {
            uint16_t px = row_px[x];
            int r = ((((px >> 11) & 0x1F) * 527 + 23) >> 6) + ec[x*3];
            int g = ((((px >> 5) & 0x3F) * 259 + 33) >> 6) + ec[x*3+1];
            int b = (((px & 0x1F) * 527 + 23) >> 6)        + ec[x*3+2];
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            uint16_t d16 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            uint8_t idx = lut[d16];
            row_indices[x] = idx;
            int er = r - pal_r[idx];
            int eg = g - pal_g[idx];
            int eb = b - pal_b[idx];
            if (x + 1 < w) {
                ec[(x+1)*3]   += er * 7 >> 4;
                ec[(x+1)*3+1] += eg * 7 >> 4;
                ec[(x+1)*3+2] += eb * 7 >> 4;
            }
            if (!last_row) {
                if (x > 0) {
                    en[(x-1)*3]   += er * 3 >> 4;
                    en[(x-1)*3+1] += eg * 3 >> 4;
                    en[(x-1)*3+2] += eb * 3 >> 4;
                }
                en[x*3]   += er * 5 >> 4;
                en[x*3+1] += eg * 5 >> 4;
                en[x*3+2] += eb * 5 >> 4;
                if (x + 1 < w) {
                    en[(x+1)*3]   += er >> 4;
                    en[(x+1)*3+1] += eg >> 4;
                    en[(x+1)*3+2] += eb >> 4;
                }
            }
        }
        for (int x = 0; x < w; x++)
            gif_lzw_enc_pixel(lzw, row_indices[x]);
        int16_t *tmp = err_cur; err_cur = err_nxt; err_nxt = tmp;
        gif_simd_memzero_s16(err_nxt, w * 3);
    }

    heap_caps_free(err_cur);
    heap_caps_free(err_nxt);
    if (row_cache) free(row_cache);
    if (row_indices) free(row_indices);
    if (lut_internal) free(lut);

    uint32_t t_enc = esp_log_timestamp();
    gif_lzw_enc_finish(lzw);
    gif_lzw_enc_destroy(lzw);

    ESP_LOGI(TAG, "Frame timing: decode=%lums encode=%lums total=%lums",
             t_decode - t_start, t_enc - t_decode, esp_log_timestamp() - t_start);

    enc->frame_count++;
    if (enc->progress_cb)
        enc->progress_cb(enc->frame_count, enc->total_frames, 2, enc->progress_user);
    return ESP_OK;
}

void gif_encoder_destroy(gif_encoder_t *enc)
{
    if (!enc) return;

    if (enc->fp) {
        fclose(enc->fp);
        enc->fp = NULL;
    }
    if (enc->jpeg_buf) free(enc->jpeg_buf);
    if (enc->decode_buf) free(enc->decode_buf);
    if (enc->scaled_buf) heap_caps_free(enc->scaled_buf);
    if (enc->pixel_lut) heap_caps_free(enc->pixel_lut);
    if (enc->quantizer) gif_quantize_destroy(enc->quantizer);
    if (enc->jpeg_handle) jpeg_del_decoder_engine(enc->jpeg_handle);
    if (enc->ppa_handle) ppa_unregister_client(enc->ppa_handle);

    free(enc);
}
