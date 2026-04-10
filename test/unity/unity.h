/**
 * @brief Minimal Unity test framework for ESP32-P4 host-based testing
 * Based on ThrowTheSwitch/Unity, simplified for embedded CI/CD
 */
#pragma once

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Test state */
static int unity_tests_run = 0;
static int unity_tests_passed = 0;
static int unity_tests_failed = 0;
static int unity_current_test_failed = 0;
static const char *unity_current_test_name = NULL;

/* Colors */
#define UNITY_RED    "\033[31m"
#define UNITY_GREEN  "\033[32m"
#define UNITY_YELLOW "\033[33m"
#define UNITY_RESET  "\033[0m"

/* Macros */
#define TEST_ASSERT(condition) do { \
    if (!(condition)) { \
        printf("  " UNITY_RED "FAIL" UNITY_RESET " %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        unity_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_TRUE(condition) TEST_ASSERT(condition)
#define TEST_ASSERT_FALSE(condition) TEST_ASSERT(!(condition))

#define TEST_ASSERT_EQUAL(expected, actual) do { \
    long long _e = (long long)(expected); \
    long long _a = (long long)(actual); \
    if (_e != _a) { \
        printf("  " UNITY_RED "FAIL" UNITY_RESET " %s:%d: Expected %lld, got %lld\n", \
            __FILE__, __LINE__, _e, _a); \
        unity_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_INT(e, a) TEST_ASSERT_EQUAL(e, a)
#define TEST_ASSERT_EQUAL_UINT8(e, a) TEST_ASSERT_EQUAL(e, a)
#define TEST_ASSERT_EQUAL_UINT16(e, a) TEST_ASSERT_EQUAL(e, a)
#define TEST_ASSERT_EQUAL_UINT32(e, a) TEST_ASSERT_EQUAL(e, a)
#define TEST_ASSERT_EQUAL_INT32(e, a) TEST_ASSERT_EQUAL(e, a)

#define TEST_ASSERT_NOT_EQUAL(expected, actual) do { \
    if ((long long)(expected) == (long long)(actual)) { \
        printf("  " UNITY_RED "FAIL" UNITY_RESET " %s:%d: Expected != %lld\n", \
            __FILE__, __LINE__, (long long)(expected)); \
        unity_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_NULL(ptr) do { \
    if ((ptr) != NULL) { \
        printf("  " UNITY_RED "FAIL" UNITY_RESET " %s:%d: Expected NULL\n", __FILE__, __LINE__); \
        unity_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        printf("  " UNITY_RED "FAIL" UNITY_RESET " %s:%d: Expected non-NULL\n", __FILE__, __LINE__); \
        unity_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) do { \
    if (strcmp((expected), (actual)) != 0) { \
        printf("  " UNITY_RED "FAIL" UNITY_RESET " %s:%d: Expected \"%s\", got \"%s\"\n", \
            __FILE__, __LINE__, (expected), (actual)); \
        unity_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_GREATER_THAN(threshold, actual) do { \
    if ((long long)(actual) <= (long long)(threshold)) { \
        printf("  " UNITY_RED "FAIL" UNITY_RESET " %s:%d: Expected > %lld, got %lld\n", \
            __FILE__, __LINE__, (long long)(threshold), (long long)(actual)); \
        unity_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_LESS_THAN(threshold, actual) do { \
    if ((long long)(actual) >= (long long)(threshold)) { \
        printf("  " UNITY_RED "FAIL" UNITY_RESET " %s:%d: Expected < %lld, got %lld\n", \
            __FILE__, __LINE__, (long long)(threshold), (long long)(actual)); \
        unity_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_MEMORY(expected, actual, len) do { \
    if (memcmp((expected), (actual), (len)) != 0) { \
        printf("  " UNITY_RED "FAIL" UNITY_RESET " %s:%d: Memory mismatch (%zu bytes)\n", \
            __FILE__, __LINE__, (size_t)(len)); \
        unity_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define TEST_FAIL_MESSAGE(msg) do { \
    printf("  " UNITY_RED "FAIL" UNITY_RESET " %s:%d: %s\n", __FILE__, __LINE__, msg); \
    unity_current_test_failed = 1; \
    return; \
} while(0)

/* Test runner macros */
#define RUN_TEST(func) do { \
    unity_current_test_failed = 0; \
    unity_current_test_name = #func; \
    unity_tests_run++; \
    func(); \
    if (unity_current_test_failed) { \
        unity_tests_failed++; \
        printf("  " UNITY_RED "[FAIL]" UNITY_RESET " %s\n", #func); \
    } else { \
        unity_tests_passed++; \
        printf("  " UNITY_GREEN "[PASS]" UNITY_RESET " %s\n", #func); \
    } \
} while(0)

#define UNITY_BEGIN() do { \
    unity_tests_run = 0; \
    unity_tests_passed = 0; \
    unity_tests_failed = 0; \
} while(0)

#define UNITY_END() do { \
    printf("\n----- Results -----\n"); \
    printf("%d Tests %d Failures %d Ignored\n", unity_tests_run, unity_tests_failed, 0); \
    if (unity_tests_failed > 0) { \
        printf(UNITY_RED "FAILED" UNITY_RESET "\n"); \
    } else { \
        printf(UNITY_GREEN "OK" UNITY_RESET "\n"); \
    } \
    return unity_tests_failed; \
} while(0)
