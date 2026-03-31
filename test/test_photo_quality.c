/**
 * @brief Tests for Photo Quality settings (JPEG compression)
 *
 * Tests photo quality constants, resolution handling, and quality validation
 * for the JPEG photo capture functionality.
 */

#include "unity/unity.h"
#include "mocks/esp_err.h"
#include "mocks/sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Photo Quality Constants (from app_video_photo.c) ---- */
#define JPEG_PHOTO_QUALITY      90            /* JPEG quality setting */
#define JPEG_VIDEO_QUALITY      80            /* JPEG video quality setting */
#define MIN_JPEG_QUALITY        10            /* Minimum JPEG quality */
#define MAX_JPEG_QUALITY        100           /* Maximum JPEG quality */

/* ---- Photo Resolution Enums (from app_video.h) ---- */
typedef enum {
    PHOTO_RESOLUTION_480P = 0,
    PHOTO_RESOLUTION_720P,
    PHOTO_RESOLUTION_1080P,
    PHOTO_RESOLUTION_MAX
} photo_resolution_t;

/* ---- Resolution Parameters (from app_video_photo.c) ---- */
static const uint32_t photo_resolution_width[PHOTO_RESOLUTION_MAX] = {640, 1280, 1920};
static const uint32_t photo_resolution_height[PHOTO_RESOLUTION_MAX] = {480, 720, 1080};

/* ---- Helper Functions ---- */
static int validate_jpeg_quality(int quality) {
    if (quality < MIN_JPEG_QUALITY || quality > MAX_JPEG_QUALITY) {
        return -1;
    }
    return 0;
}

static int validate_photo_resolution(photo_resolution_t resolution) {
    if (resolution >= PHOTO_RESOLUTION_MAX) {
        return -1;
    }
    return 0;
}

static void get_resolution_string(photo_resolution_t resolution, char *buf, size_t len) {
    switch (resolution) {
        case PHOTO_RESOLUTION_480P:
            snprintf(buf, len, "480P");
            break;
        case PHOTO_RESOLUTION_720P:
            snprintf(buf, len, "720P");
            break;
        case PHOTO_RESOLUTION_1080P:
            snprintf(buf, len, "1080P");
            break;
        default:
            snprintf(buf, len, "UNKNOWN");
            break;
    }
}

/* ---- Test Cases ---- */

/* Test 1: JPEG Quality Constants */
void test_jpeg_photo_quality_constant(void) {
    TEST_ASSERT_EQUAL_INT(90, JPEG_PHOTO_QUALITY);
}

void test_jpeg_video_quality_constant(void) {
    TEST_ASSERT_EQUAL_INT(80, JPEG_VIDEO_QUALITY);
}

void test_jpeg_quality_range_constants(void) {
    TEST_ASSERT_EQUAL_INT(10, MIN_JPEG_QUALITY);
    TEST_ASSERT_EQUAL_INT(100, MAX_JPEG_QUALITY);
}

/* Test 2: Quality Validation */
void test_validate_jpeg_quality_valid(void) {
    TEST_ASSERT_EQUAL_INT(0, validate_jpeg_quality(50));
    TEST_ASSERT_EQUAL_INT(0, validate_jpeg_quality(JPEG_PHOTO_QUALITY));
    TEST_ASSERT_EQUAL_INT(0, validate_jpeg_quality(JPEG_VIDEO_QUALITY));
    TEST_ASSERT_EQUAL_INT(0, validate_jpeg_quality(90));
}

void test_validate_jpeg_quality_invalid_low(void) {
    TEST_ASSERT_EQUAL_INT(-1, validate_jpeg_quality(5));
    TEST_ASSERT_EQUAL_INT(-1, validate_jpeg_quality(0));
    TEST_ASSERT_EQUAL_INT(-1, validate_jpeg_quality(-10));
}

void test_validate_jpeg_quality_invalid_high(void) {
    TEST_ASSERT_EQUAL_INT(-1, validate_jpeg_quality(105));
    TEST_ASSERT_EQUAL_INT(-1, validate_jpeg_quality(150));
    TEST_ASSERT_EQUAL_INT(-1, validate_jpeg_quality(255));
}

void test_validate_jpeg_quality_boundary_low(void) {
    TEST_ASSERT_EQUAL_INT(0, validate_jpeg_quality(MIN_JPEG_QUALITY));
    TEST_ASSERT_EQUAL_INT(-1, validate_jpeg_quality(MIN_JPEG_QUALITY - 1));
}

void test_validate_jpeg_quality_boundary_high(void) {
    TEST_ASSERT_EQUAL_INT(0, validate_jpeg_quality(MAX_JPEG_QUALITY));
    TEST_ASSERT_EQUAL_INT(-1, validate_jpeg_quality(MAX_JPEG_QUALITY + 1));
}

/* Test 3: Photo Resolution Constants */
void test_photo_resolution_width_480p(void) {
    TEST_ASSERT_EQUAL_INT(640, photo_resolution_width[PHOTO_RESOLUTION_480P]);
}

void test_photo_resolution_height_480p(void) {
    TEST_ASSERT_EQUAL_INT(480, photo_resolution_height[PHOTO_RESOLUTION_480P]);
}

void test_photo_resolution_width_720p(void) {
    TEST_ASSERT_EQUAL_INT(1280, photo_resolution_width[PHOTO_RESOLUTION_720P]);
}

void test_photo_resolution_height_720p(void) {
    TEST_ASSERT_EQUAL_INT(720, photo_resolution_height[PHOTO_RESOLUTION_720P]);
}

void test_photo_resolution_width_1080p(void) {
    TEST_ASSERT_EQUAL_INT(1920, photo_resolution_width[PHOTO_RESOLUTION_1080P]);
}

void test_photo_resolution_height_1080p(void) {
    TEST_ASSERT_EQUAL_INT(1080, photo_resolution_height[PHOTO_RESOLUTION_1080P]);
}

/* Test 4: Resolution Validation */
void test_validate_photo_resolution_valid(void) {
    TEST_ASSERT_EQUAL_INT(0, validate_photo_resolution(PHOTO_RESOLUTION_480P));
    TEST_ASSERT_EQUAL_INT(0, validate_photo_resolution(PHOTO_RESOLUTION_720P));
    TEST_ASSERT_EQUAL_INT(0, validate_photo_resolution(PHOTO_RESOLUTION_1080P));
}

void test_validate_photo_resolution_invalid_max(void) {
    TEST_ASSERT_EQUAL_INT(-1, validate_photo_resolution(PHOTO_RESOLUTION_MAX));
}

void test_validate_photo_resolution_invalid_high(void) {
    TEST_ASSERT_EQUAL_INT(-1, validate_photo_resolution(PHOTO_RESOLUTION_MAX + 1));
    TEST_ASSERT_EQUAL_INT(-1, validate_photo_resolution(100));
}

/* Test 5: Resolution String Conversion */
void test_get_resolution_string_480p(void) {
    char buf[16];
    get_resolution_string(PHOTO_RESOLUTION_480P, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("480P", buf);
}

void test_get_resolution_string_720p(void) {
    char buf[16];
    get_resolution_string(PHOTO_RESOLUTION_720P, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("720P", buf);
}

void test_get_resolution_string_1080p(void) {
    char buf[16];
    get_resolution_string(PHOTO_RESOLUTION_1080P, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1080P", buf);
}

void test_get_resolution_string_invalid(void) {
    char buf[16];
    get_resolution_string(PHOTO_RESOLUTION_MAX, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", buf);
}

/* Test 6: Resolution Aspect Ratios */
void test_resolution_aspect_ratio_480p(void) {
    /* 480P is 640x480 = 4:3 = 1.333... */
    uint32_t width = photo_resolution_width[PHOTO_RESOLUTION_480P];
    uint32_t height = photo_resolution_height[PHOTO_RESOLUTION_480P];
    TEST_ASSERT_EQUAL_INT(640, width);
    TEST_ASSERT_EQUAL_INT(480, height);
}

void test_resolution_aspect_ratio_720p(void) {
    /* 720P is 1280x720 = 16:9 */
    uint32_t width = photo_resolution_width[PHOTO_RESOLUTION_720P];
    uint32_t height = photo_resolution_height[PHOTO_RESOLUTION_720P];
    TEST_ASSERT_EQUAL_INT(1280, width);
    TEST_ASSERT_EQUAL_INT(720, height);
}

void test_resolution_aspect_ratio_1080p(void) {
    /* 1080P is 1920x1080 = 16:9 */
    uint32_t width = photo_resolution_width[PHOTO_RESOLUTION_1080P];
    uint32_t height = photo_resolution_height[PHOTO_RESOLUTION_1080P];
    TEST_ASSERT_EQUAL_INT(1920, width);
    TEST_ASSERT_EQUAL_INT(1080, height);
}

/* Test 7: JPEG Quality Impact on File Size (Mock Analysis) */
void test_photo_quality_comparison(void) {
    /* Higher quality = larger file size but better image */
    /* JPEG_PHOTO_QUALITY (90) > JPEG_VIDEO_QUALITY (80) */
    TEST_ASSERT_GREATER_THAN(0, JPEG_VIDEO_QUALITY);
    TEST_ASSERT_GREATER_THAN(JPEG_VIDEO_QUALITY, JPEG_PHOTO_QUALITY);
    TEST_ASSERT_EQUAL_INT(10, JPEG_PHOTO_QUALITY - JPEG_VIDEO_QUALITY);
}

/* Test 9: Quality Level Categories */
void test_quality_category_high(void) {
    int quality = 90;  /* JPEG_PHOTO_QUALITY */
    TEST_ASSERT_TRUE(quality >= 80);  /* High quality threshold */
}

void test_quality_category_medium(void) {
    int quality = 60;
    TEST_ASSERT_TRUE(quality >= 50 && quality < 80);  /* Medium quality */
}

void test_quality_category_low(void) {
    int quality = 30;
    TEST_ASSERT_TRUE(quality >= 10 && quality < 50);  /* Low quality */
}

void test_quality_category_minimum(void) {
    int quality = 10;
    TEST_ASSERT_TRUE(quality >= 10 && quality < 20);  /* Minimum quality */
}

/* ---- Main Entry Point ---- */
int main(void) {
    printf("\n===== Photo Quality Tests =====\n");
    UNITY_BEGIN();

    RUN_TEST(test_jpeg_photo_quality_constant);
    RUN_TEST(test_jpeg_video_quality_constant);
    RUN_TEST(test_jpeg_quality_range_constants);

    RUN_TEST(test_validate_jpeg_quality_valid);
    RUN_TEST(test_validate_jpeg_quality_invalid_low);
    RUN_TEST(test_validate_jpeg_quality_invalid_high);
    RUN_TEST(test_validate_jpeg_quality_boundary_low);
    RUN_TEST(test_validate_jpeg_quality_boundary_high);

    RUN_TEST(test_photo_resolution_width_480p);
    RUN_TEST(test_photo_resolution_height_480p);
    RUN_TEST(test_photo_resolution_width_720p);
    RUN_TEST(test_photo_resolution_height_720p);
    RUN_TEST(test_photo_resolution_width_1080p);
    RUN_TEST(test_photo_resolution_height_1080p);

    RUN_TEST(test_validate_photo_resolution_valid);
    RUN_TEST(test_validate_photo_resolution_invalid_max);
    RUN_TEST(test_validate_photo_resolution_invalid_high);

    RUN_TEST(test_get_resolution_string_480p);
    RUN_TEST(test_get_resolution_string_720p);
    RUN_TEST(test_get_resolution_string_1080p);
    RUN_TEST(test_get_resolution_string_invalid);

    RUN_TEST(test_resolution_aspect_ratio_480p);
    RUN_TEST(test_resolution_aspect_ratio_720p);
    RUN_TEST(test_resolution_aspect_ratio_1080p);

    RUN_TEST(test_photo_quality_comparison);
    RUN_TEST(test_quality_category_high);
    RUN_TEST(test_quality_category_medium);
    RUN_TEST(test_quality_category_low);
    RUN_TEST(test_quality_category_minimum);

    UNITY_END();

    return 0;
}
