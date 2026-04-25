/**
 * @file host_encode_main.c
 * @brief Host-side encode harness for the P4MSLO GIF pipeline.
 *
 * Drives `gif_encoder_t` against the 4 fixture JPEGs in `debug_gifs/`,
 * producing a `.gif` we can byte-inspect (or diff against a known-good
 * reference) without flashing the P4. Built with AddressSanitizer to
 * catch the same heap-corruption class that fires on-device as
 * `tlsf_control_functions.h:374` panics.
 *
 * Specifically validates:
 *   - Pass 1 → Pass 2 sequence completes without ASan diagnostics
 *   - encoder destroy frees everything (no leaks under -fsanitize=leak)
 *   - 50× back-to-back create/encode/destroy stress matches the
 *     "after a recent encode" panic trigger we see on hardware
 *
 * Usage:
 *   ./host_encode <fixture_dir> <output.gif> [--stress N]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "gif_encoder.h"

#define MAX_CAMS 4

/* PIMSLO parallax: same logic as app_gifs.c::app_gifs_encode_pimslo_from_dir. */
static void compute_crops(int src_w, int src_h, int num_cams, float strength,
                          gif_crop_rect_t *crops_out, int *crop_w_out, int *square_out)
{
    int square = (src_w < src_h) ? src_w : src_h;
    int h_margin = (src_w - square) / 2;
    int v_margin = (src_h - square) / 2;
    int total_parallax = (int)(square * strength);
    int crop_w = square - total_parallax;
    for (int i = 0; i < num_cams; i++) {
        float ratio = (num_cams > 1) ? (float)i / (num_cams - 1) : 0.0f;
        int parallax_offset = (int)(ratio * total_parallax);
        crops_out[i].x = h_margin + parallax_offset;
        crops_out[i].y = v_margin;
        crops_out[i].w = crop_w;
        crops_out[i].h = square;
    }
    *crop_w_out = crop_w;
    *square_out = square;
}

static int load_jpeg(const char *path, uint8_t **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }
    *out_data = malloc((size_t)sz);
    if (!*out_data) { fclose(f); return -1; }
    if (fread(*out_data, 1, (size_t)sz, f) != (size_t)sz) {
        free(*out_data); *out_data = NULL; fclose(f); return -1;
    }
    fclose(f);
    *out_size = (size_t)sz;
    return 0;
}

/* tjpgd dimensions from a JPEG buffer — we need them up front to compute
 * the parallax crop. Borrowed from gif_tjpgd's prepare path. */
#include "gif_tjpgd.h"
typedef struct { const uint8_t *p; size_t sz; size_t pos; } stream_t;
static size_t stream_in(JDEC *jd, uint8_t *buf, size_t len)
{
    stream_t *s = jd->device;
    size_t avail = s->sz > s->pos ? s->sz - s->pos : 0;
    if (len > avail) len = avail;
    if (buf) memcpy(buf, s->p + s->pos, len);
    s->pos += len;
    return len;
}
static int peek_jpeg_size(const uint8_t *data, size_t size, int *w, int *h)
{
    stream_t st = { .p = data, .sz = size, .pos = 0 };
    static uint8_t work[32768] __attribute__((aligned(4)));
    JDEC jd;
    if (gif_jd_prepare(&jd, stream_in, work, sizeof(work), &st) != JDR_OK) return -1;
    *w = jd.width; *h = jd.height;
    return 0;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int run_one_encode(const char *fixture_dir, const char *output_path)
{
    uint8_t *jpeg_data[MAX_CAMS] = {0};
    size_t   jpeg_size[MAX_CAMS] = {0};
    int loaded = 0;

    for (int i = 0; i < MAX_CAMS; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/pimslo_pos%d.jpg", fixture_dir, i + 1);
        if (load_jpeg(path, &jpeg_data[i], &jpeg_size[i]) != 0) {
            fprintf(stderr, "skip: %s not found\n", path);
            continue;
        }
        printf("loaded %s: %zu bytes\n", path, jpeg_size[i]);
        loaded++;
    }
    if (loaded < 2) { fprintf(stderr, "need ≥2 cams, got %d\n", loaded); return 1; }

    int src_w = 0, src_h = 0;
    if (peek_jpeg_size(jpeg_data[0], jpeg_size[0], &src_w, &src_h) != 0) {
        fprintf(stderr, "bad jpeg header\n"); return 1;
    }
    printf("source: %dx%d, %d cameras\n", src_w, src_h, loaded);

    gif_crop_rect_t crops[MAX_CAMS];
    int crop_w, square;
    compute_crops(src_w, src_h, loaded, 0.05f, crops, &crop_w, &square);
    printf("crop: %dx%d (parallax=0.05)\n", crop_w, square);

    gif_encoder_config_t cfg = {
        .frame_delay_cs = 15,
        .loop_count = 0,
        .target_width = crop_w,
        .target_height = square,
    };
    gif_encoder_t *enc = NULL;
    if (gif_encoder_create(&cfg, &enc) != ESP_OK) {
        fprintf(stderr, "encoder create failed\n"); return 1;
    }

    /* Pass 1 — palette */
    double t0 = now_ms();
    for (int i = 0; i < loaded; i++) {
        gif_encoder_pass1_add_frame_from_buffer(enc, jpeg_data[i], jpeg_size[i], &crops[i]);
    }
    if (gif_encoder_pass1_finalize(enc) != ESP_OK) {
        fprintf(stderr, "pass1 finalize failed\n");
        gif_encoder_destroy(enc); return 1;
    }
    double t_pass1 = now_ms() - t0;

    /* Pass 2 — encode */
    if (gif_encoder_pass2_begin(enc, output_path) != ESP_OK) {
        fprintf(stderr, "pass2 begin failed\n");
        gif_encoder_destroy(enc); return 1;
    }
    t0 = now_ms();
    for (int i = 0; i < loaded; i++) {
        gif_encoder_pass2_add_frame_from_buffer(enc, jpeg_data[i], jpeg_size[i], &crops[i]);
    }
    /* Reverse oscillation: encode middle frames in reverse (no replay
     * optimization on host, just re-encode). */
    for (int i = loaded - 2; i >= 1; i--) {
        gif_encoder_pass2_add_frame_from_buffer(enc, jpeg_data[i], jpeg_size[i], &crops[i]);
    }
    if (gif_encoder_pass2_finish(enc) != ESP_OK) {
        fprintf(stderr, "pass2 finish failed\n");
        gif_encoder_destroy(enc); return 1;
    }
    double t_pass2 = now_ms() - t0;

    gif_encoder_destroy(enc);

    for (int i = 0; i < MAX_CAMS; i++) free(jpeg_data[i]);

    struct stat st;
    if (stat(output_path, &st) == 0) {
        printf("OK: %s (%lld bytes)  pass1=%.0fms pass2=%.0fms\n",
               output_path, (long long)st.st_size, t_pass1, t_pass2);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *fixture_dir = (argc > 1) ? argv[1] : "debug_gifs";
    const char *output_path = (argc > 2) ? argv[2] : "/tmp/host_encode_out.gif";
    int stress = 1;
    if (argc > 3 && strcmp(argv[3], "--stress") == 0 && argc > 4) {
        stress = atoi(argv[4]);
    }

    for (int i = 0; i < stress; i++) {
        if (stress > 1) printf("\n=== run %d/%d ===\n", i + 1, stress);
        int rc = run_one_encode(fixture_dir, output_path);
        if (rc != 0) { fprintf(stderr, "FAIL on run %d\n", i + 1); return rc; }
    }
    printf("\nAll %d run(s) PASSED\n", stress);
    return 0;
}
