/**
 * @brief Tests for AI detection buffer management
 *
 * Tests buffer allocation, circular indexing, initialization state,
 * and cleanup for the AI detection pipeline.
 */

#include "unity/unity.h"
#include "mocks/esp_err.h"
#include "mocks/esp_log.h"
#include "mocks/esp_memory_utils.h"
#include "mocks/sdkconfig.h"

#include <stdlib.h>
#include <string.h>

/* ---- Replicate AI buffer logic from app_ai_detect.cpp ---- */
#define DETECT_HEIGHT    240
#define DETECT_WIDTH     240
#define AI_BUFFER_COUNT  5
#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

typedef struct {
    void *ai_buffers[AI_BUFFER_COUNT];
    size_t ai_buffer_size;
    int current_ai_buffer_index;
    int ai_buffers_initialized;  /* use int for C compat */
} ai_detection_buffers_t;

static ai_detection_buffers_t test_bufs;

static void reset_buffers(void) {
    memset(&test_bufs, 0, sizeof(test_bufs));
}

static esp_err_t init_buffers(size_t cache_line_size) {
    test_bufs.ai_buffer_size = ALIGN_UP(BSP_LCD_H_RES * BSP_LCD_V_RES * 2, cache_line_size);
    for (int i = 0; i < AI_BUFFER_COUNT; i++) {
        test_bufs.ai_buffers[i] = heap_caps_aligned_calloc(
            cache_line_size, 1, test_bufs.ai_buffer_size, MALLOC_CAP_SPIRAM);
        if (!test_bufs.ai_buffers[i]) {
            for (int j = 0; j < i; j++) {
                heap_caps_free(test_bufs.ai_buffers[j]);
                test_bufs.ai_buffers[j] = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
    }
    test_bufs.ai_buffers_initialized = 1;
    test_bufs.current_ai_buffer_index = 0;
    return ESP_OK;
}

static void deinit_buffers(void) {
    for (int i = 0; i < AI_BUFFER_COUNT; i++) {
        if (test_bufs.ai_buffers[i]) {
            heap_caps_free(test_bufs.ai_buffers[i]);
            test_bufs.ai_buffers[i] = NULL;
        }
    }
    test_bufs.ai_buffers_initialized = 0;
}

/* ---- Tests ---- */
void test_buffer_init(void) {
    reset_buffers();
    esp_err_t ret = init_buffers(64);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(test_bufs.ai_buffers_initialized);
    TEST_ASSERT_EQUAL(0, test_bufs.current_ai_buffer_index);
    for (int i = 0; i < AI_BUFFER_COUNT; i++) {
        TEST_ASSERT_NOT_NULL(test_bufs.ai_buffers[i]);
    }
    deinit_buffers();
}

void test_buffer_size_alignment(void) {
    reset_buffers();
    init_buffers(64);
    /* 240 * 240 * 2 = 115200, aligned to 64 → 115200 (already aligned) */
    size_t expected = ALIGN_UP(240 * 240 * 2, 64);
    TEST_ASSERT_EQUAL(expected, test_bufs.ai_buffer_size);
    TEST_ASSERT_EQUAL(0, test_bufs.ai_buffer_size % 64);
    deinit_buffers();
}

void test_buffer_circular_index(void) {
    reset_buffers();
    init_buffers(64);

    for (int cycle = 0; cycle < 3; cycle++) {
        for (int i = 0; i < AI_BUFFER_COUNT; i++) {
            int expected_idx = i;
            TEST_ASSERT_EQUAL(expected_idx, test_bufs.current_ai_buffer_index);
            test_bufs.current_ai_buffer_index =
                (test_bufs.current_ai_buffer_index + 1) % AI_BUFFER_COUNT;
        }
        TEST_ASSERT_EQUAL(0, test_bufs.current_ai_buffer_index);
    }
    deinit_buffers();
}

void test_buffer_content_isolation(void) {
    reset_buffers();
    init_buffers(64);

    /* Write distinct pattern to each buffer */
    for (int i = 0; i < AI_BUFFER_COUNT; i++) {
        memset(test_bufs.ai_buffers[i], i + 1, 64);
    }

    /* Verify patterns are isolated */
    for (int i = 0; i < AI_BUFFER_COUNT; i++) {
        uint8_t *buf = (uint8_t *)test_bufs.ai_buffers[i];
        for (int j = 0; j < 64; j++) {
            TEST_ASSERT_EQUAL_UINT8(i + 1, buf[j]);
        }
    }
    deinit_buffers();
}

void test_buffer_deinit(void) {
    reset_buffers();
    init_buffers(64);
    TEST_ASSERT_TRUE(test_bufs.ai_buffers_initialized);

    deinit_buffers();
    TEST_ASSERT_FALSE(test_bufs.ai_buffers_initialized);
    for (int i = 0; i < AI_BUFFER_COUNT; i++) {
        TEST_ASSERT_NULL(test_bufs.ai_buffers[i]);
    }
}

void test_buffer_double_deinit(void) {
    reset_buffers();
    init_buffers(64);
    deinit_buffers();
    /* Second deinit should be safe */
    deinit_buffers();
    TEST_ASSERT_FALSE(test_bufs.ai_buffers_initialized);
}

void test_buffer_reinit(void) {
    reset_buffers();
    init_buffers(64);
    deinit_buffers();

    /* Re-init should work cleanly */
    esp_err_t ret = init_buffers(128);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(test_bufs.ai_buffers_initialized);
    TEST_ASSERT_EQUAL(0, test_bufs.ai_buffer_size % 128);
    deinit_buffers();
}

void test_align_up_macro(void) {
    TEST_ASSERT_EQUAL(64, ALIGN_UP(1, 64));
    TEST_ASSERT_EQUAL(64, ALIGN_UP(64, 64));
    TEST_ASSERT_EQUAL(128, ALIGN_UP(65, 64));
    TEST_ASSERT_EQUAL(256, ALIGN_UP(200, 128));
    TEST_ASSERT_EQUAL(0, ALIGN_UP(0, 64));
}

/* ---- Main ---- */
int main(void) {
    printf("\n===== AI Buffer Tests =====\n");
    UNITY_BEGIN();

    RUN_TEST(test_buffer_init);
    RUN_TEST(test_buffer_size_alignment);
    RUN_TEST(test_buffer_circular_index);
    RUN_TEST(test_buffer_content_isolation);
    RUN_TEST(test_buffer_deinit);
    RUN_TEST(test_buffer_double_deinit);
    RUN_TEST(test_buffer_reinit);
    RUN_TEST(test_align_up_macro);

    UNITY_END();
}
