/**
 * @file p4_mem_model.h
 * @brief P4-EYE memory-constraint model for host tests.
 *
 * Captures the actual numbers we measured on the device so that
 * architecture decisions about where buffers live can be validated
 * locally before flashing.
 *
 * Numbers from `heap_caps` serial command on a freshly-booted
 * fix/pimslo-encode-stuck firmware:
 *   dma_int  free=13191  largest=6400
 *   int      free=25527  largest=7168
 *   psram    free=8675828 largest=8650752 (≈ 8.26 MB)
 *
 * The LARGEST contiguous block is what matters for allocations of any
 * meaningful size — `dma_int largest = 6.4 KB` means a 16 KB stack
 * alloc cannot land in DMA-internal even if total free DMA-internal
 * space is 13 KB. This mock enforces that constraint: heap_caps_malloc
 * with MALLOC_CAP_INTERNAL fails (returns NULL) when the requested
 * size exceeds `largest_contiguous`, just like FreeRTOS's allocator
 * silently falls back to PSRAM when the same condition is hit on
 * device.
 *
 * Use:
 *   p4_mem_init(P4_MEM_MODEL_DEFAULT);  // default device-shape limits
 *   ... run code under heap_caps_* ...
 *   p4_mem_print_summary(stdout);
 *   p4_mem_assert_no_leaks();
 *
 * To experiment with architectures that free up budget:
 *   p4_mem_set_internal_largest(7168 + 32768);  // simulate freeing 32 KB BSS
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

typedef enum {
    P4_POOL_DMA_INT = 0,  /* DMA-capable internal RAM (HP L2MEM, shared
                           * physically with INT — internal BSS deducts
                           * from both per the budget mirror logic) */
    P4_POOL_INT     = 1,  /* general internal RAM (BSS goes here at link) */
    P4_POOL_PSRAM   = 2,
    P4_POOL_TCM     = 3,  /* Tightly-Coupled Memory at 0x30100000.
                           * 8 KB total. NOT DMA-capable, separate from
                           * HP L2MEM, so BSS placed here doesn't compete
                           * with the SPI master's DMA-internal pool.
                           * Perfect home for an octree LUT or any small
                           * hot-loop static that we want internal-RAM
                           * speed for without starving SPI. Section
                           * attribute: __attribute__((section(".tcm.bss")))
                           * (linker-script defined; pre-cleared at boot
                           * so functionally a normal BSS region). */
    P4_POOL_COUNT
} p4_pool_t;

typedef struct {
    /* Total free + largest contiguous, all in bytes.
     * Largest is the binding constraint for the alloc; total is the
     * sum-of-fragments.
     *
     * For PSRAM specifically we track up to a few free blocks because
     * the chip has multiple banks and the on-device behavior is
     * "alloc finds best-fit"; a 7 MB alloc doesn't preclude a parallel
     * 6 MB alloc from a different bank. block[0] is largest;
     * block[1..] are smaller. Internal-RAM pools just use block[0]. */
    size_t total_free;
    size_t largest_contiguous;
    size_t blocks[3];     /* additional free blocks (PSRAM uses these) */
} p4_pool_state_t;

typedef struct {
    p4_pool_state_t pool[P4_POOL_COUNT];
} p4_mem_model_t;

/* DEFAULT profile — post-boot state on the live board.
 * Matches `heap_caps` serial cmd output on the fix/pimslo-encode-stuck
 * branch. Already accounts for all current BSS reservations + ESP-IDF
 * + FreeRTOS startup overhead.
 *
 * NOTE on PSRAM total_free: the real chip has 32 MB PSRAM, but
 * heap_caps reads ~8.6 MB largest contiguous. The chip exposes PSRAM
 * via multiple banks; the 8.6 MB number is the best single block
 * after init. When viewfinder (~7 MB) allocates, ANOTHER 6+ MB block
 * is still available for album PPA — they don't compete pixel-for-
 * pixel. Our single-largest-contiguous model is too pessimistic for
 * PSRAM specifically; we set total_free to 16 MB so two big blocks
 * can coexist. Internal RAM is small enough that the
 * single-largest-contiguous model holds.
 *
 * NOTE on DMA-internal vs INTERNAL pool sharing — HARDWARE-VALIDATED:
 * the ESP32-P4's HP L2MEM is a single physical region used by BOTH
 * the regular internal heap (`MALLOC_CAP_INTERNAL`) AND the DMA-
 * capable pool (`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`). BSS items
 * placed in internal RAM at link time SUBTRACT FROM BOTH pools.
 *
 * Discovered the hard way: a 64 KB static BSS `pixel_lut` shrunk
 * `dma_int largest` from ~6.4 KB to ~1.6 KB on hardware, breaking
 * the SPI master's per-xfer priv-buffer alloc and panicking
 * mid-capture (`setup_dma_priv_buffer(1206): Failed to allocate
 * priv RX buffer` → `Load access fault`). The simulator's old
 * separate-pool model showed PROPOSED_BSS_LUT as fitting fine; it
 * doesn't.
 *
 * Mitigation in `p4_budget.c::p4_budget_simulate`: when running with
 * mode=FROM_RAW and a BSS item has `pool == P4_POOL_INT`, also
 * deduct the same size from `P4_POOL_DMA_INT`. That mirrors the
 * hardware sharing. AS_IS mode keeps using the post-boot snapshot
 * which already reflects whatever BSS is live. */
#define P4_MEM_MODEL_DEFAULT ((p4_mem_model_t){ \
    .pool[P4_POOL_DMA_INT] = { .total_free = 13191,    .largest_contiguous = 6400, .blocks = {0,0,0} }, \
    .pool[P4_POOL_INT]     = { .total_free = 25527,    .largest_contiguous = 7168, .blocks = {0,0,0} }, \
    .pool[P4_POOL_PSRAM]   = { .total_free = 17000000, .largest_contiguous = 8650752, \
                               .blocks = { 6266880, 1027072, 0 } }, \
    .pool[P4_POOL_TCM]     = { .total_free = 8192,     .largest_contiguous = 8192, .blocks = {0,0,0} }, \
})

/* RAW profile — pre-BSS, pre-ESP-IDF state. Use when experimenting
 * with "what if I add/remove N KB of BSS?": start RAW, run the catalog
 * (BSS items will drain the pools first), then runtime allocs see
 * the resulting headroom. Numbers from the linker map: DRAM segment
 * 0x4ff40000+0xeefc0 = 956 KB total, of which ~115 KB is IRAM (code +
 * vectors, lives in same segment), leaving ~841 KB for data + BSS +
 * heap. ESP-IDF + FreeRTOS startup eats ~720 KB before our app runs;
 * we model that as a fixed delta. */
#define P4_MEM_MODEL_RAW ((p4_mem_model_t){ \
    .pool[P4_POOL_DMA_INT] = { .total_free = 13191 + 4224, \
                               .largest_contiguous = 32768, .blocks = {0,0,0} }, \
    .pool[P4_POOL_INT]     = { .total_free = 25527 + 96 * 1024, \
                               .largest_contiguous = 7168 + 96 * 1024, .blocks = {0,0,0} }, \
    .pool[P4_POOL_PSRAM]   = { .total_free = 17000000, .largest_contiguous = 8650752, \
                               .blocks = { 6266880, 1027072, 0 } }, \
    .pool[P4_POOL_TCM]     = { .total_free = 8192,     .largest_contiguous = 8192, .blocks = {0,0,0} }, \
})

void p4_mem_init(p4_mem_model_t initial);
void p4_mem_set_internal_largest(size_t bytes);
void p4_mem_set_psram_largest(size_t bytes);

/* Apply a one-shot delta to a pool — useful to model "after we add
 * 32 KB of internal BSS, the heap shrinks by 32 KB". Negative values
 * shrink the pool, positive grow it. */
void p4_mem_adjust_pool(p4_pool_t pool, ptrdiff_t total_delta, ptrdiff_t largest_delta);

p4_pool_state_t p4_mem_pool_state(p4_pool_t pool);

/* Allocation tracking — every call counts; on free the count drops.
 * Used by p4_mem_assert_no_leaks() at the end of a test. */
size_t p4_mem_active_alloc_count(void);
size_t p4_mem_active_alloc_bytes(p4_pool_t pool);

/* Per-pool tally of allocations that FAILED because they exceeded
 * largest_contiguous. Architecture decisions that produce >0 fail
 * counts on the internal pools mirror real device runs where the
 * stack/buffer landed in PSRAM as a silent fallback. */
size_t p4_mem_alloc_fail_count(p4_pool_t pool);

void p4_mem_print_summary(FILE *fh);
void p4_mem_assert_no_leaks(void);

/* Test helpers used by the constrained heap_caps shim. */
void *p4_mem_malloc(size_t size, p4_pool_t pool);
void *p4_mem_calloc(size_t n, size_t size, p4_pool_t pool);
void *p4_mem_aligned_alloc(size_t align, size_t size, p4_pool_t pool);
void  p4_mem_free(void *p);
