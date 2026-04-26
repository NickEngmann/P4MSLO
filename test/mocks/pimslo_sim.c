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

/* Orphan tracking — separate from the gallery_entry list because
 * orphans should NEVER appear in the gallery. The mock's gallery_scan
 * must filter them out. */
typedef struct {
    char stem[16];
    bool has_truncated_gif;     /* 0-byte .gif */
    int  pos_files;             /* 0-1 means orphan; >=2 is a real capture */
} orphan_t;
#define MAX_ORPHANS 16
static orphan_t s_orphans[MAX_ORPHANS];
static int s_n_orphans = 0;
static int s_orphan_cleanup_count = 0;

void gallery_inject_orphan_gif(const char *stem)
{
    if (s_n_orphans >= MAX_ORPHANS) return;
    orphan_t *o = &s_orphans[s_n_orphans++];
    snprintf(o->stem, sizeof(o->stem), "%s", stem);
    o->has_truncated_gif = true;
    o->pos_files = 0;
}

void gallery_inject_orphan_capture_dir(const char *stem, int pos_files)
{
    if (s_n_orphans >= MAX_ORPHANS) return;
    orphan_t *o = &s_orphans[s_n_orphans++];
    snprintf(o->stem, sizeof(o->stem), "%s", stem);
    o->has_truncated_gif = false;
    o->pos_files = pos_files;
}

int gallery_orphan_cleanup_count(void) { return s_orphan_cleanup_count; }

/* Empty-overlay visibility — sticky bool that mirrors the LVGL label
 * widget. Set by refresh_empty_overlay (count==0 → visible, count>0
 * → hidden). The bug 3 path was: after format the gallery was empty
 * → overlay shown. User took a photo → gallery has 1 entry, but the
 * gallery-entry path only refreshed the overlay when count==0. So
 * the visible flag stayed true even though count>0 — that's the
 * "Album empty" + "Processing" + filename overlap the user saw. */
static bool s_overlay_visible = false;

bool gallery_overlay_visible(void) { return s_overlay_visible; }

void gallery_refresh_empty_overlay(void)
{
    /* Mirrors app_gifs.c::app_gifs_refresh_empty_overlay — visible
     * when count==0, hidden otherwise. */
    s_overlay_visible = (s_n_entries == 0);
}

/* Mirrors ui_extra.c::ui_extra_redirect_to_gifs_page after the
 * fix: ALWAYS call refresh_empty_overlay after scan, regardless of
 * count. */
void gallery_enter(void)
{
    s_gallery_ever_opened = true;
    ui_extra_set_current_page(UI_PAGE_GIFS);
    gallery_scan();
    gallery_refresh_empty_overlay();   /* THE FIX */
}

void gallery_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_n_entries = 0;
    s_current_index = 0;
    s_is_encoding = false;
    s_is_playing  = false;
    s_gallery_ever_opened = false;
    /* Don't reset s_overlay_visible here — tests want to assert
     * "after format, overlay is visible because gallery is empty AND
     * the scan ran". gallery_refresh_empty_overlay handles that
     * explicitly. */
}
void gallery_scan(void)
{
    /* In the real app, scan walks /sdcard/p4mslo_gifs and
     * /sdcard/p4mslo_previews and merges by stem. Here we just keep
     * the in-memory list — gallery_record_capture is what populates. */
    /* Clean orphans first — mirrors the firmware's pass-1 scan logic
     * that drops 0-byte .gifs and <2-pos capture dirs. */
    int kept = 0;
    for (int i = 0; i < s_n_orphans; i++) {
        orphan_t *o = &s_orphans[i];
        if (o->has_truncated_gif || o->pos_files < 2) {
            s_orphan_cleanup_count++;
            continue;     /* drop */
        }
        s_orphans[kept++] = *o;
    }
    s_n_orphans = kept;

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
 * Capture-error-overlay model
 * ======================================================================== */
#define ERROR_OVERLAY_MS 3000
static int64_t s_error_until_ms = 0;
/* Forward decl — defined at file scope below alongside other sim state. */
extern int64_t s_sim_clock_ms;

bool capture_error_pending(void)
{
    if (s_error_until_ms == 0) return false;
    if (s_sim_clock_ms >= s_error_until_ms) {
        s_error_until_ms = 0;
        return false;
    }
    return true;
}

void simulate_advance_time_ms(int ms) { s_sim_clock_ms += ms; }
int64_t simulate_clock_ms(void) { return s_sim_clock_ms; }

/* ========================================================================
 * Camera viewfinder buffer model — catches stale-pointer use-after-free
 * ======================================================================== */
static struct {
    int    generation;          /* increments on every realloc */
    bool   alive;               /* are buffers currently allocated? */
    int    consumer_cached_gen; /* what gen did consumer cache? -1 = nothing cached */
} s_vf;

void viewfinder_init_buffers(void)
{
    s_vf.generation = 1;
    s_vf.alive = true;
    s_vf.consumer_cached_gen = -1;
}

void viewfinder_free_buffers(void)
{
    s_vf.alive = false;
}

void viewfinder_realloc_buffers(void)
{
    s_vf.alive = true;
    s_vf.generation++;          /* NEW addresses */
}

int viewfinder_buf_generation(void)  { return s_vf.generation; }
bool viewfinder_buffers_alive(void)  { return s_vf.alive; }

void consumer_cache_buffers(void)
{
    s_vf.consumer_cached_gen = s_vf.generation;
}

bool consumer_use_buffers(void)
{
    /* Read via cached pointer. Returns false if stale or freed. */
    if (s_vf.consumer_cached_gen < 0) return false;     /* never cached */
    if (!s_vf.alive) return false;                       /* freed currently */
    if (s_vf.consumer_cached_gen != s_vf.generation) return false; /* stale */
    return true;
}

void consumer_refresh_buffers(void)
{
    /* The FIX: consumer re-reads the current pointers before use.
     * Mirrors `take_and_save_photo` calling `app_video_stream_get_*_buf()`
     * on every entry instead of relying on init-time cache. */
    if (s_vf.alive) {
        s_vf.consumer_cached_gen = s_vf.generation;
    }
}

/* ========================================================================
 * Gallery canvas rendering model (mirrors app_gifs.c::play_current)
 * ======================================================================== */
static struct {
    canvas_state_t state;
    int show_count;
    int show_fail_count;
    int force_fail_remaining;
} s_canvas;

canvas_state_t gallery_canvas_state(void) { return s_canvas.state; }
bool gallery_canvas_is_blue(void)         { return s_canvas.state == CANVAS_BLUE; }
bool gallery_canvas_has_jpeg(void)        { return s_canvas.state == CANVAS_JPEG; }
int  gallery_jpeg_show_count(void)        { return s_canvas.show_count; }
int  gallery_jpeg_show_fail_count(void)   { return s_canvas.show_fail_count; }
void gallery_force_next_jpeg_fail(int n)  { s_canvas.force_fail_remaining = n; }

/* Mirrors app_gifs.c::show_jpeg flow: memset to 0x10 (blue) FIRST,
 * then tjpgd-decode into the canvas. If decode fails, blue stays.
 * Returns 0 on success, -1 on failure (host int — no esp_err_t in
 * pimslo_sim.c's scope without pulling in the firmware esp_err.h). */
static int sim_show_jpeg(const char *path)
{
    s_canvas.show_count++;
    s_canvas.state = CANVAS_BLUE;   /* memset(canvas, 0x10, ...) */

    if (!path || path[0] == '\0') {
        s_canvas.show_fail_count++;
        return -1;
    }
    if (s_canvas.force_fail_remaining > 0) {
        s_canvas.force_fail_remaining--;
        s_canvas.show_fail_count++;
        return -1;
    }
    /* tjpgd decode succeeded — canvas now has JPEG content. */
    s_canvas.state = CANVAS_JPEG;
    return 0;
}

void gallery_play_current(void)
{
    const gallery_entry_t *e = gallery_current();
    if (!e) {
        s_canvas.state = CANVAS_EMPTY;
        return;
    }
    if (e->type == GALLERY_ENTRY_JPEG) {
        sim_show_jpeg(e->jpeg_path);
        return;
    }
    /* GIF entry: show JPEG flash first if we have one, then "play"
     * the GIF (here just mark canvas as a GIF frame). */
    if (e->jpeg_path[0]) {
        sim_show_jpeg(e->jpeg_path);
    }
    s_canvas.state = CANVAS_GIF_FRAME;
}

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

/* Definition of the forward-declared s_sim_clock_ms above. */
int64_t s_sim_clock_ms = 0;

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

/* Forward declarations for power_idle / encoder-boost telemetry —
 * the real definitions live in the power-saving section further
 * down; run_encode_pipeline references them. */
bool power_idle_is_sleeping(void);
static int s_encoder_boost_count;
static int s_encoder_normal_count;

static int run_encode_pipeline(int n_cams, const char *stem)
{
    s_is_encoding = true;

    /* Drop ~6 MB PPA buffer to make PSRAM available — matches the
     * app_album_release_jpeg_decoder() call in app_gifs.c:1232. The HW
     * decoder handle stays alive (release does NOT destroy it). */
    album_decoder_release_ppa();

    /* Dual-core boost is only available while the device is sleeping
     * — that's the whole premise: Core 0 (LVGL + video_stream paused)
     * is genuinely free to help. If the user wakes mid-encode the
     * boost goes off; the model snapshots the state at the start of
     * each encode so each frame's modeled time is consistent for the
     * scenario duration. Tests can flip the device into sleep
     * BEFORE pimslo_sim_wait_idle to exercise the boost path. */
    bool boost = power_idle_is_sleeping();
    if (boost) s_encoder_boost_count++;
    else       s_encoder_normal_count++;
    /* cache_source_jpegs is unconditional in the model — once the
     * firmware moves the jpeg_data[] free to AFTER pass 2, every
     * encode benefits regardless of sleep state. */
    p4_pipeline_timing_t t = p4_timing_estimate((p4_pipeline_params_t){
        .n_cams = n_cams, .stack = encoder_stack_location(),
        .lut = encoder_lut_location(), .save_p4ms = true,
        .boost_dual_core = boost,
        .cache_source_jpegs = true,
    });

    /* Allocate encoder buffers (PSRAM). The 7 MB scaled_buf is what
     * the PPA-release was making room for — without that release, the
     * PIMSLO encoder collides with album. */
    void *scaled_buf = p4_mem_malloc(1824 * 1920 * 2, P4_POOL_PSRAM);
    void *pixel_lut  = p4_mem_malloc(65536, P4_POOL_PSRAM);

    /* Pass 2 frame churn — 4 forward frames + 2 replay frames. Each
     * frame allocates err_cur (11.5 KB), err_nxt (11.5 KB), row_cache
     * (3.8 KB), row_indices (1.9 KB) — all from INTERNAL with PSRAM
     * fallback — and frees at end of frame. We model the alloc/free
     * pair to expose the fragmentation pressure on internal_largest:
     * if alloc lands in INTERNAL and the pool can't accommodate
     * concurrent capture-task allocations, the budget catches it. */
    enum { FRAMES_PER_ENCODE = 6 };
    for (int frame = 0; frame < FRAMES_PER_ENCODE; frame++) {
        void *err_cur = p4_mem_malloc(11520, P4_POOL_INT);
        void *err_nxt = p4_mem_malloc(11520, P4_POOL_INT);
        void *row_cache   = p4_mem_malloc(3840, P4_POOL_INT);
        void *row_indices = p4_mem_malloc(1920, P4_POOL_INT);

        /* PSRAM fallback if internal can't satisfy. Mirrors the real
         * encoder's `if (!err_cur || !err_nxt) { ... heap_caps_calloc(
         * MALLOC_CAP_SPIRAM) }` pattern. */
        if (!err_cur) err_cur = p4_mem_malloc(11520, P4_POOL_PSRAM);
        if (!err_nxt) err_nxt = p4_mem_malloc(11520, P4_POOL_PSRAM);
        if (!row_indices) row_indices = p4_mem_malloc(1920, P4_POOL_PSRAM);

        if (err_cur)     p4_mem_free(err_cur);
        if (err_nxt)     p4_mem_free(err_nxt);
        if (row_cache)   p4_mem_free(row_cache);
        if (row_indices) p4_mem_free(row_indices);
    }

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
        /* Drop — capture task discards if <2 cams. Set the user-
         * visible error window so the saving overlay shows ERROR
         * instead of vanishing silently. */
        s_error_until_ms = s_sim_clock_ms + ERROR_OVERLAY_MS;
        return r;
    }
    /* Successful capture clears any pending error window. */
    s_error_until_ms = 0;

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

/* "Deferred encode in flight" state — a job has been popped off the
 * queue and the encoder task is sitting in its 2 s defer-poll loop.
 * Critical: BEFORE the firmware fix, s_is_encoding was only set after
 * exiting the defer loop, and queue_depth was 0 (job already taken).
 * format_workers_idle() couldn't see the job at all → format wiped
 * the source dir → encoder later resumed → encode failed on missing
 * dir. After the fix, we set this flag the moment the encoder
 * commits to a job, and format_workers_idle treats it as "busy". */
static bool s_deferred_in_flight = false;

void pimslo_sim_force_deferred_encode_in_flight(bool yes)
{
    s_deferred_in_flight = yes;
}

bool app_pimslo_is_encoding_or_deferred(void)
{
    return s_is_encoding || s_deferred_in_flight;
}

int pimslo_sim_wait_idle(int max_wait_ms)
{
    int waited = 0;
    while (s_encode_queue_depth > 0 && waited < max_wait_ms) {
        if (encode_should_defer()) {
            /* Encoder has committed to the head-of-queue job and is
             * polling for a safe page. The fix: claim the job now so
             * format_workers_idle can see it. */
            s_deferred_in_flight = true;
            s_sim_clock_ms += 100; waited += 100;
            continue;
        }
        s_deferred_in_flight = false;
        encode_job_t job = s_encode_queue[s_encode_queue_head];
        s_encode_queue_head = (s_encode_queue_head + 1) % ENCODE_QUEUE_DEPTH_MAX;
        int t = run_encode_pipeline(job.n_cams, job.stem);
        waited += t;
    }
    /* Only drop the in-flight flag when the queue actually drained.
     * Hitting max_wait_ms while still deferred must leave the flag
     * up — that's exactly the state format_workers_idle needs to
     * detect. */
    if (s_encode_queue_depth == 0) s_deferred_in_flight = false;
    return waited;
}

/* ========================================================================
 * SD format model (mirrors ui_extra.c::format_bg_task)
 * ======================================================================== */
static int s_deferred_jobs_lost = 0;

int pimslo_sim_deferred_jobs_lost_count(void)
{
    return s_deferred_jobs_lost;
}

format_result_t pimslo_sim_format_sd(void)
{
    /* Pre-format gate — mirrors format_workers_idle. Refuse if any
     * pipeline stage is mid-flight, OR if a deferred encoder has
     * already claimed a job (the bug path). */
    if (s_is_encoding || s_deferred_in_flight ||
        s_encode_queue_depth > 0) {
        return FORMAT_REFUSED_BUSY;
    }

    /* Wipe the simulated SD layout. Mirrors esp_vfs_fat_sdcard_format
     * + the four post-format mkdirs. */
    gallery_init();
    s_p4ms_count = s_gif_count = 0;
    s_bg_pre_render_count = s_bg_re_encode_count = 0;

    /* Post-format LVGL cleanup: scan rebuilds (now-empty) gallery and
     * refreshes the empty-overlay. Mirrors ui_extra.c:1166-1168. */
    gallery_scan();
    gallery_refresh_empty_overlay();

    return FORMAT_OK;
}

/* ========================================================================
 * Power-saving idle/sleep model
 *
 * Pure state machine — no side effects on real hardware, just counters
 * for tests + tracks last-activity timestamp against the sim clock.
 * The "encoder runs uninterrupted" property is enforced by NOT pausing
 * any encode-queue work in this state machine — the existing
 * pimslo_sim_wait_idle / run_encode_pipeline run independently of
 * power state.
 * ======================================================================== */
static power_state_t s_power_state = POWER_ACTIVE;
static int64_t       s_last_activity_clock_ms = 0;
static int64_t       s_modal_entered_clock_ms = 0;
static int           s_backlight_off_count = 0;
static int           s_backlight_on_count = 0;
static int           s_lvgl_stop_count = 0;
static int           s_lvgl_resume_count = 0;
/* s_encoder_boost_count / s_encoder_normal_count are forward-declared
 * earlier in the file (above run_encode_pipeline). They're tentative
 * definitions; this is just where they get their initialization
 * intent — actual zero-init happens in BSS. */

static void power_enter_sleep_modal(void)
{
    s_power_state = POWER_SLEEP_MODAL;
    /* Anchor to the MOMENT the idle threshold was crossed, not the
     * current sim clock — otherwise a single advance_ms() that
     * overshoots both thresholds (e.g. 180000+1500 in one call)
     * would land in MODAL with in_modal=0 and never transition to
     * SLEEPING in the same call. The threshold-crossing time is
     * `last_activity + POWER_IDLE_TIMEOUT_MS`. */
    s_modal_entered_clock_ms =
        s_last_activity_clock_ms + POWER_IDLE_TIMEOUT_MS;
}

static void power_enter_sleeping(void)
{
    s_power_state = POWER_SLEEPING;
    /* These are the side effects the firmware will perform. The mock
     * just tallies them so tests can assert. */
    s_backlight_off_count++;
    s_lvgl_stop_count++;
}

static void power_wake(void)
{
    if (s_power_state == POWER_SLEEPING) {
        s_backlight_on_count++;
        s_lvgl_resume_count++;
    }
    s_power_state = POWER_ACTIVE;
    s_last_activity_clock_ms = s_sim_clock_ms;
}

void power_idle_init(void)
{
    s_power_state = POWER_ACTIVE;
    s_last_activity_clock_ms = s_sim_clock_ms;
    s_modal_entered_clock_ms = 0;
    s_backlight_off_count = s_backlight_on_count = 0;
    s_lvgl_stop_count = s_lvgl_resume_count = 0;
    s_encoder_boost_count = s_encoder_normal_count = 0;
}

void power_idle_kick(void)
{
    s_last_activity_clock_ms = s_sim_clock_ms;
}

power_state_t power_idle_state(void)        { return s_power_state; }
bool power_idle_is_sleeping(void) {
    return s_power_state != POWER_ACTIVE;
}
bool power_idle_modal_visible(void) {
    return s_power_state == POWER_SLEEP_MODAL;
}

power_state_t power_idle_advance_ms(int ms)
{
    s_sim_clock_ms += ms;
    /* Re-evaluate transitions. The model uses absolute clock
     * comparisons so a single advance that crosses both thresholds
     * ends up in POWER_SLEEPING in the same call. */
    while (1) {
        if (s_power_state == POWER_ACTIVE) {
            int64_t idle = s_sim_clock_ms - s_last_activity_clock_ms;
            if (idle >= POWER_IDLE_TIMEOUT_MS) {
                power_enter_sleep_modal();
                continue;     /* re-evaluate; modal might also expire */
            }
            break;
        } else if (s_power_state == POWER_SLEEP_MODAL) {
            int64_t in_modal = s_sim_clock_ms - s_modal_entered_clock_ms;
            if (in_modal >= POWER_SLEEP_MODAL_MS) {
                power_enter_sleeping();
            }
            break;
        } else {
            break;
        }
    }
    return s_power_state;
}

bool power_idle_press_button(void)
{
    bool was_sleeping = (s_power_state != POWER_ACTIVE);
    if (was_sleeping) {
        power_wake();
        /* Caller must swallow the press — wake-only, no action. */
        return true;
    }
    /* Active state — just kick the timer, press propagates normally. */
    s_last_activity_clock_ms = s_sim_clock_ms;
    return false;
}

int power_backlight_off_count(void) { return s_backlight_off_count; }
int power_backlight_on_count(void)  { return s_backlight_on_count; }
int power_lvgl_stop_count(void)     { return s_lvgl_stop_count; }
int power_lvgl_resume_count(void)   { return s_lvgl_resume_count; }
int encoder_boost_encode_count(void)  { return s_encoder_boost_count; }
int encoder_normal_encode_count(void) { return s_encoder_normal_count; }

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
    memset(&s_canvas, 0, sizeof(s_canvas));
    memset(&s_vf, 0, sizeof(s_vf));
    s_vf.consumer_cached_gen = -1;
    s_error_until_ms = 0;
    memset(s_orphans, 0, sizeof(s_orphans));
    s_n_orphans = 0;
    s_orphan_cleanup_count = 0;
    /* Reset power-saving state — must happen AFTER s_sim_clock_ms is
     * zeroed so power_idle_init() seeds last_activity from clock=0. */
    power_idle_init();
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
