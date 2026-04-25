/**
 * @file pimslo_sim.c
 * @brief End-to-end PIMSLO pipeline simulator.
 *
 * Single-threaded model — no real pthreads. Time is virtual: each
 * operation advances a simulated clock by its modeled duration. Tasks
 * are state machines that the test driver pumps via pimslo_sim_step()
 * or by calling the user-facing actions (photo_btn, gallery_next, etc).
 *
 * This is sufficient to validate ARCHITECTURE properties:
 *   - "does the encoder fit in the available memory?"
 *   - "does the encode complete under the timing budget?"
 *   - "does the bg worker not run on UI_PAGE_CAMERA?"
 *
 * For race-condition / scheduling validation we'd need pthreads, but
 * we can layer that on later.
 */
#include "pimslo_sim.h"
#include "p4_mem_model.h"
#include "p4_budget.h"
#include "p4_timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ========================================================================
 * Page state
 * ======================================================================== */
static ui_page_t s_page = UI_PAGE_MAIN;
void ui_extra_set_current_page(ui_page_t p) { s_page = p; }
ui_page_t ui_extra_get_current_page(void) { return s_page; }

/* ========================================================================
 * Gallery state
 * ======================================================================== */
static gallery_entry_t s_entries[GALLERY_MAX_ENTRIES];
static int s_n_entries = 0;
static int s_current_index = 0;
static bool s_is_encoding = false;
static bool s_is_playing  = false;
static bool s_gallery_ever_opened = false;

void gallery_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_n_entries = 0;
    s_current_index = 0;
    s_is_encoding = false;
    s_is_playing  = false;
    s_gallery_ever_opened = false;
}
void gallery_scan(void)
{
    /* In the real app, scan walks /sdcard/p4mslo_gifs and
     * /sdcard/p4mslo_previews and merges by stem. Here we just keep
     * the in-memory list — gallery_record_capture is what populates. */
    /* Sort by stem for stable ordering. */
    for (int i = 1; i < s_n_entries; i++) {
        for (int j = i; j > 0 && strcmp(s_entries[j-1].stem, s_entries[j].stem) > 0; j--) {
            gallery_entry_t tmp = s_entries[j-1];
            s_entries[j-1] = s_entries[j];
            s_entries[j]   = tmp;
        }
    }
}
int gallery_count(void) { return s_n_entries; }
int gallery_current_index(void) { return s_current_index; }
const gallery_entry_t *gallery_current(void)
{
    if (s_current_index < 0 || s_current_index >= s_n_entries) return NULL;
    return &s_entries[s_current_index];
}
void gallery_next(void) { if (s_current_index + 1 < s_n_entries) s_current_index++; }
void gallery_prev(void) { if (s_current_index > 0) s_current_index--; }
bool gallery_is_encoding(void) { return s_is_encoding; }
bool gallery_is_playing(void)  { return s_is_playing; }
bool gallery_was_ever_opened(void) { return s_gallery_ever_opened; }
void gallery_mark_opened(void) { s_gallery_ever_opened = true; }

void gallery_delete_current(void)
{
    if (s_current_index < 0 || s_current_index >= s_n_entries) return;
    /* Shift everything after the deleted slot down. */
    for (int i = s_current_index; i < s_n_entries - 1; i++) {
        s_entries[i] = s_entries[i + 1];
    }
    s_n_entries--;
    memset(&s_entries[s_n_entries], 0, sizeof(s_entries[s_n_entries]));
    /* Clamp index — if we deleted the last entry, step back to the
     * new last entry (or to 0 if the gallery is now empty). Mirrors
     * app_gifs_delete_current()'s behavior. */
    if (s_current_index >= s_n_entries) {
        s_current_index = s_n_entries > 0 ? s_n_entries - 1 : 0;
    }
}

void gallery_record_capture(const char *stem, bool has_gif, bool has_jpeg, bool has_p4ms)
{
    /* Update if existing, else insert. */
    for (int i = 0; i < s_n_entries; i++) {
        if (strcmp(s_entries[i].stem, stem) == 0) {
            s_entries[i].has_gif  |= has_gif;
            s_entries[i].has_jpeg |= has_jpeg;
            s_entries[i].has_p4ms |= has_p4ms;
            s_entries[i].type = s_entries[i].has_gif ? GALLERY_ENTRY_GIF : GALLERY_ENTRY_JPEG;
            return;
        }
    }
    if (s_n_entries >= GALLERY_MAX_ENTRIES) return;
    gallery_entry_t *e = &s_entries[s_n_entries++];
    snprintf(e->stem, sizeof(e->stem), "%s", stem);
    snprintf(e->gif_path,  sizeof(e->gif_path),  "/sdcard/p4mslo_gifs/%s.gif", stem);
    snprintf(e->jpeg_path, sizeof(e->jpeg_path), "/sdcard/p4mslo_previews/%s.jpg", stem);
    snprintf(e->p4ms_path, sizeof(e->p4ms_path), "/sdcard/p4mslo_small/%s.p4ms", stem);
    e->has_gif  = has_gif;
    e->has_jpeg = has_jpeg;
    e->has_p4ms = has_p4ms;
    e->type = has_gif ? GALLERY_ENTRY_GIF : GALLERY_ENTRY_JPEG;
}

size_t pimslo_sim_internal_largest(void) {
    return p4_mem_pool_state(P4_POOL_INT).largest_contiguous;
}
size_t pimslo_sim_psram_largest(void) {
    return p4_mem_pool_state(P4_POOL_PSRAM).largest_contiguous;
}

/* ========================================================================
 * Album JPEG decoder model (mirrors app_album.c::album_ctx)
 * ======================================================================== */
#define ALBUM_PPA_BYTES (1920 * 1088 * 3)  /* ~6 MB PSRAM */

static struct {
    void  *ppa_buffer;
    bool   hw_decoder_alive;
    int    release_count;
    int    reacquire_count;
    int    reacquire_fail_count;
    int    force_fail_remaining;
} s_album;

void album_decoder_init(void)
{
    /* HW decoder handle is just a flag — no PSRAM cost. PPA buffer is
     * the ~6 MB allocation. In real firmware, this happens once when
     * the user first enters the album page. Tests that don't open the
     * album never call this. */
    if (!s_album.ppa_buffer) {
        s_album.ppa_buffer = p4_mem_malloc(ALBUM_PPA_BYTES, P4_POOL_PSRAM);
    }
    s_album.hw_decoder_alive = true;
}
void album_decoder_release_ppa(void)
{
    if (s_album.ppa_buffer) {
        p4_mem_free(s_album.ppa_buffer);
        s_album.ppa_buffer = NULL;
    }
    /* HW decoder STAYS ALIVE — comment in app_album.c:891 explains why. */
    s_album.release_count++;
}
bool album_decoder_reacquire_ppa(void)
{
    s_album.reacquire_count++;
    if (s_album.force_fail_remaining > 0) {
        s_album.force_fail_remaining--;
        s_album.reacquire_fail_count++;
        return false;
    }
    if (!s_album.ppa_buffer) {
        s_album.ppa_buffer = p4_mem_malloc(ALBUM_PPA_BYTES, P4_POOL_PSRAM);
        if (!s_album.ppa_buffer) {
            s_album.reacquire_fail_count++;
            return false;
        }
    }
    return true;
}
bool   album_ppa_held(void)              { return s_album.ppa_buffer != NULL; }
bool   album_hw_decoder_alive(void)      { return s_album.hw_decoder_alive; }
size_t album_ppa_size(void)              { return ALBUM_PPA_BYTES; }
int    album_release_count(void)         { return s_album.release_count; }
int    album_reacquire_count(void)       { return s_album.reacquire_count; }
int    album_reacquire_fail_count(void)  { return s_album.reacquire_fail_count; }
void   album_force_next_reacquire_fail(int n) { s_album.force_fail_remaining = n; }

/* ========================================================================
 * Architecture switch
 * ======================================================================== */
static pimslo_sim_arch_t s_arch = PIMSLO_ARCH_BASELINE;
pimslo_sim_arch_t pimslo_sim_architecture(void) { return s_arch; }
void pimslo_sim_set_architecture(pimslo_sim_arch_t a) { s_arch = a; }

/* The arch determines whether the encoder task's stack lives in
 * INTERNAL (proposed: BSS-resident) or PSRAM (baseline: FreeRTOS
 * fallback). That's the original knob.
 *
 * The follow-up knob is LUT placement. PROPOSED has the stack fixed
 * but the 64 KB pixel_lut still lives in PSRAM (won't fit the new
 * 31 KB largest internal block). The LUT_FIX architectures replace
 * it with a smaller alternative. */
static p4_stack_location_t encoder_stack_location(void)
{
    return (s_arch == PIMSLO_ARCH_BASELINE) ? P4_STACK_PSRAM : P4_STACK_INTERNAL;
}

static p4_lut_location_t encoder_lut_location(void)
{
    switch (s_arch) {
        case PIMSLO_ARCH_BASELINE:              return P4_LUT_PSRAM;
        case PIMSLO_ARCH_PROPOSED:              return P4_LUT_PSRAM;
        case PIMSLO_ARCH_PROPOSED_OCTREE_HPRAM: return P4_LUT_OCTREE_HPRAM;
        case PIMSLO_ARCH_PROPOSED_OCTREE_TCM:   return P4_LUT_OCTREE_TCM;
        case PIMSLO_ARCH_PROPOSED_RGB444:       return P4_LUT_RGB444;
        case PIMSLO_ARCH_PROPOSED_BSS_LUT:      return P4_LUT_INTERNAL;
        default: return P4_LUT_PSRAM;
    }
}

/* ========================================================================
 * Capture counter — assigns P4Mxxxx stems
 * ======================================================================== */
static int s_capture_counter = 1;
static int s_force_fail_remaining = 0;
static int s_force_fail_cams = 0;

void pimslo_sim_force_capture_fail(int n, int cams) {
    s_force_fail_remaining = n; s_force_fail_cams = cams;
}

/* ========================================================================
 * Task pipeline state machine
 *
 * Encode pipeline runs entirely synchronously here — when a save job is
 * "queued", we immediately run the encoder (subject to defer logic).
 * This is fine for architecture validation because we're checking
 * memory + timing, not concurrency.
 * ======================================================================== */

#define ENCODE_QUEUE_DEPTH_MAX 8
typedef struct {
    int  n_cams;
    char stem[16];
} encode_job_t;
static encode_job_t s_encode_queue[ENCODE_QUEUE_DEPTH_MAX];
static int s_encode_queue_head = 0;  /* next to dequeue */
static int s_encode_queue_tail = 0;  /* next free slot */
#define s_encode_queue_depth ((s_encode_queue_tail - s_encode_queue_head + ENCODE_QUEUE_DEPTH_MAX) % ENCODE_QUEUE_DEPTH_MAX)

static int s_p4ms_count = 0;
static int s_gif_count  = 0;

static int s_bg_pre_render_count = 0;
static int s_bg_re_encode_count  = 0;

static int64_t s_sim_clock_ms = 0;

static bool encode_should_defer(void)
{
    /* Mirror app_pimslo.c::encode_should_defer — current state on
     * fix/pimslo-encode-stuck. MAIN is allowed; only camera-type
     * pages are excluded. */
    ui_page_t p = ui_extra_get_current_page();
    if (p == UI_PAGE_CAMERA ||
        p == UI_PAGE_INTERVAL_CAM ||
        p == UI_PAGE_VIDEO_MODE) return true;
    if (gallery_is_encoding()) return true;
    return false;
}

static int run_encode_pipeline(int n_cams, const char *stem)
{
    s_is_encoding = true;

    /* Drop ~6 MB PPA buffer to make PSRAM available — matches the
     * app_album_release_jpeg_decoder() call in app_gifs.c:1232. The HW
     * decoder handle stays alive (release does NOT destroy it). */
    album_decoder_release_ppa();

    p4_pipeline_timing_t t = p4_timing_estimate((p4_pipeline_params_t){
        .n_cams = n_cams, .stack = encoder_stack_location(),
        .lut = encoder_lut_location(), .save_p4ms = true,
    });

    /* Allocate encoder buffers (PSRAM). The 7 MB scaled_buf is what
     * the PPA-release was making room for — without that release, the
     * PIMSLO encoder collides with album. */
    void *scaled_buf = p4_mem_malloc(1824 * 1920 * 2, P4_POOL_PSRAM);
    void *pixel_lut  = p4_mem_malloc(65536, P4_POOL_PSRAM);

    s_sim_clock_ms += t.total_ms;

    if (scaled_buf) p4_mem_free(scaled_buf);
    if (pixel_lut)  p4_mem_free(pixel_lut);

    /* Mirror app_gifs.c:1290 — try to reacquire the PPA buffer. Failure
     * is non-fatal: the album view degrades to no full-res preview, the
     * rest of the UI keeps working. */
    album_decoder_reacquire_ppa();

    s_is_encoding = false;

    /* Record outputs to the gallery. .p4ms ALWAYS produced (direct-JPEG
     * path inside encode pipeline). .gif produced when encode finishes. */
    if (n_cams >= 2) {
        gallery_record_capture(stem,
                               /* has_gif  */ true,
                               /* has_jpeg */ true,
                               /* has_p4ms */ true);
        s_p4ms_count++;
        s_gif_count++;
    }
    return t.total_ms;
}

pimslo_sim_capture_result_t pimslo_sim_photo_btn(int cams_to_simulate)
{
    pimslo_sim_capture_result_t r = {0};

    if (s_force_fail_remaining > 0) {
        cams_to_simulate = s_force_fail_cams;
        s_force_fail_remaining--;
    }
    r.cams_usable = cams_to_simulate;

    /* Capture: GPIO trigger + SPI receive 4 cams. ~600 ms trigger +
     * 4×~300 ms transfer = ~1700 ms when 4/4 succeed. */
    r.capture_ms = 600 + 300 * cams_to_simulate;
    s_sim_clock_ms += r.capture_ms;

    if (cams_to_simulate < 2) {
        /* Drop — capture task discards if <2 cams. */
        return r;
    }

    snprintf(r.stem, sizeof(r.stem), "P4M%04d", s_capture_counter++);

    /* Save: fwrite 4×500 KB JPEG + 1 preview. ~250 KB/s SD = ~1.5 s
     * per file × n + preview ≈ 6.8 s for 4 cams. */
    r.save_ms = (1700 * cams_to_simulate) + 700;
    s_sim_clock_ms += r.save_ms;

    /* Save task creates a JPEG-only gallery entry immediately. */
    gallery_record_capture(r.stem, false, true, false);

    /* Queue encode. The encode runs inside pimslo_sim_wait_idle() so
     * the caller can measure end-to-end latency including encoder time
     * and so we mirror the real device's "encode runs after photo_btn
     * returns" behavior. */
    int next = (s_encode_queue_tail + 1) % ENCODE_QUEUE_DEPTH_MAX;
    if (next != s_encode_queue_head) {
        s_encode_queue[s_encode_queue_tail].n_cams = cams_to_simulate;
        memcpy(s_encode_queue[s_encode_queue_tail].stem, r.stem, sizeof(r.stem));
        s_encode_queue_tail = next;
    }

    return r;
}

int pimslo_sim_wait_idle(int max_wait_ms)
{
    int waited = 0;
    while (s_encode_queue_depth > 0 && waited < max_wait_ms) {
        if (encode_should_defer()) {
            /* Idle — encoder is waiting for safe page. */
            s_sim_clock_ms += 100; waited += 100;
            continue;
        }
        encode_job_t job = s_encode_queue[s_encode_queue_head];
        s_encode_queue_head = (s_encode_queue_head + 1) % ENCODE_QUEUE_DEPTH_MAX;
        int t = run_encode_pipeline(job.n_cams, job.stem);
        waited += t;
    }
    return waited;
}

/* ========================================================================
 * Init / shutdown
 * ======================================================================== */
void pimslo_sim_init(void)
{
    /* Apply the BASELINE budget (everything that exists at boot before
     * any photo button press). Use AS_IS mode so the model state
     * already reflects post-boot heap_caps numbers. */
    p4_budget_simulate(P4_BUDGET_BASELINE, P4_BUDGET_BASELINE_COUNT,
                       P4_BUDGET_MODE_AS_IS, NULL);
    gallery_init();
    s_page = UI_PAGE_MAIN;
    s_capture_counter = 1;
    s_p4ms_count = s_gif_count = 0;
    s_bg_pre_render_count = s_bg_re_encode_count = 0;
    s_encode_queue_head = s_encode_queue_tail = 0;
    s_sim_clock_ms = 0;
    memset(&s_album, 0, sizeof(s_album));
}

void pimslo_sim_shutdown(void)
{
    /* p4_mem_init resets everything for next test. */
}

void pimslo_sim_reset(void)
{
    p4_mem_init(P4_MEM_MODEL_DEFAULT);
    pimslo_sim_init();
}

/* ========================================================================
 * Background worker
 *
 * In real firmware, gif_bg runs on Core 1 at low priority. It walks:
 *   1. .gif files without matching .p4ms  → pre-render
 *   2. JPEG-only captures (encode interrupted) → re-encode
 *
 * Both are gated by bg_should_yield() (15s after gallery nav) and
 * bg_encode_safe_page() (NOT UI_PAGE_GIFS for the encode path).
 *
 * Here we simulate by walking the gallery and running the appropriate
 * fix-up for each entry. Test code calls bg_worker_kick() to force a
 * pass.
 * ======================================================================== */

bool bg_worker_should_yield(void)
{
    /* Yield on UI_PAGE_GIFS for the encode path; pre-render is OK
     * anywhere. We model the simpler "yield on GIFS" rule. */
    return ui_extra_get_current_page() == UI_PAGE_GIFS;
}

void bg_worker_kick(void)
{
    if (bg_worker_should_yield()) return;
    if (encode_should_defer())    return;

    /* Pass 1: pre-render .p4ms for any .gif that doesn't have one. */
    for (int i = 0; i < s_n_entries; i++) {
        gallery_entry_t *e = &s_entries[i];
        if (e->has_gif && !e->has_p4ms) {
            /* tjpgd-decode the JPEG fixture, save as .p4ms.
             * ~1.5 s per pre-render in real firmware. */
            s_sim_clock_ms += 1500;
            e->has_p4ms = true;
            s_bg_pre_render_count++;
        }
    }

    /* Pass 2: re-encode any JPEG-only captures (encode crashed/interrupted). */
    for (int i = 0; i < s_n_entries; i++) {
        gallery_entry_t *e = &s_entries[i];
        if (e->has_jpeg && !e->has_gif) {
            run_encode_pipeline(4, e->stem);
            s_bg_re_encode_count++;
        }
    }
}

int bg_worker_pre_render_count(void) { return s_bg_pre_render_count; }
int bg_worker_re_encode_count(void)  { return s_bg_re_encode_count; }
