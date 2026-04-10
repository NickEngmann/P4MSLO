/**
 * @file sim_config.h
 * @brief UI Simulator Configuration
 *
 * All configurable parameters for the UI simulator engine.
 * When the UI changes (new pages, different menu items, etc.),
 * update this file instead of modifying the simulator core.
 *
 * This file is the single source of truth for:
 * - Screen dimensions and display properties
 * - Menu structure and page definitions
 * - Button/knob behavior tuning
 * - Color palette
 * - Test output directories
 */
#pragma once

/* ---- Display Configuration ---- */
#ifndef SIM_CFG_WIDTH
#define SIM_CFG_WIDTH       240
#endif
#ifndef SIM_CFG_HEIGHT
#define SIM_CFG_HEIGHT      240
#endif
#ifndef SIM_CFG_BPP
#define SIM_CFG_BPP         3       /* RGB888 for PPM output */
#endif

/* ---- Knob Behavior ---- */
#ifndef SIM_CFG_KNOB_THRESHOLD
#define SIM_CFG_KNOB_THRESHOLD  3   /* Steps before triggering action */
#endif
#ifndef SIM_CFG_KNOB_TIMEOUT_MS
#define SIM_CFG_KNOB_TIMEOUT_MS 500 /* Reset counter after this idle time */
#endif

/* ---- Camera / Zoom ---- */
#ifndef SIM_CFG_MIN_ZOOM
#define SIM_CFG_MIN_ZOOM    1
#endif
#ifndef SIM_CFG_MAX_ZOOM
#define SIM_CFG_MAX_ZOOM    8
#endif
#ifndef SIM_CFG_DEFAULT_ZOOM
#define SIM_CFG_DEFAULT_ZOOM 1
#endif

/* ---- Album ---- */
#ifndef SIM_CFG_DEFAULT_ALBUM_IMAGES
#define SIM_CFG_DEFAULT_ALBUM_IMAGES 5
#endif

/* ---- Display Defaults ---- */
#ifndef SIM_CFG_DEFAULT_BRIGHTNESS
#define SIM_CFG_DEFAULT_BRIGHTNESS 100
#endif

/* ---- Settings Defaults ---- */
#ifndef SIM_CFG_DEFAULT_RESOLUTION
#define SIM_CFG_DEFAULT_RESOLUTION  0   /* 0=720P, 1=1080P, 2=480P */
#endif
#ifndef SIM_CFG_RESOLUTION_COUNT
#define SIM_CFG_RESOLUTION_COUNT    3
#endif
#ifndef SIM_CFG_DEFAULT_CONTRAST
#define SIM_CFG_DEFAULT_CONTRAST    50
#endif
#ifndef SIM_CFG_DEFAULT_SATURATION
#define SIM_CFG_DEFAULT_SATURATION  50
#endif
#ifndef SIM_CFG_DEFAULT_HUE
#define SIM_CFG_DEFAULT_HUE         50
#endif
#ifndef SIM_CFG_DEFAULT_INTERVAL_TIME
#define SIM_CFG_DEFAULT_INTERVAL_TIME 5 /* minutes */
#endif
#ifndef SIM_CFG_MAX_INTERVAL_TIME
#define SIM_CFG_MAX_INTERVAL_TIME   60  /* minutes */
#endif
#ifndef SIM_CFG_INTERVAL_STEP
#define SIM_CFG_INTERVAL_STEP       5   /* minutes per click */
#endif

/* ---- Main Menu Configuration ----
 * To add/remove/reorder menu items, update these arrays.
 * SIM_CFG_MENU_COUNT must match the array lengths. */
#ifndef SIM_CFG_MENU_COUNT
#define SIM_CFG_MENU_COUNT  6
#endif

/* ---- Settings Menu Configuration ---- */
#ifndef SIM_CFG_SETTINGS_COUNT
#define SIM_CFG_SETTINGS_COUNT 6
#endif

/* ---- Action Log ---- */
#ifndef SIM_CFG_MAX_LOG_ENTRIES
#define SIM_CFG_MAX_LOG_ENTRIES 64
#endif
#ifndef SIM_CFG_LOG_LINE_LEN
#define SIM_CFG_LOG_LINE_LEN    128
#endif

/* ---- Screenshot Output ---- */
#ifndef SIM_CFG_SCREENSHOT_DIR
#define SIM_CFG_SCREENSHOT_DIR "/workspace/repo/test/simulator/screenshots"
#endif

/* ---- Color Palette (RGB888 triplets) ----
 * Override any color by defining before including this header. */
#ifndef SIM_CFG_COLOR_BG
#define SIM_CFG_COLOR_BG        0x1A, 0x1A, 0x2E
#endif
#ifndef SIM_CFG_COLOR_HEADER
#define SIM_CFG_COLOR_HEADER    0x16, 0x21, 0x3E
#endif
#ifndef SIM_CFG_COLOR_ACCENT
#define SIM_CFG_COLOR_ACCENT    0x00, 0xD2, 0xFF
#endif
#ifndef SIM_CFG_COLOR_TEXT
#define SIM_CFG_COLOR_TEXT      0xFF, 0xFF, 0xFF
#endif
#ifndef SIM_CFG_COLOR_SELECTED
#define SIM_CFG_COLOR_SELECTED  0xFF, 0x69, 0x00
#endif
#ifndef SIM_CFG_COLOR_GREEN
#define SIM_CFG_COLOR_GREEN     0x00, 0xFF, 0x7F
#endif
#ifndef SIM_CFG_COLOR_RED
#define SIM_CFG_COLOR_RED       0xFF, 0x45, 0x45
#endif
#ifndef SIM_CFG_COLOR_GRAY
#define SIM_CFG_COLOR_GRAY      0x88, 0x88, 0x88
#endif
#ifndef SIM_CFG_COLOR_YELLOW
#define SIM_CFG_COLOR_YELLOW    0xFF, 0xD7, 0x00
#endif

/* ---- AI Detection Modes ---- */
#ifndef SIM_CFG_AI_MODE_COUNT
#define SIM_CFG_AI_MODE_COUNT   3
#endif
