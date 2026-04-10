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
    void *decode_buf;       /* JPEG decode output (RGB565) */
    size_t decode_buf_size;
    uint16_t *scaled_buf;   /* PPA output (RGB565, target resolution) */
    size_t scaled_buf_size;

    /* Output */
    FILE *fp;
    int frame_count;
    int width, height;      /* Resolved target dimensions */
    size_t cache_line_size;

    /* Progress */
    gif_encoder_progress_cb_t progress_cb;
    void *progress_user;
    int total_frames;
    int current_pass;
};

/* ---- Helpers ---- */

/* The JPEG decoder outputs BGR565 (JPEG_DEC_RGB_ELEMENT_ORDER_BGR).
 * In BGR565: bits [15:11]=B, [10:5]=G, [4:0]=R */
static inline void bgr565_to_rgb888(uint16_t px, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t b5 = (px >> 11) & 0x1F;
    uint8_t g6 = (px >> 5) & 0x3F;
    uint8_t r5 = px & 0x1F;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

/* Read a JPEG file from SD card, decode it, and scale to target size.
 * Output is in enc->scaled_buf as RGB565. */
static esp_err_t load_and_decode_jpeg(gif_encoder_t *enc, const char *jpeg_path)
{
    /* Read JPEG file */
    FILE *f = fopen(jpeg_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", jpeg_path);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (enc->jpeg_buf) free(enc->jpeg_buf);
    enc->jpeg_buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!enc->jpeg_buf) {
        fclose(f);
        ESP_LOGE(TAG, "OOM for JPEG buffer (%zu bytes)", file_size);
        return ESP_ERR_NO_MEM;
    }

    fread(enc->jpeg_buf, 1, file_size, f);
    fclose(f);

    /* Get JPEG dimensions */
    jpeg_decode_picture_info_t info;
    esp_err_t ret = jpeg_decoder_get_info(enc->jpeg_buf, file_size, &info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG header parse failed: %s", jpeg_path);
        return ret;
    }

    ESP_LOGI(TAG, "JPEG: %ux%u (%zu bytes)", info.width, info.height, file_size);

    /* Set target dimensions from first frame if not specified */
    if (enc->width == 0 || enc->height == 0) {
        enc->width = enc->config.target_width > 0 ? enc->config.target_width : BSP_LCD_H_RES;
        enc->height = enc->config.target_height > 0 ? enc->config.target_height : BSP_LCD_V_RES;
        ESP_LOGI(TAG, "GIF dimensions: %dx%d", enc->width, enc->height);
    }

    /* Allocate decode buffer sized for this specific image (RGB565 output).
     * The hardware JPEG decoder aligns width and height to 16 bytes. */
    uint32_t aligned_w = (info.width + 15) & ~15;
    uint32_t aligned_h = (info.height + 15) & ~15;
    size_t needed = (size_t)aligned_w * aligned_h * 2;
    if (!enc->decode_buf || enc->decode_buf_size < needed) {
        if (enc->decode_buf) {
            heap_caps_free(enc->decode_buf);
            enc->decode_buf = NULL;
        }
        enc->decode_buf = heap_caps_aligned_calloc(enc->cache_line_size, 1,
                                                    needed, MALLOC_CAP_SPIRAM);
        if (!enc->decode_buf) {
            ESP_LOGE(TAG, "Cannot allocate %zu byte decode buffer (free: %zu)",
                     needed, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            free(enc->jpeg_buf);
            enc->jpeg_buf = NULL;
            return ESP_ERR_NO_MEM;
        }
        enc->decode_buf_size = needed;
        ESP_LOGI(TAG, "Allocated decode buffer: %zu bytes", needed);
    }

    /* Decode JPEG to RGB565 */
    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    uint32_t out_size = 0;
    ret = jpeg_decoder_process(enc->jpeg_handle, &decode_cfg,
                               enc->jpeg_buf, file_size,
                               enc->decode_buf, enc->decode_buf_size, &out_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %s (need %zu, have %zu)",
                 jpeg_path, needed, enc->decode_buf_size);
        free(enc->jpeg_buf);
        enc->jpeg_buf = NULL;
        return ret;
    }

    /* Free raw JPEG data early */
    free(enc->jpeg_buf);
    enc->jpeg_buf = NULL;

    /* Allocate scaled output buffer if needed */
    if (!enc->scaled_buf) {
        enc->scaled_buf_size = ALIGN_UP(enc->width * enc->height * 2, enc->cache_line_size);
        enc->scaled_buf = heap_caps_aligned_calloc(enc->cache_line_size, 1,
                                                    enc->scaled_buf_size, MALLOC_CAP_SPIRAM);
        if (!enc->scaled_buf) {
            ESP_LOGE(TAG, "Failed to allocate %zu byte scaled buffer", enc->scaled_buf_size);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Allocated scaled buffer: %zu bytes", enc->scaled_buf_size);
    }

    /* Scale to target resolution using PPA */
    /* Compute square crop from center */
    int crop_size = (info.width < info.height) ? info.width : info.height;

    ppa_srm_oper_config_t srm = {
        .in.buffer = enc->decode_buf,
        .in.pic_w = info.width,
        .in.pic_h = info.height,
        .in.block_w = crop_size,
        .in.block_h = crop_size,
        .in.block_offset_x = (info.width - crop_size) / 2,
        .in.block_offset_y = (info.height - crop_size) / 2,
        .in.srm_cm = PPA_SRM_COLOR_MODE_RGB565,

        .out.buffer = enc->scaled_buf,
        .out.buffer_size = enc->scaled_buf_size,
        .out.pic_w = enc->width,
        .out.pic_h = enc->height,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm = PPA_SRM_COLOR_MODE_RGB565,

        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)enc->width / crop_size,
        .scale_y = (float)enc->height / crop_size,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    ret = ppa_do_scale_rotate_mirror(enc->ppa_handle, &srm);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA scale failed");
        return ret;
    }

    return ESP_OK;
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
    return gif_quantize_build_palette(enc->quantizer, &enc->palette);
}

esp_err_t gif_encoder_pass2_begin(gif_encoder_t *enc, const char *output_path)
{
    enc->fp = fopen(output_path, "wb");
    if (!enc->fp) {
        ESP_LOGE(TAG, "Cannot create %s", output_path);
        return ESP_FAIL;
    }

    write_gif_header(enc);
    enc->frame_count = 0;
    enc->current_pass = 2;

    return ESP_OK;
}

esp_err_t gif_encoder_pass2_add_frame(gif_encoder_t *enc, const char *jpeg_path)
{
    esp_err_t ret = load_and_decode_jpeg(enc, jpeg_path);
    if (ret != ESP_OK) return ret;

    write_frame_header(enc);

    /* LZW encode: map each pixel to palette index and compress */
    gif_lzw_enc_t *lzw = NULL;
    ret = gif_lzw_enc_create(8, enc->fp, &lzw);  /* 8 = min code size for 256 colors */
    if (ret != ESP_OK) return ret;

    int total = enc->width * enc->height;
    for (int i = 0; i < total; i++) {
        uint8_t r, g, b;
        bgr565_to_rgb888(enc->scaled_buf[i], &r, &g, &b);
        uint8_t idx = gif_quantize_map_pixel(&enc->palette, r, g, b);
        gif_lzw_enc_pixel(lzw, idx);
    }

    gif_lzw_enc_finish(lzw);
    gif_lzw_enc_destroy(lzw);

    enc->frame_count++;
    if (enc->progress_cb)
        enc->progress_cb(enc->frame_count, enc->total_frames, 2, enc->progress_user);

    return ESP_OK;
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
    if (enc->quantizer) gif_quantize_destroy(enc->quantizer);
    if (enc->jpeg_handle) jpeg_del_decoder_engine(enc->jpeg_handle);
    if (enc->ppa_handle) ppa_unregister_client(enc->ppa_handle);

    free(enc);
}
