/**
 * @file gif_lzw.c
 * @brief GIF LZW compression and decompression
 *
 * Standard GIF variable-length-code LZW with 4096-entry dictionary.
 * Encoder outputs 255-byte sub-blocks per the GIF spec.
 * Decoder accepts sub-blocks and emits pixel indices.
 */

#include "gif_lzw.h"
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "gif_lzw";

#define MAX_DICT_SIZE  4096
#define MAX_CODE_BITS  12
#define SUB_BLOCK_MAX  255

/* ================================================================== */
/* LZW Encoder                                                        */
/* ================================================================== */

/*
 * Dictionary via hash table with XOR-based hash (no multiply) and
 * generation counter (no memset on reset). Open addressing, 16K slots.
 */

#define HT_SIZE  8192   /* Power of 2, > 4096. 8K × 8 bytes = 64KB */
#define HT_MASK  (HT_SIZE - 1)

typedef struct {
    int32_t key;      /* (prefix << 8) | suffix */
    int16_t code;
    uint16_t gen;
} ht_slot_t;

struct gif_lzw_enc {
    FILE *fp;
    int min_code_size;
    int clear_code;
    int eoi_code;
    int next_code;
    int code_size;
    int current;      /* Current prefix code, -1 = none */
    uint16_t gen;     /* Current generation */

    /* Bit packer */
    uint32_t bits;
    int nbits;

    /* Sub-block buffer */
    uint8_t blk[SUB_BLOCK_MAX];
    int blk_len;

    /* Hash table with generation tagging */
    ht_slot_t *ht;
};

static void enc_flush_blk(gif_lzw_enc_t *e)
{
    if (e->blk_len > 0) {
        uint8_t len = (uint8_t)e->blk_len;
        fwrite(&len, 1, 1, e->fp);
        fwrite(e->blk, 1, e->blk_len, e->fp);
        e->blk_len = 0;
    }
}

static void enc_put_byte(gif_lzw_enc_t *e, uint8_t b)
{
    e->blk[e->blk_len++] = b;
    if (e->blk_len >= SUB_BLOCK_MAX)
        enc_flush_blk(e);
}

static void enc_put_code(gif_lzw_enc_t *e, int code)
{
    e->bits |= ((uint32_t)code << e->nbits);
    e->nbits += e->code_size;
    while (e->nbits >= 8) {
        enc_put_byte(e, (uint8_t)(e->bits & 0xFF));
        e->bits >>= 8;
        e->nbits -= 8;
    }
}

static void enc_reset_dict(gif_lzw_enc_t *e)
{
    e->next_code = e->eoi_code + 1;
    e->code_size = e->min_code_size + 1;
    e->gen++;  /* Invalidate all entries without memset */
    if (e->gen == 0) {
        memset(e->ht, 0, HT_SIZE * sizeof(ht_slot_t));
        e->gen = 1;
    }
}

/* XOR-shift hash — much faster than multiply on RISC-V */
static inline uint32_t ht_hash(int32_t key)
{
    uint32_t h = (uint32_t)key;
    h ^= h >> 16;
    h ^= h >> 8;
    return h & HT_MASK;
}

static inline int enc_ht_find(gif_lzw_enc_t *e, int prefix, uint8_t suffix)
{
    int32_t key = ((int32_t)prefix << 8) | suffix;
    uint32_t idx = ht_hash(key);
    for (;;) {
        ht_slot_t *s = &e->ht[idx];
        if (s->gen != e->gen) return -1;  /* Empty slot (stale generation) */
        if (s->key == key) return s->code;
        idx = (idx + 1) & HT_MASK;
    }
}

static inline void enc_ht_insert(gif_lzw_enc_t *e, int prefix, uint8_t suffix, int code)
{
    int32_t key = ((int32_t)prefix << 8) | suffix;
    uint32_t idx = ht_hash(key);
    while (e->ht[idx].gen == e->gen)
        idx = (idx + 1) & HT_MASK;
    e->ht[idx].key = key;
    e->ht[idx].code = (int16_t)code;
    e->ht[idx].gen = e->gen;
}

esp_err_t gif_lzw_enc_create(int min_code_size, FILE *output, gif_lzw_enc_t **out)
{
    gif_lzw_enc_t *e = heap_caps_calloc(1, sizeof(gif_lzw_enc_t), MALLOC_CAP_SPIRAM);
    if (!e) return ESP_ERR_NO_MEM;

    /* Try internal RAM for the hash table (64KB), fall back to PSRAM */
    e->ht = heap_caps_calloc(HT_SIZE, sizeof(ht_slot_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!e->ht)
        e->ht = heap_caps_calloc(HT_SIZE, sizeof(ht_slot_t), MALLOC_CAP_SPIRAM);
    if (!e->ht) {
        heap_caps_free(e);
        return ESP_ERR_NO_MEM;
    }

    e->fp = output;
    e->min_code_size = min_code_size;
    e->clear_code = 1 << min_code_size;
    e->eoi_code = e->clear_code + 1;
    e->current = -1;

    /* Write LZW minimum code size byte */
    uint8_t mcs = (uint8_t)min_code_size;
    fwrite(&mcs, 1, 1, output);

    enc_reset_dict(e);
    enc_put_code(e, e->clear_code);

    *out = e;
    return ESP_OK;
}

esp_err_t gif_lzw_enc_pixel(gif_lzw_enc_t *e, uint8_t index)
{
    if (e->current < 0) {
        e->current = index;
        return ESP_OK;
    }

    int found = enc_ht_find(e, e->current, index);
    if (found >= 0) {
        e->current = found;
        return ESP_OK;
    }

    /* Output current code */
    enc_put_code(e, e->current);

    /* Add new entry to dictionary */
    if (e->next_code < MAX_DICT_SIZE) {
        enc_ht_insert(e, e->current, index, e->next_code);
        if (e->next_code > (1 << e->code_size) - 1 && e->code_size < MAX_CODE_BITS)
            e->code_size++;
        e->next_code++;
    } else {
        /* Dictionary full — emit clear code and reset */
        enc_put_code(e, e->clear_code);
        enc_reset_dict(e);
    }

    e->current = index;
    return ESP_OK;
}

esp_err_t gif_lzw_enc_finish(gif_lzw_enc_t *e)
{
    /* Output final code */
    if (e->current >= 0)
        enc_put_code(e, e->current);

    enc_put_code(e, e->eoi_code);

    /* Flush remaining bits */
    if (e->nbits > 0)
        enc_put_byte(e, (uint8_t)(e->bits & 0xFF));

    enc_flush_blk(e);

    /* Block terminator */
    uint8_t zero = 0;
    fwrite(&zero, 1, 1, e->fp);

    return ESP_OK;
}

void gif_lzw_enc_destroy(gif_lzw_enc_t *e)
{
    if (!e) return;
    if (e->ht) heap_caps_free(e->ht);
    heap_caps_free(e);
}

/* ================================================================== */
/* LZW Decoder                                                        */
/* ================================================================== */

typedef struct {
    int16_t prefix;
    uint8_t suffix;
    uint8_t first;   /* first byte of this code's string */
} dec_entry_t;

struct gif_lzw_dec {
    int min_code_size;
    int clear_code;
    int eoi_code;
    int next_code;
    int code_size;
    int prev_code;
    bool finished;

    /* Bit unpacker */
    uint32_t bits;
    int nbits;

    /* Dictionary */
    dec_entry_t dict[MAX_DICT_SIZE];

    /* Decode stack for reversing LZW strings */
    uint8_t stack[MAX_DICT_SIZE];
};

static void dec_reset(gif_lzw_dec_t *d)
{
    d->next_code = d->eoi_code + 1;
    d->code_size = d->min_code_size + 1;
    d->prev_code = -1;

    /* Initialize root entries */
    for (int i = 0; i < d->clear_code; i++) {
        d->dict[i].prefix = -1;
        d->dict[i].suffix = (uint8_t)i;
        d->dict[i].first = (uint8_t)i;
    }
}

esp_err_t gif_lzw_dec_create(int min_code_size, gif_lzw_dec_t **out)
{
    gif_lzw_dec_t *d = heap_caps_calloc(1, sizeof(gif_lzw_dec_t), MALLOC_CAP_SPIRAM);
    if (!d) return ESP_ERR_NO_MEM;

    d->min_code_size = min_code_size;
    d->clear_code = 1 << min_code_size;
    d->eoi_code = d->clear_code + 1;
    d->finished = false;

    dec_reset(d);

    *out = d;
    return ESP_OK;
}

esp_err_t gif_lzw_dec_feed(gif_lzw_dec_t *d, const uint8_t *data, int len,
                           uint8_t *out_pixels, int out_cap, int *out_len)
{
    int out_pos = 0;
    int data_pos = 0;

    while (data_pos < len || d->nbits >= d->code_size) {
        /* Fill bit buffer */
        while (d->nbits < d->code_size && data_pos < len) {
            d->bits |= ((uint32_t)data[data_pos++] << d->nbits);
            d->nbits += 8;
        }

        if (d->nbits < d->code_size) break;

        /* Extract next code */
        int code = d->bits & ((1 << d->code_size) - 1);
        d->bits >>= d->code_size;
        d->nbits -= d->code_size;

        if (code == d->clear_code) {
            dec_reset(d);
            continue;
        }
        if (code == d->eoi_code) {
            d->finished = true;
            break;
        }

        /* Decode the code to a pixel string */
        int stack_top = 0;

        if (code < d->next_code) {
            /* Known code */
            int c = code;
            while (c >= d->clear_code && c < MAX_DICT_SIZE) {
                if (stack_top >= MAX_DICT_SIZE) break;
                d->stack[stack_top++] = d->dict[c].suffix;
                c = d->dict[c].prefix;
            }
            if (c >= 0 && c < d->clear_code && stack_top < MAX_DICT_SIZE)
                d->stack[stack_top++] = (uint8_t)c;
        } else if (code == d->next_code && d->prev_code >= 0) {
            /* Special case: code == next_code */
            uint8_t first_byte = (d->prev_code < d->clear_code)
                ? (uint8_t)d->prev_code : d->dict[d->prev_code].first;

            d->stack[stack_top++] = first_byte;
            int c = d->prev_code;
            while (c >= d->clear_code && c < MAX_DICT_SIZE) {
                if (stack_top >= MAX_DICT_SIZE) break;
                d->stack[stack_top++] = d->dict[c].suffix;
                c = d->dict[c].prefix;
            }
            if (c >= 0 && c < d->clear_code && stack_top < MAX_DICT_SIZE)
                d->stack[stack_top++] = (uint8_t)c;
        } else {
            ESP_LOGW(TAG, "LZW decode: bad code %d (next=%d)", code, d->next_code);
            continue;
        }

        /* Output pixels in reverse (stack is LIFO) */
        for (int i = stack_top - 1; i >= 0 && out_pos < out_cap; i--)
            out_pixels[out_pos++] = d->stack[i];

        /* Add new dictionary entry */
        if (d->prev_code >= 0 && d->next_code < MAX_DICT_SIZE) {
            uint8_t first = (code < d->next_code)
                ? ((code < d->clear_code) ? (uint8_t)code : d->dict[code].first)
                : ((d->prev_code < d->clear_code) ? (uint8_t)d->prev_code : d->dict[d->prev_code].first);

            d->dict[d->next_code].prefix = (int16_t)d->prev_code;
            d->dict[d->next_code].suffix = first;
            d->dict[d->next_code].first = (d->prev_code < d->clear_code)
                ? (uint8_t)d->prev_code : d->dict[d->prev_code].first;
            d->next_code++;

            if (d->next_code > (1 << d->code_size) - 1 && d->code_size < MAX_CODE_BITS)
                d->code_size++;
        }

        d->prev_code = code;
    }

    *out_len = out_pos;
    return ESP_OK;
}

void gif_lzw_dec_destroy(gif_lzw_dec_t *d)
{
    if (d) heap_caps_free(d);
}
