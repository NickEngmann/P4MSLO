/**
 * @file p4_mem_model.c
 * @brief Constrained allocator tracking the on-device memory shape.
 */
#include "p4_mem_model.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

static p4_mem_model_t s_model;
static size_t         s_active_count = 0;
static size_t         s_active_bytes[P4_POOL_COUNT] = {0};
static size_t         s_alloc_fail[P4_POOL_COUNT]   = {0};

/* Side-table mapping live user-pointer → metadata. Avoids a header
 * prefix that ASan flags as out-of-bounds when free walks backwards. */
#define MAX_LIVE_ALLOCS 256
typedef struct {
    void     *ptr;       /* NULL = slot empty */
    size_t    size;
    p4_pool_t pool;
} alloc_record_t;

static alloc_record_t s_live[MAX_LIVE_ALLOCS];

static int find_or_take_slot(void *p)
{
    for (int i = 0; i < MAX_LIVE_ALLOCS; i++) {
        if (s_live[i].ptr == NULL) return i;
    }
    return -1;
}
static int find_slot(void *p)
{
    for (int i = 0; i < MAX_LIVE_ALLOCS; i++) {
        if (s_live[i].ptr == p) return i;
    }
    return -1;
}

void p4_mem_init(p4_mem_model_t initial)
{
    /* If we still have live allocations from a previous run, free them
     * so AddressSanitizer's leak detector doesn't yell. The user-facing
     * contract is "p4_mem_init resets the model" — frees are part of
     * that. */
    for (int i = 0; i < MAX_LIVE_ALLOCS; i++) {
        if (s_live[i].ptr) { free(s_live[i].ptr); s_live[i].ptr = NULL; }
    }
    s_model = initial;
    s_active_count = 0;
    memset(s_active_bytes, 0, sizeof(s_active_bytes));
    memset(s_alloc_fail, 0, sizeof(s_alloc_fail));
}

void p4_mem_set_internal_largest(size_t bytes)
{
    s_model.pool[P4_POOL_INT].largest_contiguous = bytes;
}
void p4_mem_set_psram_largest(size_t bytes)
{
    s_model.pool[P4_POOL_PSRAM].largest_contiguous = bytes;
}
void p4_mem_adjust_pool(p4_pool_t pool, ptrdiff_t total_delta, ptrdiff_t largest_delta)
{
    if (pool >= P4_POOL_COUNT) return;
    p4_pool_state_t *p = &s_model.pool[pool];
    /* Floor at 0 so size_t doesn't wrap when a delta would push past zero.
     * In practice this fires when the proposed BSS layout exceeds the
     * RAW model's available space — meaning the BSS would land in DRAM
     * outside the heap (linker uses total DRAM, not per-pool heap),
     * but the heap allocator effectively has nothing left. */
    ptrdiff_t new_total   = (ptrdiff_t)p->total_free + total_delta;
    ptrdiff_t new_largest = (ptrdiff_t)p->largest_contiguous + largest_delta;
    p->total_free = (new_total < 0) ? 0 : (size_t)new_total;
    p->largest_contiguous = (new_largest < 0) ? 0 : (size_t)new_largest;
}

p4_pool_state_t p4_mem_pool_state(p4_pool_t pool)
{
    if (pool >= P4_POOL_COUNT) return (p4_pool_state_t){0};
    return s_model.pool[pool];
}
size_t p4_mem_active_alloc_count(void) { return s_active_count; }
size_t p4_mem_active_alloc_bytes(p4_pool_t pool) {
    return (pool < P4_POOL_COUNT) ? s_active_bytes[pool] : 0;
}
size_t p4_mem_alloc_fail_count(p4_pool_t pool) {
    return (pool < P4_POOL_COUNT) ? s_alloc_fail[pool] : 0;
}

static const char *pool_name(p4_pool_t p)
{
    switch (p) {
        case P4_POOL_DMA_INT: return "dma_int";
        case P4_POOL_INT:     return "int";
        case P4_POOL_PSRAM:   return "psram";
        case P4_POOL_TCM:     return "tcm";
        default:              return "?";
    }
}

void p4_mem_print_summary(FILE *fh)
{
    fprintf(fh, "\n--- p4_mem summary ---\n");
    fprintf(fh, "  active allocs        : %zu\n", s_active_count);
    for (int p = 0; p < P4_POOL_COUNT; p++) {
        fprintf(fh, "  %-12s active     : %zu B\n", pool_name(p), s_active_bytes[p]);
        fprintf(fh, "  %-12s alloc fails: %zu\n", pool_name(p), s_alloc_fail[p]);
        fprintf(fh, "  %-12s largest    : %zu B (model)\n",
                pool_name(p), s_model.pool[p].largest_contiguous);
    }
    fprintf(fh, "----------------------\n");
}

void p4_mem_assert_no_leaks(void)
{
    if (s_active_count != 0) {
        fprintf(stderr, "LEAK: %zu allocations still live\n", s_active_count);
        for (int p = 0; p < P4_POOL_COUNT; p++) {
            if (s_active_bytes[p] > 0) {
                fprintf(stderr, "  %s: %zu B leaked\n", pool_name(p), s_active_bytes[p]);
            }
        }
        abort();
    }
}

/* Find the smallest free block ≥ size (best-fit). Returns block
 * index 0..3 (where 0 is largest_contiguous). On no-fit, returns -1. */
static int find_block_for(p4_pool_state_t *p, size_t size)
{
    /* Build sorted block list including largest_contiguous + blocks[]. */
    size_t blks[4] = { p->largest_contiguous, p->blocks[0], p->blocks[1], p->blocks[2] };
    int best = -1;
    size_t best_sz = SIZE_MAX;
    for (int i = 0; i < 4; i++) {
        if (blks[i] >= size && blks[i] < best_sz) {
            best = i; best_sz = blks[i];
        }
    }
    return best;
}

static void shrink_pool_after_alloc(p4_pool_t pool, size_t size)
{
    p4_pool_state_t *p = &s_model.pool[pool];
    int blk = find_block_for(p, size);
    p->total_free -= size;
    if (blk == 0) {
        p->largest_contiguous -= size;
    } else if (blk > 0) {
        p->blocks[blk - 1] -= size;
    }
    /* After consuming, the largest may not be largest anymore — sort. */
    size_t pool_blks[4] = { p->largest_contiguous, p->blocks[0], p->blocks[1], p->blocks[2] };
    /* simple insertion sort, descending */
    for (int i = 1; i < 4; i++) {
        for (int j = i; j > 0 && pool_blks[j-1] < pool_blks[j]; j--) {
            size_t t = pool_blks[j]; pool_blks[j] = pool_blks[j-1]; pool_blks[j-1] = t;
        }
    }
    p->largest_contiguous = pool_blks[0];
    p->blocks[0] = pool_blks[1];
    p->blocks[1] = pool_blks[2];
    p->blocks[2] = pool_blks[3];
}

static void *do_alloc(size_t size, p4_pool_t pool, bool zeroed, size_t align)
{
    if (pool >= P4_POOL_COUNT) return NULL;
    /* Try to find any free block that can hold `size`. PSRAM may have
     * multiple sizable blocks; INT pools generally only use blocks[0]
     * which is largest_contiguous. */
    if (find_block_for(&s_model.pool[pool], size) < 0) {
        s_alloc_fail[pool]++;
        return NULL;
    }
    int slot = find_or_take_slot(NULL);
    if (slot < 0) {
        fprintf(stderr, "p4_mem: side-table full (>%d live allocs)\n", MAX_LIVE_ALLOCS);
        return NULL;
    }
    void *p = NULL;
    size_t a = (align >= 8) ? align : 8;
    if (posix_memalign(&p, a, size) != 0) {
        s_alloc_fail[pool]++;
        return NULL;
    }
    if (zeroed) memset(p, 0, size);
    s_live[slot].ptr  = p;
    s_live[slot].size = size;
    s_live[slot].pool = pool;

    s_active_count++;
    s_active_bytes[pool] += size;
    shrink_pool_after_alloc(pool, size);
    return p;
}

void *p4_mem_malloc(size_t size, p4_pool_t pool) { return do_alloc(size, pool, false, 8); }
void *p4_mem_calloc(size_t n, size_t size, p4_pool_t pool) { return do_alloc(n * size, pool, true, 8); }
void *p4_mem_aligned_alloc(size_t align, size_t size, p4_pool_t pool) { return do_alloc(size, pool, false, align); }

void p4_mem_free(void *p)
{
    if (!p) return;
    int slot = find_slot(p);
    if (slot < 0) {
        fprintf(stderr, "p4_mem_free: untracked pointer %p (double-free?)\n", p);
        abort();
    }
    p4_pool_t pool = s_live[slot].pool;
    size_t    size = s_live[slot].size;
    if (s_active_bytes[pool] >= size) s_active_bytes[pool] -= size;
    s_active_count--;
    s_model.pool[pool].total_free += size;
    /* Largest-contiguous: grow back by `size` (conservative — real
     * heap may coalesce or not). For PSRAM, also consider promoting
     * a smaller block to "largest" if the freed bytes refill it past
     * the current largest. */
    s_model.pool[pool].largest_contiguous += size;
    /* Re-sort blocks. */
    {
        p4_pool_state_t *pp = &s_model.pool[pool];
        size_t b[4] = { pp->largest_contiguous, pp->blocks[0], pp->blocks[1], pp->blocks[2] };
        for (int i = 1; i < 4; i++)
            for (int j = i; j > 0 && b[j-1] < b[j]; j--) {
                size_t t = b[j]; b[j] = b[j-1]; b[j-1] = t;
            }
        pp->largest_contiguous = b[0];
        pp->blocks[0] = b[1]; pp->blocks[1] = b[2]; pp->blocks[2] = b[3];
    }

    free(s_live[slot].ptr);
    s_live[slot].ptr = NULL;
}
