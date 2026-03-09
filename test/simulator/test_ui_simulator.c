/**
 * @file test_ui_simulator.c
 * @brief UI Simulator Engine — automated interaction tests with screenshot capture
 *
 * Runs through complete UI workflows:
 * 1. Boot → Main menu navigation
 * 2. Camera page (photo, zoom, flashlight)
 * 3. Settings page (change all settings)
 * 4. Album browsing
 * 5. AI detection mode cycling
 * 6. Interval photography
 * 7. USB/SD card state transitions
 * 8. Knob rotation with threshold debounce
 *
 * Generates PPM screenshots at each step for visual verification.
 */

#include <stdlib.h>
#include "ui_simulator.h"
#include "../unity/unity.h"

/* Use env var if set (for Docker), otherwise default to relative path */
#ifndef SCREENSHOT_DIR
#define SCREENSHOT_DIR_DEFAULT "screenshots"
#define SCREENSHOT_DIR (getenv("SIM_SCREENSHOT_DIR") ? getenv("SIM_SCREENSHOT_DIR") : SCREENSHOT_DIR_DEFAULT)
#endif

static sim_state_t sim;
static int global_screenshot_counter = 0;

/* Wrapper that uses global counter so screenshots don't overwrite across tests */
static int sim_screenshot(sim_state_t *s, const char *dir) {
    s->screenshot_count = global_screenshot_counter;
    int ret = sim_save_screenshot(s, dir);
    global_screenshot_counter = s->screenshot_count;
    return ret;
}

/* ---- Test: Boot and initial state ---- */
void test_sim_boot(void) {
    sim_init(&sim);
    TEST_ASSERT_EQUAL(SIM_PAGE_MAIN, sim.current_page);
    TEST_ASSERT_TRUE(sim.display_on);
    TEST_ASSERT_EQUAL(100, sim.display_brightness);
    TEST_ASSERT_EQUAL(1, sim.magnification);
    TEST_ASSERT_EQUAL(0, sim.main_menu_cursor);
    TEST_ASSERT_FALSE(sim.sd_card_present);
    TEST_ASSERT_FALSE(sim.usb_mounted);
    sim_screenshot(&sim, SCREENSHOT_DIR);
}

/* ---- Test: Main menu navigation ---- */
void test_sim_main_menu_navigation(void) {
    sim_init(&sim);
    sim_render_ascii(&sim);

    /* Navigate down through all menu items */
    for (int i = 0; i < SIM_MAIN_MENU_COUNT; i++) {
        TEST_ASSERT_EQUAL(i, sim.main_menu_cursor);
        sim_screenshot(&sim, SCREENSHOT_DIR);
        sim_press_button(&sim, SIM_BTN_DOWN);
        sim_advance_time(&sim, 100);
    }
    /* Should wrap around */
    TEST_ASSERT_EQUAL(0, sim.main_menu_cursor);

    /* Navigate up */
    sim_press_button(&sim, SIM_BTN_UP);
    TEST_ASSERT_EQUAL(SIM_MAIN_MENU_COUNT - 1, sim.main_menu_cursor);
    sim_render_ascii(&sim);
    sim_screenshot(&sim, SCREENSHOT_DIR);
}

/* ---- Test: Navigate to Camera and take photos ---- */
void test_sim_camera_workflow(void) {
    sim_init(&sim);

    /* Go to Camera (first menu item) */
    sim.main_menu_cursor = 0;
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_EQUAL(SIM_PAGE_CAMERA, sim.current_page);
    sim_render_ascii(&sim);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Take a photo */
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_EQUAL(1, sim.photo_count);

    /* Toggle flashlight */
    sim_press_button(&sim, SIM_BTN_UP);
    TEST_ASSERT_TRUE(sim.flashlight_on);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Take another photo with flash */
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_EQUAL(2, sim.photo_count);

    /* Turn off flashlight */
    sim_press_button(&sim, SIM_BTN_UP);
    TEST_ASSERT_FALSE(sim.flashlight_on);

    /* Go back to main */
    sim_press_button(&sim, SIM_BTN_MENU);
    TEST_ASSERT_EQUAL(SIM_PAGE_MAIN, sim.current_page);
}

/* ---- Test: Zoom via knob rotation ---- */
void test_sim_knob_zoom(void) {
    sim_init(&sim);

    /* Go to Camera */
    sim.current_page = SIM_PAGE_CAMERA;
    TEST_ASSERT_EQUAL(1, sim.magnification);

    /* Rotate knob left (increase magnification) — needs 3 steps for threshold */
    sim_rotate_knob(&sim, SIM_KNOB_LEFT, 3);
    TEST_ASSERT_EQUAL(2, sim.magnification);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Rotate more */
    sim_rotate_knob(&sim, SIM_KNOB_LEFT, 3);
    TEST_ASSERT_EQUAL(3, sim.magnification);

    /* Rotate right (decrease) */
    sim_rotate_knob(&sim, SIM_KNOB_RIGHT, 3);
    TEST_ASSERT_EQUAL(2, sim.magnification);

    /* Can't go below 1x */
    sim_rotate_knob(&sim, SIM_KNOB_RIGHT, 9);
    TEST_ASSERT_EQUAL(1, sim.magnification);

    /* Can't go above 8x */
    sim_rotate_knob(&sim, SIM_KNOB_LEFT, 30);
    TEST_ASSERT_EQUAL(8, sim.magnification);
    sim_screenshot(&sim, SCREENSHOT_DIR);
}

/* ---- Test: Knob timeout resets counter ---- */
void test_sim_knob_timeout(void) {
    sim_init(&sim);
    sim.current_page = SIM_PAGE_CAMERA;

    /* 2 steps, then wait 600ms (> 500ms timeout), then 2 more — should NOT trigger */
    sim_rotate_knob(&sim, SIM_KNOB_LEFT, 2);
    TEST_ASSERT_EQUAL(1, sim.magnification);  /* Threshold is 3, only 2 counted */

    sim_advance_time(&sim, 600);
    sim_rotate_knob(&sim, SIM_KNOB_LEFT, 2);
    TEST_ASSERT_EQUAL(1, sim.magnification);  /* Counter was reset, 2 < 3 */

    /* Now 3 continuous steps should trigger */
    sim_rotate_knob(&sim, SIM_KNOB_LEFT, 3);
    TEST_ASSERT_EQUAL(2, sim.magnification);
}

/* ---- Test: Settings page ---- */
void test_sim_settings(void) {
    sim_init(&sim);

    /* Navigate to Settings */
    sim.main_menu_cursor = 2;
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_EQUAL(SIM_PAGE_SETTINGS, sim.current_page);
    sim_render_ascii(&sim);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Change resolution: 720P → 1080P */
    sim.settings_cursor = 0;
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_EQUAL(1, sim.settings.resolution);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Toggle flash ON */
    sim_press_button(&sim, SIM_BTN_DOWN);
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_TRUE(sim.settings.flash_on);

    /* Toggle OD ON */
    sim_press_button(&sim, SIM_BTN_DOWN);
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_TRUE(sim.settings.od_enabled);

    /* Toggle gyroscope ON */
    sim_press_button(&sim, SIM_BTN_DOWN);
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_TRUE(sim.settings.gyroscope_on);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Back to main */
    sim_press_button(&sim, SIM_BTN_MENU);
    TEST_ASSERT_EQUAL(SIM_PAGE_MAIN, sim.current_page);
}

/* ---- Test: Album browsing ---- */
void test_sim_album(void) {
    sim_init(&sim);
    sim.sd_card_present = true;

    /* Go to Album */
    sim.main_menu_cursor = 1;
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_EQUAL(SIM_PAGE_ALBUM, sim.current_page);
    sim_render_ascii(&sim);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Browse images */
    TEST_ASSERT_EQUAL(0, sim.album_image_index);
    sim_press_button(&sim, SIM_BTN_DOWN);
    TEST_ASSERT_EQUAL(1, sim.album_image_index);
    sim_press_button(&sim, SIM_BTN_DOWN);
    TEST_ASSERT_EQUAL(2, sim.album_image_index);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Go back */
    sim_press_button(&sim, SIM_BTN_UP);
    TEST_ASSERT_EQUAL(1, sim.album_image_index);

    /* Can't go below 0 */
    sim_press_button(&sim, SIM_BTN_UP);
    sim_press_button(&sim, SIM_BTN_UP);
    TEST_ASSERT_EQUAL(0, sim.album_image_index);

    /* Back to main */
    sim_press_button(&sim, SIM_BTN_MENU);
    TEST_ASSERT_EQUAL(SIM_PAGE_MAIN, sim.current_page);
}

/* ---- Test: AI detection mode cycling ---- */
void test_sim_ai_detect(void) {
    sim_init(&sim);

    /* Go to AI Detect */
    sim.main_menu_cursor = 4;
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_EQUAL(SIM_PAGE_AI_DETECT, sim.current_page);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Cycle through modes */
    TEST_ASSERT_EQUAL(SIM_AI_FACE, sim.ai_mode);
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_EQUAL(SIM_AI_PEDESTRIAN, sim.ai_mode);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_EQUAL(SIM_AI_COCO, sim.ai_mode);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_EQUAL(SIM_AI_FACE, sim.ai_mode);  /* Wraps around */

    sim_press_button(&sim, SIM_BTN_MENU);
    TEST_ASSERT_EQUAL(SIM_PAGE_MAIN, sim.current_page);
}

/* ---- Test: Interval photography ---- */
void test_sim_interval(void) {
    sim_init(&sim);

    /* Go to Interval */
    sim.main_menu_cursor = 5;
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_EQUAL(SIM_PAGE_INTERVAL_CAM, sim.current_page);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Start interval */
    TEST_ASSERT_FALSE(sim.interval_active);
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_TRUE(sim.interval_active);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Stop interval */
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_FALSE(sim.interval_active);

    /* Menu stops interval and goes back */
    sim_press_button(&sim, SIM_BTN_ENCODER);
    TEST_ASSERT_TRUE(sim.interval_active);
    sim_press_button(&sim, SIM_BTN_MENU);
    TEST_ASSERT_EQUAL(SIM_PAGE_MAIN, sim.current_page);
    TEST_ASSERT_FALSE(sim.interval_active);
}

/* ---- Test: USB MSC interrupts current page ---- */
void test_sim_usb_msc(void) {
    sim_init(&sim);

    /* Navigate to Camera */
    sim.current_page = SIM_PAGE_CAMERA;

    /* Plug in USB */
    sim_set_usb(&sim, true);
    TEST_ASSERT_EQUAL(SIM_PAGE_USB_DISK, sim.current_page);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Buttons are blocked on USB page */
    sim_press_button(&sim, SIM_BTN_MENU);
    TEST_ASSERT_EQUAL(SIM_PAGE_USB_DISK, sim.current_page);

    /* Unplug USB → returns to Camera */
    sim_set_usb(&sim, false);
    TEST_ASSERT_EQUAL(SIM_PAGE_CAMERA, sim.current_page);
}

/* ---- Test: SD card removal from album ---- */
void test_sim_sd_removal(void) {
    sim_init(&sim);
    sim.sd_card_present = true;
    sim.current_page = SIM_PAGE_ALBUM;

    /* Remove SD card while in Album → forced to MAIN */
    sim_set_sd_card(&sim, false);
    TEST_ASSERT_EQUAL(SIM_PAGE_MAIN, sim.current_page);
    TEST_ASSERT_FALSE(sim.sd_card_present);
}

/* ---- Test: Full workflow simulation ---- */
void test_sim_full_workflow(void) {
    sim_init(&sim);
    sim_set_sd_card(&sim, true);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Main → Camera → take 3 photos → zoom → back */
    sim.main_menu_cursor = 0;
    sim_press_button(&sim, SIM_BTN_ENCODER);
    sim_press_button(&sim, SIM_BTN_ENCODER);  /* photo 1 */
    sim_press_button(&sim, SIM_BTN_ENCODER);  /* photo 2 */
    sim_press_button(&sim, SIM_BTN_ENCODER);  /* photo 3 */
    sim_rotate_knob(&sim, SIM_KNOB_LEFT, 6);  /* zoom 3x */
    sim_screenshot(&sim, SCREENSHOT_DIR);
    sim_press_button(&sim, SIM_BTN_MENU);

    /* Main → Settings → change resolution → back */
    sim.main_menu_cursor = 2;
    sim_press_button(&sim, SIM_BTN_ENCODER);
    sim_press_button(&sim, SIM_BTN_ENCODER);  /* change resolution */
    sim_screenshot(&sim, SCREENSHOT_DIR);
    sim_press_button(&sim, SIM_BTN_MENU);

    /* Main → AI → cycle to COCO → back */
    sim.main_menu_cursor = 4;
    sim_press_button(&sim, SIM_BTN_ENCODER);
    sim_press_button(&sim, SIM_BTN_ENCODER);  /* Face → Pedestrian */
    sim_press_button(&sim, SIM_BTN_ENCODER);  /* Pedestrian → COCO */
    sim_screenshot(&sim, SCREENSHOT_DIR);
    sim_press_button(&sim, SIM_BTN_MENU);

    /* USB interrupt and recovery */
    sim.main_menu_cursor = 0;
    sim_press_button(&sim, SIM_BTN_ENCODER);  /* Camera */
    sim_set_usb(&sim, true);
    sim_screenshot(&sim, SCREENSHOT_DIR);
    sim_set_usb(&sim, false);
    TEST_ASSERT_EQUAL(SIM_PAGE_CAMERA, sim.current_page);
    sim_screenshot(&sim, SCREENSHOT_DIR);

    /* Final state assertions */
    TEST_ASSERT_EQUAL(3, sim.photo_count);
    TEST_ASSERT_EQUAL(1, sim.settings.resolution);  /* Changed from 720P to 1080P */
    TEST_ASSERT_EQUAL(SIM_AI_COCO, sim.ai_mode);

    /* Print action log */
    printf("\n===== Action Log (%d entries) =====\n", sim.action_log_count);
    for (int i = 0; i < sim.action_log_count && i < 30; i++) {
        printf("  %s\n", sim.action_log[i]);
    }
    if (sim.action_log_count > 30) {
        printf("  ... (%d more)\n", sim.action_log_count - 30);
    }
}

/* ---- Main ---- */
int main(void) {
    printf("\n===== ESP32-P4X-EYE UI Simulator Tests =====\n");
    printf("Generating screenshots to %s/\n", SCREENSHOT_DIR);

    UNITY_BEGIN();

    RUN_TEST(test_sim_boot);
    RUN_TEST(test_sim_main_menu_navigation);
    RUN_TEST(test_sim_camera_workflow);
    RUN_TEST(test_sim_knob_zoom);
    RUN_TEST(test_sim_knob_timeout);
    RUN_TEST(test_sim_settings);
    RUN_TEST(test_sim_album);
    RUN_TEST(test_sim_ai_detect);
    RUN_TEST(test_sim_interval);
    RUN_TEST(test_sim_usb_msc);
    RUN_TEST(test_sim_sd_removal);
    RUN_TEST(test_sim_full_workflow);

    UNITY_END();
}
