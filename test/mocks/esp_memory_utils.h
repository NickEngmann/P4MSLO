/**
 * @brief ESP memory utils mock
 */
#pragma once

#include <stdlib.h>
#include <string.h>

#define MALLOC_CAP_SPIRAM   (1 << 0)
#define MALLOC_CAP_DMA      (1 << 1)
#define MALLOC_CAP_DEFAULT  (1 << 2)

static inline void *heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps) {
    (void)alignment; (void)caps;
    void *ptr = calloc(n, size);
    return ptr;
}

static inline void heap_caps_free(void *ptr) {
    free(ptr);
}
