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

/* ========================================================================
 * Memory-snapshot helpers — useful in test assertions
 * ======================================================================== */
size_t pimslo_sim_internal_largest(void);
size_t pimslo_sim_psram_largest(void);

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
    PIMSLO_ARCH_BASELINE,           /* original firmware: PSRAM stack, PSRAM LUT */
    PIMSLO_ARCH_PROPOSED,           /* commit f9fad72: INTERNAL stack, PSRAM LUT (LUT
                                     * still doesn't fit internal — Pass 2 LZW slow) */
    PIMSLO_ARCH_PROPOSED_OCTREE,    /* PROPOSED + 8 KB octree LUT in internal */
    PIMSLO_ARCH_PROPOSED_RGB444,    /* PROPOSED + 4 KB RGB444 LUT in internal */
    PIMSLO_ARCH_PROPOSED_BSS_LUT,   /* PROPOSED + 64 KB pixel_lut as static BSS
                                     * (drops the gif_bg static stack to make room) */
} pimslo_sim_arch_t;

void pimslo_sim_set_architecture(pimslo_sim_arch_t arch);
pimslo_sim_arch_t pimslo_sim_architecture(void);
