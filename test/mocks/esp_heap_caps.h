/**
 * @brief ESP-IDF heap_caps mock — maps to libc malloc/free.
 *
 * The host doesn't have the ESP-IDF capability flags; we ignore them.
 * AddressSanitizer running over the host build catches the same
 * use-after-free / heap-overrun bugs that fire on-device as
 * tlsf_control_functions.h:374 panics.
 */
#pragma once

#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#define MALLOC_CAP_DEFAULT  0
#define MALLOC_CAP_SPIRAM   0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_DMA      0
#define MALLOC_CAP_8BIT     0
#define MALLOC_CAP_32BIT    0

static inline void *heap_caps_malloc(size_t size, unsigned flags) {
    (void)flags; return malloc(size);
}
static inline void *heap_caps_calloc(size_t n, size_t size, unsigned flags) {
    (void)flags; return calloc(n, size);
}
static inline void heap_caps_free(void *p) { free(p); }
static inline void *heap_caps_aligned_alloc(size_t align, size_t size, unsigned flags) {
    (void)flags;
    void *p = NULL;
    if (posix_memalign(&p, align, size) != 0) return NULL;
    return p;
}
static inline void *heap_caps_aligned_calloc(size_t align, size_t n, size_t size, unsigned flags) {
    void *p = heap_caps_aligned_alloc(align, n * size, flags);
    if (p) memset(p, 0, n * size);
    return p;
}
static inline size_t heap_caps_get_free_size(unsigned flags) { (void)flags; return SIZE_MAX; }
static inline size_t heap_caps_get_largest_free_block(unsigned flags) { (void)flags; return SIZE_MAX; }
