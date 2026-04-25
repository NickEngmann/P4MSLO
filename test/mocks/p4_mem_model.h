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
    P4_POOL_DMA_INT = 0,  /* DMA-capable internal RAM */
    P4_POOL_INT     = 1,  /* general internal RAM (BSS goes here at link) */
    P4_POOL_PSRAM   = 2,
    P4_POOL_COUNT
} p4_pool_t;

typedef struct {
    /* Total free + largest contiguous, all in bytes.
     * Largest is the binding constraint for the alloc; total is the
     * sum-of-fragments. */
    size_t total_free;
    size_t largest_contiguous;
} p4_pool_state_t;

typedef struct {
    p4_pool_state_t pool[P4_POOL_COUNT];
} p4_mem_model_t;

/* DEFAULT profile — post-boot state on the live board.
 * Matches `heap_caps` serial cmd output on the fix/pimslo-encode-stuck
 * branch. Already accounts for all current BSS reservations + ESP-IDF
 * + FreeRTOS startup overhead. Use this when simulating runtime
 * behavior of the AS-IS firmware. */
#define P4_MEM_MODEL_DEFAULT ((p4_mem_model_t){ \
    .pool[P4_POOL_DMA_INT] = { .total_free = 13191, .largest_contiguous = 6400 }, \
    .pool[P4_POOL_INT]     = { .total_free = 25527, .largest_contiguous = 7168 }, \
    .pool[P4_POOL_PSRAM]   = { .total_free = 8675828, .largest_contiguous = 8650752 }, \
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
    .pool[P4_POOL_DMA_INT] = { .total_free = 13191 + 4224 /* SPI scratch+chunk */, \
                               .largest_contiguous = 32768 }, \
    .pool[P4_POOL_INT]     = { .total_free = 25527 + 96 * 1024 /* 3×32 KB BSS */, \
                               .largest_contiguous = 7168 + 96 * 1024 }, \
    .pool[P4_POOL_PSRAM]   = { .total_free = 8675828, .largest_contiguous = 8650752 }, \
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
