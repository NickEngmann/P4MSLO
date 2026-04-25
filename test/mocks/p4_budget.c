/**
 * @file p4_budget.c
 * @brief Per-component memory footprint catalog + simulator.
 */
#include "p4_budget.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * BASELINE CATALOG — measured numbers for every sized allocation we know
 * about on fix/pimslo-encode-stuck.
 *
 * Sources cited in `note`:
 *   - LM = factory_demo/build/factory_demo.map (linker map)
 *   - HC = `heap_caps` serial cmd output during a run
 *   - CM = CLAUDE.md "Known Issues"
 *   - SRC = source-code constant (e.g. STACK_DEPTH macro)
 * ============================================================================ */

const p4_component_t P4_BUDGET_BASELINE[] = {

    /* ---- BSS-resident in internal DRAM (LM) ---- */
    { .name = "tjpgd workspace (app_gifs.c::s_tjpgd_work)",
      .pool = P4_POOL_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_BSS, .size_bytes = 32768,
      .note = "LM .bss.s_tjpgd_work 0x8000. Required INTERNAL — "
              "tjpgd hot-loop tables. Comment in app_gifs.c says PSRAM "
              "version turns 2s decode into 30s decode." },

    { .name = "tjpgd workspace (gif_encoder.c::tjwork)",
      .pool = P4_POOL_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_BSS, .size_bytes = 32768,
      .note = "LM .bss.tjwork.1 0x8000. CANDIDATE for sharing with "
              "s_tjpgd_work — never used concurrently within an encode "
              "pipeline. Saves 32 KB if dropped." },

    { .name = "stdio fwrite buffer (gif_encoder.c::file_buf)",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_BSS, .size_bytes = 32768,
      .note = "LM .bss.file_buf.0 0x8000. Used by setvbuf for the GIF "
              "output FILE*. Pure SD-bound buffering — could be PSRAM "
              "with negligible perf cost (SD throughput is ~250 KB/s, "
              "PSRAM read is way faster). Saves 32 KB internal if moved." },

    /* ---- LVGL (CM + SRC) ---- */
    { .name = "LVGL canvas buffer (BSP_LCD_H_RES * BSP_LCD_V_RES * 2)",
      .pool = P4_POOL_PSRAM, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PERMANENT, .size_bytes = 240 * 240 * 2,
      .note = "LVGL draw buffer for the gallery canvas. PSRAM via "
              "buff_spiram=true in esp_lvgl_port config." },

    { .name = "LVGL DMA staging buffer (esp_lvgl_port trans_size)",
      .pool = P4_POOL_DMA_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PERMANENT, .size_bytes = 240 * 4,
      .note = "CM 'LCD SPI priv TX buffer OOM' fix: a permanent "
              "1920-byte DMA-internal staging buffer is allocated at "
              "init via lvgl_port_display_cfg_t.trans_size = "
              "BSP_LCD_H_RES * 4. LVGL memcpys PSRAM→internal through "
              "this buffer before each LCD flush." },

    /* ---- SPI camera (CM) ---- */
    { .name = "SPI camera chunk RX buffer",
      .pool = P4_POOL_DMA_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PERMANENT, .size_bytes = 4096,
      .note = "spi_camera.c::s_chunk_rx, eagerly allocated at "
              "spi_camera_init while the DMA-internal pool is fresh. "
              "Permanent. Required INTERNAL for SPI master DMA." },

    { .name = "SPI camera scratch TX",
      .pool = P4_POOL_DMA_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PERMANENT, .size_bytes = 64,
      .note = "spi_camera.c::s_scratch_tx, 64-byte aligned for "
              "control transactions to bypass the per-xfer priv-buffer "
              "alloc that fragments the pool and panics mid-capture." },

    { .name = "SPI camera scratch RX",
      .pool = P4_POOL_DMA_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PERMANENT, .size_bytes = 64,
      .note = "Same rationale as scratch_tx, RX direction." },

    /* ---- FreeRTOS task stacks (SRC) ----
     *
     * These are allocated by xTaskCreatePinnedToCore at init. The
     * default allocator tries internal first, falls back to PSRAM
     * silently when largest-free-contiguous can't satisfy the size.
     * `psram_fallback_ok` is true here — but the simulator will
     * report each fallback as a WARN so we can see when our budget
     * is forcing a slow PSRAM-stack task. */
    { .name = "task: pimslo_cap (8 KB stack)",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_TASK_LIFE, .size_bytes = 8192,
      .note = "Capture task on Core 0. SPI I/O bound, PSRAM stack "
              "is OK perf-wise." },

    { .name = "task: pimslo_save (6 KB stack, explicit PSRAM)",
      .pool = P4_POOL_PSRAM, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PER_TASK_LIFE, .size_bytes = 6144,
      .note = "xTaskCreatePinnedToCoreWithCaps(MALLOC_CAP_SPIRAM). "
              "Explicit PSRAM since the LCD priv-TX path needs the "
              "DMA-internal headroom this 6 KB would otherwise eat." },

    { .name = "task: pimslo_gif (16 KB stack — encoder hot path)",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_TASK_LIFE, .size_bytes = 16384,
      .note = "Currently `xTaskCreatePinnedToCore` (no caps) which "
              "falls back to PSRAM whenever 16 KB doesn't fit "
              "internal — and on this board it never does. PSRAM "
              "stack costs ~5× perf on the encoder hot loop. Goal of "
              "this work: get this stack into INTERNAL." },

    { .name = "task: gif_bg (16 KB stack)",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_TASK_LIFE, .size_bytes = 16384,
      .note = "Background pre-render / encode task. Same situation "
              "as pimslo_gif." },

    { .name = "task: serial_cmd (16 KB stack)",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_TASK_LIFE, .size_bytes = 16384,
      .note = "CM '-O2 widening… serial_cmd → 16 KB'." },

    { .name = "task: video_stream (8 KB stack)",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_TASK_LIFE, .size_bytes = 8192,
      .note = "CM 'Video-stream task stack is 4 KB' — actually bumped "
              "to 8 KB after -O2 stack pressure investigation." },

    { .name = "task: LVGL (8 KB stack)",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_TASK_LIFE, .size_bytes = 8192,
      .note = "CM 'LVGL task stack is 8 KB' — set in esp32_p4_eye.c." },

    /* ---- Camera viewfinder (CM) ---- */
    { .name = "camera viewfinder scaled_buf (~7 MB)",
      .pool = P4_POOL_PSRAM, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PERMANENT, .size_bytes = 7 * 1024 * 1024,
      .note = "app_video_stream's frame buffer for the viewfinder. "
              "Free'd before encode (encode pipeline needs the same "
              "7 MB for its scaled_buf) and re-alloc'd on camera-page "
              "entry." },

    /* ---- GIF encoder runtime allocations (per encode) ---- */
    { .name = "encoder pixel_lut (RGB565→palette index)",
      .pool = P4_POOL_PSRAM, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PER_ENCODE, .size_bytes = 65536,
      .note = "gif_encoder.c::pixel_lut. 64 KB hot-loop LUT — tries "
              "INTERNAL with PSRAM fallback in the actual code, but "
              "INTERNAL never has room so it always lands in PSRAM. "
              "Pass 2 hot loop hits this once per pixel = 3.5 M × "
              "PSRAM read per frame ≈ ~1 s/frame just for this." },

    { .name = "encoder scaled_buf (1824×1920 RGB565 = 7 MB)",
      .pool = P4_POOL_PSRAM, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PER_ENCODE, .size_bytes = 1824 * 1920 * 2,
      .note = "gif_encoder.c::scaled_buf. The decoded frame, fed to "
              "the dither+LZW pass." },

    { .name = "encoder error buffer cur (Floyd-Steinberg)",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_FRAME, .size_bytes = 1920 * 3 * 2,
      .note = "gif_encoder.c err_cur — 12 RMW ops per pixel land here. "
              "PSRAM fallback adds ~1.5-2 µs/pixel = ~7 s/frame to "
              "Pass 2. ~11.5 KB doesn't fit in INTERNAL on this board." },

    { .name = "encoder error buffer nxt",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_FRAME, .size_bytes = 1920 * 3 * 2,
      .note = "Pair with err_cur, swapped per row." },

    { .name = "encoder row_cache (per-row pixel prefetch)",
      .pool = P4_POOL_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PER_FRAME, .size_bytes = 1920 * 2,
      .note = "Optional per-row prefetch from PSRAM to internal. If "
              "this fails, the loop reads pixels straight from PSRAM "
              "scaled_buf each time — adds another ~3 s/frame. ~3.8 KB "
              "DOES usually fit in INTERNAL after boot." },

    /* ---- Album HW JPEG decoder (CM) ---- */
    { .name = "album HW JPEG decoder PPA buffer (~6 MB)",
      .pool = P4_POOL_PSRAM, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PERMANENT, .size_bytes = 1920 * 1088 * 3,
      .note = "app_album.c::album_ctx.ppa_buffer. Released before "
              "encode (frees PSRAM headroom for encoder scaled_buf), "
              "reacquired after." },
};

const size_t P4_BUDGET_BASELINE_COUNT =
    sizeof(P4_BUDGET_BASELINE) / sizeof(P4_BUDGET_BASELINE[0]);

/* ============================================================================
 * PROPOSED CATALOG — what the architecture should look like after the
 * surgical changes documented in the header. Compare against BASELINE
 * to predict whether the tjpgd-share + PSRAM-file_buf + static-stack
 * refactor will land cleanly.
 * ============================================================================ */

const p4_component_t P4_BUDGET_PROPOSED[] = {

    /* CHANGED: only one tjpgd workspace. gif_encoder.c::tjwork is
     * dropped; gif_encoder uses s_tjpgd_work via mutex. */
    { .name = "tjpgd workspace (shared, app_gifs.c::s_tjpgd_work)",
      .pool = P4_POOL_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_BSS, .size_bytes = 32768,
      .note = "Shared between show_jpeg, decode_jpeg_crop_to_canvas, "
              "and gif_encoder.c::decode_and_scale_jpeg. Mutex "
              "serializes; never used concurrently inside one encode." },

    /* CHANGED: file_buf moved to PSRAM via EXT_RAM_BSS_ATTR. */
    { .name = "stdio fwrite buffer (PSRAM, EXT_RAM_BSS_ATTR)",
      .pool = P4_POOL_PSRAM, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_BSS, .size_bytes = 32768,
      .note = "Pure SD-bound; PSRAM access is faster than SD throughput "
              "so no perf hit. Frees 32 KB internal BSS." },

    /* NEW: static BSS-resident stack for pimslo_gif task. */
    { .name = "task: pimslo_gif STATIC BSS STACK (16 KB)",
      .pool = P4_POOL_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_BSS, .size_bytes = 16384,
      .note = "xTaskCreateStaticPinnedToCore. BSS-resident → linker "
              "places it in DRAM at fixed address. Recovers ~95 s "
              "encode timing (vs ~5-7 min with PSRAM stack)." },

    /* NEW: static BSS-resident stack for gif_bg (background worker). */
    { .name = "task: gif_bg STATIC BSS STACK (16 KB)",
      .pool = P4_POOL_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_BSS, .size_bytes = 16384,
      .note = "Same xTaskCreateStaticPinnedToCore treatment — gif_bg "
              "calls app_gifs_encode_pimslo_from_dir, same hot loop. "
              "Single instance, never deleted, so static reuse is "
              "trivially safe." },

    /* TCB headers for the two static tasks (sizeof StaticTask_t ≈ 350 B
     * on RV32, two of them ≈ 0x160 from the .map). */
    { .name = "tcb: pimslo_gif + gif_bg StaticTask_t",
      .pool = P4_POOL_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_BSS, .size_bytes = 700,
      .note = "Two StaticTask_t headers — small, but counted to keep "
              "the BSS arithmetic honest." },

    /* CANDIDATE NEXT STEP: 64 KB pixel_lut as static BSS. This is the
     * fastest-encode option but consumes a chunky 64 KB of internal
     * DRAM that the heap will lose. Test the full PROPOSED catalog
     * with this enabled to verify the heap can still fit a fallback
     * 16 KB stack (gif_bg if we kept it heap-alloc'd) plus LCD/CDC
     * scratch. */
    { .name = "encoder pixel_lut STATIC BSS (next step)",
      .pool = P4_POOL_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_BSS, .size_bytes = 65536,
      .note = "Replaces the per-encode PSRAM heap_caps_malloc(65536). "
              "Pass 2 LZW becomes ~14 s/frame instead of ~55 s/frame "
              "(LUT reads on internal DRAM are ~10× faster than PSRAM). "
              "Cost: -64 KB internal heap. Mitigation: drop gif_bg's "
              "static stack (heap-alloc'd 16 KB falls back to PSRAM, "
              "bg encodes go back to ~5 min — acceptable since they're "
              "background)." },

    /* The rest is unchanged from BASELINE — copy-pasted minus the
     * three CHANGED entries above. */

    { .name = "LVGL canvas buffer",
      .pool = P4_POOL_PSRAM, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PERMANENT, .size_bytes = 240 * 240 * 2 },

    { .name = "LVGL DMA staging buffer",
      .pool = P4_POOL_DMA_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PERMANENT, .size_bytes = 240 * 4 },

    { .name = "SPI camera chunk RX buffer",
      .pool = P4_POOL_DMA_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PERMANENT, .size_bytes = 4096 },

    { .name = "SPI camera scratch TX",
      .pool = P4_POOL_DMA_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PERMANENT, .size_bytes = 64 },

    { .name = "SPI camera scratch RX",
      .pool = P4_POOL_DMA_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PERMANENT, .size_bytes = 64 },

    { .name = "task: pimslo_cap (8 KB stack)",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_TASK_LIFE, .size_bytes = 8192 },

    { .name = "task: pimslo_save (PSRAM 6 KB stack)",
      .pool = P4_POOL_PSRAM, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PER_TASK_LIFE, .size_bytes = 6144 },

    /* NOTE: pimslo_gif stack is now STATIC BSS (above), not heap-alloc.
     * Keep the entry out of the runtime task list so we don't double-count. */

    /* gif_bg stack is now STATIC BSS (above) too — keep it out of the
     * runtime task list so we don't double-count. */

    { .name = "task: serial_cmd (16 KB stack)",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_TASK_LIFE, .size_bytes = 16384 },

    { .name = "task: video_stream (8 KB stack)",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_TASK_LIFE, .size_bytes = 8192 },

    { .name = "task: LVGL (8 KB stack)",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_TASK_LIFE, .size_bytes = 8192 },

    { .name = "camera viewfinder scaled_buf",
      .pool = P4_POOL_PSRAM, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PERMANENT, .size_bytes = 7 * 1024 * 1024 },

    { .name = "encoder pixel_lut",
      .pool = P4_POOL_PSRAM, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PER_ENCODE, .size_bytes = 65536 },

    /* NOTE: encoder scaled_buf and album PPA buffer are NOT modeled
     * here — they're mutually exclusive with viewfinder via explicit
     * free/alloc in app_gifs_encode_pimslo_from_dir. The "encode
     * pipeline" simulator path covers their lifecycle separately. */

    { .name = "encoder error buffer cur",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_FRAME, .size_bytes = 1920 * 3 * 2 },

    { .name = "encoder error buffer nxt",
      .pool = P4_POOL_INT, .psram_fallback_ok = true,
      .lifetime = P4_LIFETIME_PER_FRAME, .size_bytes = 1920 * 3 * 2 },

    { .name = "encoder row_cache",
      .pool = P4_POOL_INT, .psram_fallback_ok = false,
      .lifetime = P4_LIFETIME_PER_FRAME, .size_bytes = 1920 * 2 },
};

const size_t P4_BUDGET_PROPOSED_COUNT =
    sizeof(P4_BUDGET_PROPOSED) / sizeof(P4_BUDGET_PROPOSED[0]);

/* ============================================================================
 * Simulator
 * ============================================================================ */

static const char *pool_name(p4_pool_t p)
{
    switch (p) {
        case P4_POOL_DMA_INT: return "dma_int";
        case P4_POOL_INT:     return "int";
        case P4_POOL_PSRAM:   return "psram";
        default:              return "?";
    }
}

static const char *lifetime_name(p4_lifetime_t l)
{
    switch (l) {
        case P4_LIFETIME_BSS:           return "BSS";
        case P4_LIFETIME_PERMANENT:     return "permanent";
        case P4_LIFETIME_PER_ENCODE:    return "per-encode";
        case P4_LIFETIME_PER_FRAME:     return "per-frame";
        case P4_LIFETIME_PER_TASK_LIFE: return "task-stack";
        default:                        return "?";
    }
}

int p4_budget_simulate(const p4_component_t *items, size_t n,
                        p4_budget_mode_t mode, FILE *out)
{
    int fallbacks = 0;
    int hard_fail = 0;
    /* Phase 1: BSS handling. Two modes:
     *   AS_IS: BSS is already baked into the starting model state
     *          (P4_MEM_MODEL_DEFAULT). Items just print as labels.
     *   FROM_RAW: BSS deducts from pool size — simulates "we are
     *          building the firmware fresh, here's where BSS lands."
     * Either way, BSS allocations don't fail individually because
     * they're placed at link time, not runtime. */
    size_t bss_total[P4_POOL_COUNT] = {0};
    for (size_t i = 0; i < n; i++) {
        const p4_component_t *c = &items[i];
        if (c->lifetime == P4_LIFETIME_BSS) {
            bss_total[c->pool] += c->size_bytes;
            if (mode == P4_BUDGET_MODE_FROM_RAW) {
                p4_mem_adjust_pool(c->pool, -(ptrdiff_t)c->size_bytes,
                                   -(ptrdiff_t)c->size_bytes);
                /* HARDWARE-VALIDATED: HP L2MEM is one physical region;
                 * the dma_int heap is a CAP filter on the same memory
                 * INTERNAL BSS lands in. INTERNAL BSS reduces dma_int
                 * too, but at a weighted rate — DMA-eligible banks are
                 * a subset of HP L2MEM, so only items that are big
                 * enough to span DMA-eligible regions hurt dma_int.
                 * Empirically: 32 KB BSS items (3 × in 8bb11b7) coexist
                 * with a healthy dma_int pool, but a single 64 KB BSS
                 * (the BSS pixel_lut experiment) collapses dma_int from
                 * 6.4 KB → 1.6 KB. Model that as "BSS over a 32 KB
                 * threshold deducts the EXCESS from dma_int." */
                if (c->pool == P4_POOL_INT && c->size_bytes > 32 * 1024) {
                    ptrdiff_t excess = (ptrdiff_t)c->size_bytes - 32 * 1024;
                    p4_mem_adjust_pool(P4_POOL_DMA_INT, -excess, -excess);
                }
            }
        }
    }

    if (out) fprintf(out, "\n=== Memory budget simulation ===\n");
    if (out) fprintf(out, "  BSS total: dma_int=%zu B  int=%zu B  psram=%zu B\n",
                     bss_total[P4_POOL_DMA_INT], bss_total[P4_POOL_INT],
                     bss_total[P4_POOL_PSRAM]);
    if (out) fprintf(out, "  init  dma_int_largest = %zu B  int_largest = %zu B  "
                          "psram_largest = %zu B\n",
                     p4_mem_pool_state(P4_POOL_DMA_INT).largest_contiguous,
                     p4_mem_pool_state(P4_POOL_INT).largest_contiguous,
                     p4_mem_pool_state(P4_POOL_PSRAM).largest_contiguous);

    for (size_t i = 0; i < n; i++) {
        const p4_component_t *c = &items[i];

        if (c->lifetime == P4_LIFETIME_BSS) {
            /* Already accounted for above — print confirmation. */
            if (out) fprintf(out, "  bss   [%-9s] %-50s %7zu B\n",
                             pool_name(c->pool), c->name, c->size_bytes);
            continue;
        }

        /* Try preferred pool first. */
        void *p = p4_mem_malloc(c->size_bytes, c->pool);
        if (p) {
            if (out) fprintf(out, "  ok    [%-9s %-9s] %-50s %7zu B\n",
                             pool_name(c->pool), lifetime_name(c->lifetime),
                             c->name, c->size_bytes);
            continue;
        }

        if (c->psram_fallback_ok && c->pool != P4_POOL_PSRAM) {
            /* Fall back to PSRAM — same behavior as FreeRTOS allocator on device. */
            p = p4_mem_malloc(c->size_bytes, P4_POOL_PSRAM);
            if (p) {
                fallbacks++;
                if (out) fprintf(out, "  WARN  [psram→fallback] %-50s %7zu B (wanted %s)\n",
                                 c->name, c->size_bytes, pool_name(c->pool));
                continue;
            }
        }

        hard_fail++;
        if (out) fprintf(out, "  FAIL  [%-9s %-9s] %-50s %7zu B (no room)\n",
                         pool_name(c->pool), lifetime_name(c->lifetime),
                         c->name, c->size_bytes);
    }

    if (out) fprintf(out, "  done  dma_int_largest = %zu B  int_largest = %zu B  "
                          "psram_largest = %zu B\n",
                     p4_mem_pool_state(P4_POOL_DMA_INT).largest_contiguous,
                     p4_mem_pool_state(P4_POOL_INT).largest_contiguous,
                     p4_mem_pool_state(P4_POOL_PSRAM).largest_contiguous);
    if (out) fprintf(out, "  fallbacks=%d  hard_fail=%d\n",
                     fallbacks, hard_fail);

    if (hard_fail > 0) return -hard_fail;
    return fallbacks;
}

int p4_budget_simulate_baseline(FILE *out)
{
    p4_mem_init(P4_MEM_MODEL_DEFAULT);
    return p4_budget_simulate(P4_BUDGET_BASELINE, P4_BUDGET_BASELINE_COUNT,
                              P4_BUDGET_MODE_AS_IS, out);
}
