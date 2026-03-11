/**
 * @brief Photo Quality Test Module Header
 *
 * Tests for photo quality analysis including brightness,
 * contrast, saturation, and quality scoring.
 */

#ifndef TEST_PHOTO_QUALITY_H
#define TEST_PHOTO_QUALITY_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize test photo quality module
 * @return ESP_OK on success
 */
int test_photo_quality_init(void);

/**
 * @brief Cleanup test photo quality module
 */
void test_photo_quality_cleanup(void);

/**
 * @brief Test brightness calculation
 */
void test_brightness_analysis(void);

/**
 * @brief Test contrast calculation
 */
void test_contrast_analysis(void);

/**
 * @brief Test saturation calculation
 */
void test_saturation_analysis(void);

/**
 * @brief Test quality score calculation
 */
void test_quality_score(void);

/**
 * @brief Test NVS storage of quality metrics
 */
void test_quality_metrics_storage(void);

/**
 * @brief Test uniform image quality
 */
void test_uniform_image_quality(void);

/**
 * @brief Test gradient image quality
 */
void test_gradient_image_quality(void);

/**
 * @brief Test edge detection quality
 */
void test_edge_detection_quality(void);

/**
 * @brief Test quality threshold detection
 */
void test_quality_threshold_detection(void);

/**
 * @brief Test quality scoring with different patterns
 */
void test_quality_scoring_patterns(void);

#ifdef __cplusplus
}
#endif

#endif /* TEST_PHOTO_QUALITY_H */
