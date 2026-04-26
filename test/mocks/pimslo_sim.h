/**
 * @file pimslo_sim.h
 * @brief End-to-end PIMSLO pipeline simulator.
 *
 * Glues together the constrained allocator, the budget catalog, the
 * phased lifecycle simulator, and the timing model into a unified
 * "device under test" that mirrors the on-device PIMSLO subsystem
 * structure:
 *
 *   - Gallery state (entries, current_index, processing/empty overlays)
 *   - Page state (UI_PAGE_MAIN / CAMERA / GIFS / SETTINGS / etc.)
 *   - encode_should_defer() logic
 *   - Task pipeline (capture → save → encode_queue), simulated with
 *     pthreads + per-task stack-location timing
 *   - Background worker (.p4ms pre-render + stale-capture re-encode)
 *
 * Mirrors the names from `factory_demo/main/app/{Gif,Pimslo}/` so the
 * test code reads the same as the firmware. Where the host behavior
 * diverges from the device (no real LVGL, no real SD), it's documented
 * inline.
 *
 * The CRITICAL property: every memory allocation goes through the
 * constrained p4_mem_model, so an architecture decision that makes
 * heap_caps_malloc fail on-device makes it fail here too.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "p4_mem_model.h"
#include "p4_timing.h"

/* ========================================================================
 * UI page model (mirrors ui_extra.h)
 * ======================================================================== */
typedef enum {
    UI_PAGE_MAIN = 0,
    UI_PAGE_CAMERA,
    UI_PAGE_INTERVAL_CAM,
    UI_PAGE_VIDEO_MODE,
    UI_PAGE_GIFS,
    UI_PAGE_USB_DISK,
    UI_PAGE_SETTINGS,
    UI_PAGE_ALBUM,
} ui_page_t;

void ui_extra_set_current_page(ui_page_t p);
ui_page_t ui_extra_get_current_page(void);

/* ========================================================================
 * Gallery state (mirrors app_gifs.c::s_ctx)
 * ======================================================================== */
typedef enum {
    GALLERY_ENTRY_GIF,
    GALLERY_ENTRY_JPEG,  /* preview only — encode pending */
} gallery_entry_type_t;

typedef struct {
    char    stem[16];      /* P4Mxxxx */
    char    gif_path[64];  /* /sdcard/p4mslo_gifs/P4Mxxxx.gif (may be empty) */
    char    jpeg_path[64]; /* /sdcard/p4mslo_previews/P4Mxxxx.jpg (may be empty) */
    char    p4ms_path[64]; /* /sdcard/p4mslo_small/P4Mxxxx.p4ms */
    gallery_entry_type_t type;
    bool    has_gif;
    bool    has_jpeg;
    bool    has_p4ms;
} gallery_entry_t;

#define GALLERY_MAX_ENTRIES 64

void gallery_init(void);
void gallery_scan(void);          /* re-scans the simulated SD */
int  gallery_count(void);
int  gallery_current_index(void);
const gallery_entry_t *gallery_current(void);
void gallery_next(void);
void gallery_prev(void);
bool gallery_is_encoding(void);
bool gallery_is_playing(void);
bool gallery_was_ever_opened(void);
void gallery_mark_opened(void);   /* called when user enters UI_PAGE_GIFS */

/* Inserts entry by writing to the simulated SD layout. */
void gallery_record_capture(const char *stem, bool has_gif, bool has_jpeg, bool has_p4ms);

/* Simulate a leftover orphan from a prior interrupted encode: a
 * 0-byte .gif AND/OR a capture dir with 0-1 pos*.jpg files. The
 * gallery-scan cleanup logic should drop these without showing them
 * as gallery entries. */
void gallery_inject_orphan_gif(const char *stem);
void gallery_inject_orphan_capture_dir(const char *stem, int pos_files);
int  gallery_orphan_cleanup_count(void);

/* Mirrors app_gifs_delete_current: removes the current entry from the
 * list (drops the simulated .gif/.p4ms/.jpeg files), clamps current
 * index to remain valid. Called by tests that exercise the delete-
 * modal flow. */
void gallery_delete_current(void);

/* ========================================================================
 * Memory-snapshot helpers — useful in test assertions
 * ======================================================================== */
size_t pimslo_sim_internal_largest(void);
size_t pimslo_sim_psram_largest(void);

/* ========================================================================
 * Album JPEG decoder dance (mirrors app_album.c)
 *
 * Single ESP32-P4 hardware JPEG decoder — the album viewer holds it +
 * a ~6 MB PPA output buffer in PSRAM. The encoder must release the PPA
 * buffer before its own ~7 MB scaled buffer alloc, then reacquire after.
 *
 * The HW decoder handle itself is persistent (release does NOT destroy
 * it — repeated create/destroy panics with "no memory for jpeg decode
 * rxlink"). Reacquire of the PPA buffer is allowed to fail gracefully
 * if PSRAM is fragmented; the album degrades to "no full-res preview"
 * but the rest of the UI keeps working.
 *
 * The mock tracks two state bits so tests can assert the dance happens:
 *   - s_album.ppa_held : PPA buffer currently allocated
 *   - s_album.hw_decoder_alive : decoder handle still valid
 * ======================================================================== */
void   album_decoder_init(void);            /* allocates HW decoder + PPA buf */
void   album_decoder_release_ppa(void);     /* drop PPA, keep HW alive */
bool   album_decoder_reacquire_ppa(void);   /* re-alloc PPA — may fail */
bool   album_ppa_held(void);
bool   album_hw_decoder_alive(void);
size_t album_ppa_size(void);                /* 1920×1088×3 ≈ 6 MB */
int    album_release_count(void);           /* tally for assertions */
int    album_reacquire_count(void);
int    album_reacquire_fail_count(void);

/* ========================================================================
 * Camera viewfinder buffer model (mirrors app_video_stream.c)
 *
 * `scaled_camera_buf` (~4 MB), `jpg_buf` (~835 KB), and
 * `shared_photo_buf` (~1.8 MB) are allocated at boot, freed by
 * `app_video_stream_free_buffers()` on every PIMSLO photo cycle and
 * every background encode, and re-allocated at NEW addresses by
 * `app_video_stream_realloc_buffers()`. Any consumer caching these
 * pointers (app_video_photo, app_video_record) reads STALE pointers
 * after the first free+realloc — writes to freed PSRAM blocks → heap
 * corruption.
 *
 * The mock tracks: each buffer's "current address" (just an integer
 * generation counter), plus consumer-side cached pointers. After
 * `viewfinder_free_buffers` + `viewfinder_realloc_buffers`, the
 * generation increments. Tests assert that consumers refresh on
 * use, NOT cache at init.
 * ======================================================================== */
void   viewfinder_init_buffers(void);          /* allocates at "gen 1" */
void   viewfinder_free_buffers(void);          /* frees current */
void   viewfinder_realloc_buffers(void);       /* frees + allocates "gen N+1" */
int    viewfinder_buf_generation(void);        /* current gen counter */
bool   viewfinder_buffers_alive(void);         /* are they currently allocated? */

/* Consumer-side simulated cache. Set by `consumer_cache_buffers()`
 * (mirrors app_video_photo_init's caching pattern). When the consumer
 * reads its cached pointer via `consumer_use_buffers()`, the model
 * returns whether the read was valid (gen still matches). */
void   consumer_cache_buffers(void);           /* simulates caching at boot */
bool   consumer_use_buffers(void);             /* returns false if stale */
void   consumer_refresh_buffers(void);         /* the FIX — refresh before use */

/* Force the next N album reacquire calls to fail (simulates PSRAM
 * fragmentation). */
void   album_force_next_reacquire_fail(int n);

/* ========================================================================
 * Capture-error-overlay state
 *
 * When SPI capture returns 0 usable cameras, the firmware sets a
 * 3-second window during which the saving overlay is replaced with a
 * red "ERROR" pill so the user gets explicit feedback that the photo
 * failed (instead of silent disappearance). Tests can assert:
 *   - 0-cam capture → error_pending() = true for ~3 s
 *   - subsequent successful capture clears the flag immediately
 *   - error window auto-expires after timeout
 * ======================================================================== */
bool   capture_error_pending(void);
void   simulate_advance_time_ms(int ms);   /* age the mock clock */

/* ========================================================================
 * Gallery rendering model
 *
 * Mirrors `app_gifs.c::show_jpeg` and `play_current` enough that tests
 * can assert what the user actually sees. Specifically:
 *   - When entering the gallery on a JPEG-only entry, the canvas is
 *     first memset to 0x10 (which is RGB565 ~blue), then tjpgd fills
 *     it with the preview JPEG. If tjpgd fails (mutex timeout, decode
 *     error, file missing), the blue stays — that's the "blue square"
 *     bug.
 *   - This model tracks: did the canvas get filled? With what?
 *
 * Test usage:
 *     gallery_play_current();
 *     ASSERT(gallery_canvas_is_blue() == false,
 *            "JPEG-only entry must not show solid blue");
 * ======================================================================== */
typedef enum {
    CANVAS_EMPTY,        /* no entry / not played yet */
    CANVAS_BLUE,         /* memset 0x10 only (decode failed) */
    CANVAS_JPEG,         /* show_jpeg() succeeded — preview painted */
    CANVAS_GIF_FRAME,    /* GIF playback frame painted */
} canvas_state_t;

canvas_state_t gallery_canvas_state(void);
bool gallery_canvas_is_blue(void);
bool gallery_canvas_has_jpeg(void);
void gallery_play_current(void);

/* Force show_jpeg to fail on the next call (simulates mutex timeout
 * or a corrupted preview file). */
void gallery_force_next_jpeg_fail(int n);

/* Tally counters for assertions. */
int gallery_jpeg_show_count(void);
int gallery_jpeg_show_fail_count(void);

/* ========================================================================
 * Task pipeline (mirrors app_pimslo.c)
 *
 * Each task has a stack location attribute. Tasks pinned to a core
 * contend on that core's CPU; the CPU model is "two cores, each does
 * one unit of work per ms when idle, slowed by competing tasks".
 * Tasks block on queues / semaphores like the real ones.
 * ======================================================================== */
void pimslo_sim_init(void);  /* spawns capture/save/encode_queue tasks */
void pimslo_sim_shutdown(void);

/* Trigger a photo capture as if the user pressed the photo button.
 * Returns the sim-time (ms) until the capture+save completes. The
 * encode runs asynchronously on a worker; use pimslo_sim_wait_idle()
 * to block until everything finishes. */
typedef struct {
    int  capture_ms;       /* SPI capture time */
    int  save_ms;          /* fwrite of 4 pos*.jpg + preview */
    int  cams_usable;      /* 0-4 */
    char stem[16];         /* P4Mxxxx of this capture */
} pimslo_sim_capture_result_t;

pimslo_sim_capture_result_t pimslo_sim_photo_btn(int cams_to_simulate);

/* Wait for all queued saves + encodes to finish. Returns sim-time
 * elapsed. */
int pimslo_sim_wait_idle(int max_wait_ms);

/* Force-fail the next N SPI captures (sets cams_usable accordingly).
 * Models hardware flakiness for testing the encode-skip-on-too-few path. */
void pimslo_sim_force_capture_fail(int n_captures, int cams_returned);

/* ========================================================================
 * Background worker (mirrors gif_bg in app_gifs.c)
 *
 * Walks unencoded captures + un-pre-rendered .gifs. Runs only when on
 * a "safe page" (not CAMERA / INTERVAL / VIDEO_MODE). Yields every
 * ~15s after the user navigates the gallery.
 * ======================================================================== */
void bg_worker_kick(void);     /* wake the worker if it was idle */
bool bg_worker_should_yield(void);
int  bg_worker_pre_render_count(void);  /* tally of .p4ms files pre-rendered */
int  bg_worker_re_encode_count(void);   /* tally of stale captures encoded */

/* ========================================================================
 * Test scenario helpers
 * ======================================================================== */

/* Reset everything: SD, gallery, page state, allocator. Call at the
 * top of each test. */
void pimslo_sim_reset(void);

/* Configure the architecture to test. */
typedef enum {
    PIMSLO_ARCH_BASELINE,              /* original firmware: PSRAM stack, PSRAM LUT */
    PIMSLO_ARCH_PROPOSED,              /* commit 72e06bd (current shipping):
                                        * INTERNAL stack, PSRAM LUT. Encode slow
                                        * (~4 min) but SPI healthy. */
    PIMSLO_ARCH_PROPOSED_OCTREE_HPRAM, /* PROPOSED + 8 KB octree LUT in HP L2MEM.
                                        * REJECTED — same dma_int starvation
                                        * pattern as BSS_LUT, just smaller. */
    PIMSLO_ARCH_PROPOSED_OCTREE_TCM,   /* PROPOSED + 8 KB octree LUT in TCM
                                        * (0x30100000, 8 KB total, NOT DMA-
                                        * capable). The recommended next step. */
    PIMSLO_ARCH_PROPOSED_RGB444,       /* PROPOSED + 4 KB RGB444 LUT in HP L2MEM */
    PIMSLO_ARCH_PROPOSED_BSS_LUT,      /* HARDWARE-REJECTED: 64 KB BSS LUT in
                                        * HP L2MEM starves dma_int → SPI panic.
                                        * Kept in the catalog for regression
                                        * testing only. */
} pimslo_sim_arch_t;

void pimslo_sim_set_architecture(pimslo_sim_arch_t arch);
pimslo_sim_arch_t pimslo_sim_architecture(void);

/* ========================================================================
 * SD format model (mirrors ui_extra.c::format_bg_task + format_workers_idle)
 *
 * The on-device flow:
 *   1. format_workers_idle() — refuses if pimslo_is_capturing /
 *      pimslo_is_encoding / pimslo_get_queue_depth > 0.
 *   2. f_mkfs() runs (~8 s on this board).
 *   3. mkdir the four PIMSLO dirs.
 *   4. gallery_scan() + refresh_empty_overlay() to rebuild UI state.
 *
 * THE BUG bug 1 + 2 model: when an encode job is in the queue and the
 * encoder task is in its defer loop (user on UI_PAGE_CAMERA), s_encoding
 * is NOT yet true and the queue depth is 0 (job already taken). So
 * format_workers_idle() returns true even though there's a job in
 * flight. format wipes the source dir; encoder later resumes and
 * tries to encode a missing dir → fails / panics / leaves the entry
 * stuck in QUEUED forever.
 *
 * Mock APIs:
 *   pimslo_sim_format_sd()      attempts the format. Returns 0 on
 *                                success, non-zero if refused (busy)
 *                                or if a deferred-job-vs-format race
 *                                was detected.
 *   pimslo_sim_force_deferred_encode_in_flight(true)
 *                                pretends an encoder job has been
 *                                taken from the queue but is sitting
 *                                in defer. Used to test that
 *                                format_workers_idle now detects this
 *                                state.
 * ======================================================================== */
typedef enum {
    FORMAT_OK = 0,
    FORMAT_REFUSED_BUSY,           /* something in pimslo pipeline */
    FORMAT_DEFERRED_JOB_LOST,      /* legacy: format wiped a dir an
                                    * in-defer encoder still expects.
                                    * After fix, this should never
                                    * happen. */
} format_result_t;

format_result_t pimslo_sim_format_sd(void);
void pimslo_sim_force_deferred_encode_in_flight(bool yes);
int  pimslo_sim_deferred_jobs_lost_count(void);

/* ========================================================================
 * Empty-overlay state (mirrors app_gifs.c::empty_label visibility)
 *
 * The "Album empty / Take a photo" label is shown when count==0 AND
 * sd_ok. Once shown, it's only hidden by an explicit
 * refresh_empty_overlay() call; the label widget has no auto-hide
 * trigger. So if the gallery transitions empty→non-empty between two
 * page-entries and refresh_empty_overlay isn't called, the overlay
 * STAYS visible on top of the new entry's "QUEUED" badge — exactly
 * the symptom the user reported as bug 3.
 *
 * Mock tracks: is the overlay currently visible?
 * Test entry points:
 *   gallery_overlay_visible()        — current visibility flag
 *   gallery_enter()                  — simulates redirect_to_gifs_page
 *                                       (calls scan + auto-refresh
 *                                       overlay AFTER the fix)
 * ======================================================================== */
bool gallery_overlay_visible(void);
void gallery_enter(void);    /* mirrors ui_extra_redirect_to_gifs_page */
void gallery_refresh_empty_overlay(void);

/* Public hook so tests can assert format's "busy when encoder
 * deferred" detection works. Returns true while either s_is_encoding
 * or s_deferred_in_flight is set. */
bool app_pimslo_is_encoding_or_deferred(void);

/* ========================================================================
 * Power-saving idle/sleep model (mirrors ui_extra.c idle timer)
 *
 * Goal on hardware: after 3 minutes of no user input, briefly show a
 * "Going to sleep" modal, then turn off the LCD backlight AND stop
 * the LVGL refresh task. The next button press wakes the display +
 * resumes LVGL + is swallowed (not propagated as an action). Encoder
 * + capture pipeline keeps running through sleep — power saving is
 * just for display + LVGL CPU.
 *
 * Mock state machine:
 *   POWER_ACTIVE        — display on, LVGL running
 *     tick (>= timeout) → POWER_SLEEP_MODAL
 *     button_press      → kick timer, stay
 *
 *   POWER_SLEEP_MODAL   — modal shown, ~1.5 s grace before backlight
 *                         off. Encoder still running.
 *     tick (>= modal_ms)  → POWER_SLEEPING
 *     button_press        → wake (back to ACTIVE), button swallowed
 *
 *   POWER_SLEEPING      — backlight off, LVGL stopped. Encoder still
 *                         running (this is the whole point).
 *     button_press        → wake (back to ACTIVE), button swallowed
 *
 * Side-effect counters let tests assert the right calls fired:
 *   power_backlight_off_count  — bsp_display_backlight_off()
 *   power_backlight_on_count   — bsp_display_backlight_on()
 *   power_lvgl_stop_count      — lvgl_port_stop()
 *   power_lvgl_resume_count    — lvgl_port_resume()
 * ======================================================================== */
typedef enum {
    POWER_ACTIVE = 0,
    POWER_SLEEP_MODAL,
    POWER_SLEEPING,
} power_state_t;

#define POWER_IDLE_TIMEOUT_MS    (3 * 60 * 1000)
#define POWER_SLEEP_MODAL_MS     1500

void          power_idle_init(void);
void          power_idle_kick(void);             /* reset activity timer */
power_state_t power_idle_state(void);
bool          power_idle_is_sleeping(void);      /* true in MODAL or SLEEPING */
bool          power_idle_modal_visible(void);    /* true only in MODAL */

/* Advance the simulation clock by `ms` and run state-machine ticks.
 * Returns the new state. */
power_state_t power_idle_advance_ms(int ms);

/* Simulate a button press. Returns true if this press was a wake
 * (i.e. caller should swallow the action), false if normal. Always
 * resets the activity timer. */
bool          power_idle_press_button(void);

/* Side-effect counters. */
int power_backlight_off_count(void);
int power_backlight_on_count(void);
int power_lvgl_stop_count(void);
int power_lvgl_resume_count(void);
