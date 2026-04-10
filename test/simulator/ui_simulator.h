/**
 * @file ui_simulator.h
 * @brief ESP32-P4X-EYE UI Simulator Engine
 *
 * Simulates the device UI state machine including:
 * - Page navigation (8 screens)
 * - Button presses (Menu, Up, Down, Encoder)
 * - Rotary encoder rotation (knob left/right with step threshold)
 * - Display state (brightness, lock, backlight)
 * - SD card / USB MSC state transitions
 * - AI detection mode switching
 * - Settings persistence via mock NVS
 * - Interval photography state machine
 * - Screenshot capture to PPM files
 *
 * All screen layouts are rendered as ASCII art to terminal and as
 * 240x240 RGB565 framebuffer for PPM export.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

/* ---- Configuration (override via sim_config.h or -D flags) ---- */
#include "sim_config.h"

/* ---- Screen Dimensions (from config) ---- */
#define SIM_WIDTH   SIM_CFG_WIDTH
#define SIM_HEIGHT  SIM_CFG_HEIGHT
#define SIM_BPP     SIM_CFG_BPP

/* ---- Page Definitions ---- */
typedef enum {
    SIM_PAGE_MAIN = 0,
    SIM_PAGE_CAMERA,
    SIM_PAGE_SETTINGS,
    SIM_PAGE_ALBUM,
    SIM_PAGE_USB_DISK,
    SIM_PAGE_INTERVAL_CAM,
    SIM_PAGE_VIDEO_MODE,
    SIM_PAGE_AI_DETECT,
    SIM_PAGE_PIC_SAVE,
    SIM_PAGE_COUNT
} sim_page_t;

static const char *SIM_PAGE_NAMES[] = {
    "MAIN", "CAMERA", "SETTINGS", "ALBUM", "USB_DISK",
    "INTERVAL_CAM", "VIDEO_MODE", "AI_DETECT", "PIC_SAVE"
};

/* ---- Button Definitions ---- */
typedef enum {
    SIM_BTN_MENU = 0,
    SIM_BTN_UP,
    SIM_BTN_DOWN,
    SIM_BTN_ENCODER,
    SIM_BTN_COUNT
} sim_button_t;

static const char *SIM_BTN_NAMES[] = {
    "MENU", "UP", "DOWN", "ENCODER"
};

/* ---- Knob Direction ---- */
typedef enum {
    SIM_KNOB_LEFT = -1,
    SIM_KNOB_NONE = 0,
    SIM_KNOB_RIGHT = 1,
} sim_knob_dir_t;

/* ---- AI Detection Mode ---- */
typedef enum {
    SIM_AI_FACE = 0,
    SIM_AI_PEDESTRIAN = 1,
    SIM_AI_COCO = 2,
} sim_ai_mode_t;

/* ---- Settings ---- */
typedef struct {
    bool od_enabled;
    int resolution;   /* 0=720P, 1=1080P, 2=480P */
    bool flash_on;
    bool gyroscope_on;
    int contrast;
    int saturation;
    int brightness;
    int hue;
    int interval_time;
    int magnification;
} sim_settings_t;

/* ---- Menu Item (for settings page) ---- */
typedef struct {
    const char *label;
    int value_index;
    int max_values;
    const char **value_labels;
} sim_menu_item_t;

/* ---- Simulator State ---- */
typedef struct {
    /* Current screen */
    sim_page_t current_page;
    sim_page_t previous_page;

    /* Display */
    bool display_on;
    int display_brightness;
    bool display_locked;

    /* SD Card / USB */
    bool sd_card_present;
    bool usb_mounted;

    /* Camera */
    int magnification;
    bool flashlight_on;
    sim_ai_mode_t ai_mode;
    bool interval_active;
    int interval_time_min;

    /* Settings */
    sim_settings_t settings;
    int settings_cursor;       /* Which setting is highlighted */

    /* Knob debounce state */
    int knob_step_counter;
    int knob_last_direction;
    int64_t knob_last_time_ms;
    int knob_step_threshold;

    /* Main menu cursor */
    int main_menu_cursor;      /* 0-5: Camera, Album, Settings, Video, AI, Interval */

    /* Album */
    int album_image_index;
    int album_total_images;

    /* Photo count */
    int photo_count;

    /* Framebuffer (for screenshot export) */
    uint8_t framebuffer[SIM_WIDTH * SIM_HEIGHT * SIM_BPP];

    /* Action log */
    char action_log[SIM_CFG_MAX_LOG_ENTRIES][SIM_CFG_LOG_LINE_LEN];
    int action_log_count;

    /* Screenshot counter */
    int screenshot_count;

    /* Simulation time (ms) */
    int64_t sim_time_ms;
} sim_state_t;

/* ---- Color Palette (RGB888) ---- */
#define SIM_COLOR_BG        SIM_CFG_COLOR_BG
#define SIM_COLOR_HEADER    SIM_CFG_COLOR_HEADER
#define SIM_COLOR_ACCENT    SIM_CFG_COLOR_ACCENT
#define SIM_COLOR_TEXT      SIM_CFG_COLOR_TEXT
#define SIM_COLOR_SELECTED  SIM_CFG_COLOR_SELECTED
#define SIM_COLOR_GREEN     SIM_CFG_COLOR_GREEN
#define SIM_COLOR_RED       SIM_CFG_COLOR_RED
#define SIM_COLOR_GRAY      SIM_CFG_COLOR_GRAY
#define SIM_COLOR_YELLOW    SIM_CFG_COLOR_YELLOW

/* ---- Forward declarations ---- */
static void sim_init(sim_state_t *s);
static void sim_press_button(sim_state_t *s, sim_button_t btn);
static void sim_rotate_knob(sim_state_t *s, sim_knob_dir_t dir, int steps);
static void sim_set_sd_card(sim_state_t *s, bool present);
static void sim_set_usb(sim_state_t *s, bool mounted);
static void sim_render(sim_state_t *s);
static void sim_render_ascii(const sim_state_t *s);
static int sim_save_screenshot(sim_state_t *s, const char *dir);
static void sim_log_action(sim_state_t *s, const char *fmt, ...);
static void sim_advance_time(sim_state_t *s, int64_t delta_ms);

/* ---- Implementation ---- */

#include <stdarg.h>

static void sim_log_action(sim_state_t *s, const char *fmt, ...) {
    if (s->action_log_count >= SIM_CFG_MAX_LOG_ENTRIES) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(s->action_log[s->action_log_count], SIM_CFG_LOG_LINE_LEN, fmt, args);
    va_end(args);
    s->action_log_count++;
}

static void sim_advance_time(sim_state_t *s, int64_t delta_ms) {
    s->sim_time_ms += delta_ms;
}

static void sim_init(sim_state_t *s) {
    memset(s, 0, sizeof(sim_state_t));
    s->current_page = SIM_PAGE_MAIN;
    s->display_on = true;
    s->display_brightness = SIM_CFG_DEFAULT_BRIGHTNESS;
    s->magnification = SIM_CFG_DEFAULT_ZOOM;
    s->knob_step_threshold = SIM_CFG_KNOB_THRESHOLD;
    s->album_total_images = SIM_CFG_DEFAULT_ALBUM_IMAGES;
    s->settings.resolution = SIM_CFG_DEFAULT_RESOLUTION;
    s->settings.contrast = SIM_CFG_DEFAULT_CONTRAST;
    s->settings.saturation = SIM_CFG_DEFAULT_SATURATION;
    s->settings.brightness = SIM_CFG_DEFAULT_BRIGHTNESS;
    s->settings.hue = SIM_CFG_DEFAULT_HUE;
    s->settings.magnification = SIM_CFG_DEFAULT_ZOOM;
    s->settings.interval_time = SIM_CFG_DEFAULT_INTERVAL_TIME;
    sim_log_action(s, "INIT: Simulator initialized, page=MAIN");
}

/* ---- Main menu items ---- */
#define SIM_MAIN_MENU_COUNT SIM_CFG_MENU_COUNT
static const char *SIM_MAIN_MENU_ITEMS[] = {
    "Camera", "Album", "Settings", "Video", "AI Detect", "Interval"
};

static sim_page_t SIM_MAIN_MENU_PAGES[] = {
    SIM_PAGE_CAMERA, SIM_PAGE_ALBUM, SIM_PAGE_SETTINGS,
    SIM_PAGE_VIDEO_MODE, SIM_PAGE_AI_DETECT, SIM_PAGE_INTERVAL_CAM
};

/* ---- Settings menu ---- */
#define SIM_SETTINGS_COUNT SIM_CFG_SETTINGS_COUNT
static const char *SIM_SETTINGS_LABELS[] = {
    "Resolution", "Flash", "Object Detect", "Gyroscope", "Magnification", "Interval"
};
static const char *SIM_RESOLUTION_VALUES[] = {"720P", "1080P", "480P"};
static const char *SIM_ONOFF_VALUES[] = {"Off", "On"};

/* ---- Button handler ---- */
static void sim_press_button(sim_state_t *s, sim_button_t btn) {
    sim_log_action(s, "BTN: %s pressed on page %s", SIM_BTN_NAMES[btn], SIM_PAGE_NAMES[s->current_page]);

    /* USB disk page intercepts all buttons */
    if (s->current_page == SIM_PAGE_USB_DISK) {
        sim_log_action(s, "  USB disk page active, ignoring button");
        return;
    }

    switch (s->current_page) {
    case SIM_PAGE_MAIN:
        switch (btn) {
        case SIM_BTN_UP:
            s->main_menu_cursor = (s->main_menu_cursor + SIM_MAIN_MENU_COUNT - 1) % SIM_MAIN_MENU_COUNT;
            sim_log_action(s, "  Menu cursor → %s", SIM_MAIN_MENU_ITEMS[s->main_menu_cursor]);
            break;
        case SIM_BTN_DOWN:
            s->main_menu_cursor = (s->main_menu_cursor + 1) % SIM_MAIN_MENU_COUNT;
            sim_log_action(s, "  Menu cursor → %s", SIM_MAIN_MENU_ITEMS[s->main_menu_cursor]);
            break;
        case SIM_BTN_ENCODER:
            s->previous_page = s->current_page;
            s->current_page = SIM_MAIN_MENU_PAGES[s->main_menu_cursor];
            sim_log_action(s, "  Navigate → %s", SIM_PAGE_NAMES[s->current_page]);
            break;
        case SIM_BTN_MENU:
            /* Menu button on main page — no-op */
            break;
        default: break;
        }
        break;

    case SIM_PAGE_CAMERA:
    case SIM_PAGE_VIDEO_MODE:
        switch (btn) {
        case SIM_BTN_MENU:
            s->current_page = SIM_PAGE_MAIN;
            sim_log_action(s, "  Back to MAIN");
            break;
        case SIM_BTN_ENCODER:
            /* Take photo / start recording */
            s->photo_count++;
            sim_log_action(s, "  Photo taken! Count: %d", s->photo_count);
            break;
        case SIM_BTN_UP:
            s->flashlight_on = !s->flashlight_on;
            sim_log_action(s, "  Flashlight: %s", s->flashlight_on ? "ON" : "OFF");
            break;
        case SIM_BTN_DOWN:
            /* Toggle AI mode in camera view */
            s->ai_mode = (s->ai_mode + 1) % SIM_CFG_AI_MODE_COUNT;
            sim_log_action(s, "  AI mode: %d", s->ai_mode);
            break;
        default: break;
        }
        break;

    case SIM_PAGE_SETTINGS:
        switch (btn) {
        case SIM_BTN_MENU:
            s->current_page = SIM_PAGE_MAIN;
            sim_log_action(s, "  Back to MAIN");
            break;
        case SIM_BTN_UP:
            s->settings_cursor = (s->settings_cursor + SIM_SETTINGS_COUNT - 1) % SIM_SETTINGS_COUNT;
            sim_log_action(s, "  Settings cursor → %s", SIM_SETTINGS_LABELS[s->settings_cursor]);
            break;
        case SIM_BTN_DOWN:
            s->settings_cursor = (s->settings_cursor + 1) % SIM_SETTINGS_COUNT;
            sim_log_action(s, "  Settings cursor → %s", SIM_SETTINGS_LABELS[s->settings_cursor]);
            break;
        case SIM_BTN_ENCODER:
            /* Cycle setting value */
            switch (s->settings_cursor) {
            case 0: s->settings.resolution = (s->settings.resolution + 1) % SIM_CFG_RESOLUTION_COUNT; break;
            case 1: s->settings.flash_on = !s->settings.flash_on; break;
            case 2: s->settings.od_enabled = !s->settings.od_enabled; break;
            case 3: s->settings.gyroscope_on = !s->settings.gyroscope_on; break;
            case 4: s->settings.magnification = (s->settings.magnification % SIM_CFG_MAX_ZOOM) + 1; break;
            case 5: s->settings.interval_time = (s->settings.interval_time % SIM_CFG_MAX_INTERVAL_TIME) + SIM_CFG_INTERVAL_STEP; break;
            }
            sim_log_action(s, "  Setting changed: %s", SIM_SETTINGS_LABELS[s->settings_cursor]);
            break;
        default: break;
        }
        break;

    case SIM_PAGE_ALBUM:
        switch (btn) {
        case SIM_BTN_MENU:
            s->current_page = SIM_PAGE_MAIN;
            sim_log_action(s, "  Back to MAIN");
            break;
        case SIM_BTN_UP:
            if (s->album_image_index > 0) s->album_image_index--;
            sim_log_action(s, "  Album: image %d/%d", s->album_image_index + 1, s->album_total_images);
            break;
        case SIM_BTN_DOWN:
            if (s->album_image_index < s->album_total_images - 1) s->album_image_index++;
            sim_log_action(s, "  Album: image %d/%d", s->album_image_index + 1, s->album_total_images);
            break;
        case SIM_BTN_ENCODER:
            sim_log_action(s, "  Album: selected image %d", s->album_image_index + 1);
            break;
        default: break;
        }
        break;

    case SIM_PAGE_AI_DETECT:
        switch (btn) {
        case SIM_BTN_MENU:
            s->current_page = SIM_PAGE_MAIN;
            sim_log_action(s, "  Back to MAIN");
            break;
        case SIM_BTN_ENCODER:
            s->ai_mode = (s->ai_mode + 1) % SIM_CFG_AI_MODE_COUNT;
            sim_log_action(s, "  AI mode → %d (%s)", s->ai_mode,
                s->ai_mode == 0 ? "Face" : s->ai_mode == 1 ? "Pedestrian" : "COCO");
            break;
        default: break;
        }
        break;

    case SIM_PAGE_INTERVAL_CAM:
        switch (btn) {
        case SIM_BTN_MENU:
            s->current_page = SIM_PAGE_MAIN;
            s->interval_active = false;
            sim_log_action(s, "  Back to MAIN, interval stopped");
            break;
        case SIM_BTN_ENCODER:
            s->interval_active = !s->interval_active;
            sim_log_action(s, "  Interval: %s", s->interval_active ? "STARTED" : "STOPPED");
            break;
        default: break;
        }
        break;

    case SIM_PAGE_PIC_SAVE:
        if (btn == SIM_BTN_MENU || btn == SIM_BTN_ENCODER) {
            s->current_page = SIM_PAGE_CAMERA;
            sim_log_action(s, "  Back to CAMERA");
        }
        break;

    default:
        break;
    }
}

/* ---- Knob rotation (with debounce/threshold) ---- */
static void sim_rotate_knob(sim_state_t *s, sim_knob_dir_t dir, int steps) {
    sim_log_action(s, "KNOB: %s x%d on page %s",
        dir == SIM_KNOB_LEFT ? "LEFT" : "RIGHT", steps, SIM_PAGE_NAMES[s->current_page]);

    /* Album and USB pages ignore knob */
    if (s->current_page == SIM_PAGE_ALBUM || s->current_page == SIM_PAGE_USB_DISK) {
        return;
    }

    for (int i = 0; i < steps; i++) {
        /* Check timeout or direction change */
        if (s->sim_time_ms - s->knob_last_time_ms > SIM_CFG_KNOB_TIMEOUT_MS || s->knob_last_direction == -dir) {
            s->knob_step_counter = 0;
            s->knob_last_direction = dir;
        }
        s->knob_step_counter++;
        s->knob_last_time_ms = s->sim_time_ms;

        if (s->knob_step_counter >= s->knob_step_threshold) {
            s->knob_step_counter = 0;

            switch (s->current_page) {
            case SIM_PAGE_CAMERA:
            case SIM_PAGE_INTERVAL_CAM:
            case SIM_PAGE_VIDEO_MODE:
            case SIM_PAGE_AI_DETECT:
                /* Zoom in/out */
                s->magnification += (dir == SIM_KNOB_RIGHT ? -1 : 1);
                if (s->magnification < SIM_CFG_MIN_ZOOM) s->magnification = SIM_CFG_MIN_ZOOM;
                if (s->magnification > SIM_CFG_MAX_ZOOM) s->magnification = SIM_CFG_MAX_ZOOM;
                sim_log_action(s, "  Magnification → %dx", s->magnification);
                break;
            case SIM_PAGE_MAIN:
                /* Navigate menu */
                if (dir == SIM_KNOB_LEFT) {
                    s->main_menu_cursor = (s->main_menu_cursor + 1) % SIM_MAIN_MENU_COUNT;
                } else {
                    s->main_menu_cursor = (s->main_menu_cursor + SIM_MAIN_MENU_COUNT - 1) % SIM_MAIN_MENU_COUNT;
                }
                sim_log_action(s, "  Menu cursor → %s", SIM_MAIN_MENU_ITEMS[s->main_menu_cursor]);
                break;
            case SIM_PAGE_SETTINGS:
                /* Navigate settings (same as knob handler in firmware) */
                if (dir == SIM_KNOB_LEFT) {
                    s->settings_cursor = (s->settings_cursor + SIM_SETTINGS_COUNT - 1) % SIM_SETTINGS_COUNT;
                } else {
                    s->settings_cursor = (s->settings_cursor + 1) % SIM_SETTINGS_COUNT;
                }
                sim_log_action(s, "  Settings cursor → %s", SIM_SETTINGS_LABELS[s->settings_cursor]);
                break;
            default:
                break;
            }
        }
    }
    sim_advance_time(s, 50 * steps);
}

/* ---- External state changes ---- */
static void sim_set_sd_card(sim_state_t *s, bool present) {
    s->sd_card_present = present;
    sim_log_action(s, "SD: Card %s", present ? "INSERTED" : "REMOVED");
    if (!present && s->current_page == SIM_PAGE_ALBUM) {
        s->current_page = SIM_PAGE_MAIN;
        sim_log_action(s, "  Forced back to MAIN (no SD card)");
    }
}

static void sim_set_usb(sim_state_t *s, bool mounted) {
    bool was_mounted = s->usb_mounted;
    s->usb_mounted = mounted;
    if (mounted && !was_mounted) {
        s->previous_page = s->current_page;
        s->current_page = SIM_PAGE_USB_DISK;
        sim_log_action(s, "USB: Mounted → USB_DISK page");
    } else if (!mounted && was_mounted) {
        s->current_page = s->previous_page;
        sim_log_action(s, "USB: Unmounted → back to %s", SIM_PAGE_NAMES[s->current_page]);
    }
}

/* ---- Framebuffer rendering ---- */
static void sim_fb_fill_rect(sim_state_t *s, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx, py = y + dy;
            if (px >= 0 && px < SIM_WIDTH && py >= 0 && py < SIM_HEIGHT) {
                int idx = (py * SIM_WIDTH + px) * SIM_BPP;
                s->framebuffer[idx] = r;
                s->framebuffer[idx + 1] = g;
                s->framebuffer[idx + 2] = b;
            }
        }
    }
}

static void sim_fb_draw_char(sim_state_t *s, int x, int y, char c, uint8_t r, uint8_t g, uint8_t b) {
    /* Simple 5x7 bitmap font - just draw a filled rectangle per character */
    if (c >= 32 && c < 127) {
        sim_fb_fill_rect(s, x, y, 5, 7, r, g, b);
    }
}

static void sim_fb_draw_text(sim_state_t *s, int x, int y, const char *text, uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; text[i]; i++) {
        if (text[i] != ' ') {
            sim_fb_draw_char(s, x + i * 7, y, text[i], r, g, b);
        }
    }
}

static void sim_render(sim_state_t *s) {
    /* Clear framebuffer */
    memset(s->framebuffer, 0x1A, sizeof(s->framebuffer));

    /* Header bar */
    sim_fb_fill_rect(s, 0, 0, SIM_WIDTH, 24, SIM_COLOR_HEADER);

    /* Page title */
    sim_fb_draw_text(s, 10, 8, SIM_PAGE_NAMES[s->current_page], SIM_COLOR_ACCENT);

    /* Status icons (right side of header) */
    if (s->sd_card_present)
        sim_fb_fill_rect(s, SIM_WIDTH - 30, 5, 12, 14, SIM_COLOR_GREEN);
    if (s->flashlight_on)
        sim_fb_fill_rect(s, SIM_WIDTH - 50, 5, 12, 14, SIM_COLOR_YELLOW);

    /* Page-specific content */
    switch (s->current_page) {
    case SIM_PAGE_MAIN:
        for (int i = 0; i < SIM_MAIN_MENU_COUNT; i++) {
            int item_y = 40 + i * 30;
            if (i == s->main_menu_cursor) {
                sim_fb_fill_rect(s, 5, item_y - 3, SIM_WIDTH - 10, 24, 0x30, 0x30, 0x50);
                sim_fb_draw_text(s, 20, item_y + 3, SIM_MAIN_MENU_ITEMS[i], SIM_COLOR_SELECTED);
                /* Selection indicator */
                sim_fb_fill_rect(s, 8, item_y + 2, 4, 14, SIM_COLOR_ACCENT);
            } else {
                sim_fb_draw_text(s, 20, item_y + 3, SIM_MAIN_MENU_ITEMS[i], SIM_COLOR_TEXT);
            }
        }
        break;

    case SIM_PAGE_CAMERA:
    case SIM_PAGE_VIDEO_MODE:
        /* Camera viewfinder simulation */
        sim_fb_fill_rect(s, 10, 30, SIM_WIDTH - 20, SIM_HEIGHT - 60, 0x20, 0x30, 0x20);
        /* Zoom indicator */
        {
            char zoom_str[16];
            snprintf(zoom_str, sizeof(zoom_str), "%dx", s->magnification);
            sim_fb_draw_text(s, SIM_WIDTH - 40, 35, zoom_str, SIM_COLOR_ACCENT);
        }
        /* Record indicator for video mode */
        if (s->current_page == SIM_PAGE_VIDEO_MODE) {
            sim_fb_fill_rect(s, 15, 35, 8, 8, SIM_COLOR_RED);
        }
        /* Photo count */
        {
            char count_str[16];
            snprintf(count_str, sizeof(count_str), "#%d", s->photo_count);
            sim_fb_draw_text(s, 15, SIM_HEIGHT - 25, count_str, SIM_COLOR_TEXT);
        }
        break;

    case SIM_PAGE_SETTINGS:
        for (int i = 0; i < SIM_SETTINGS_COUNT; i++) {
            int item_y = 35 + i * 32;
            const char *value_str = "";
            switch (i) {
            case 0: value_str = SIM_RESOLUTION_VALUES[s->settings.resolution]; break;
            case 1: value_str = SIM_ONOFF_VALUES[s->settings.flash_on ? 1 : 0]; break;
            case 2: value_str = SIM_ONOFF_VALUES[s->settings.od_enabled ? 1 : 0]; break;
            case 3: value_str = SIM_ONOFF_VALUES[s->settings.gyroscope_on ? 1 : 0]; break;
            case 4: { static char mbuf[8]; snprintf(mbuf, 8, "%dx", s->settings.magnification); value_str = mbuf; break; }
            case 5: { static char ibuf[8]; snprintf(ibuf, 8, "%dmin", s->settings.interval_time); value_str = ibuf; break; }
            }

            if (i == s->settings_cursor) {
                sim_fb_fill_rect(s, 5, item_y - 2, SIM_WIDTH - 10, 26, 0x30, 0x30, 0x50);
                sim_fb_fill_rect(s, 8, item_y + 2, 4, 14, SIM_COLOR_ACCENT);
            }
            uint8_t tr = (i == s->settings_cursor) ? 0xFF : 0xCC;
            uint8_t tg = (i == s->settings_cursor) ? 0x69 : 0xCC;
            uint8_t tb = (i == s->settings_cursor) ? 0x00 : 0xCC;
            sim_fb_draw_text(s, 20, item_y + 3, SIM_SETTINGS_LABELS[i], tr, tg, tb);
            sim_fb_draw_text(s, SIM_WIDTH - 70, item_y + 3, value_str, SIM_COLOR_GREEN);
        }
        break;

    case SIM_PAGE_ALBUM:
        /* Photo viewer */
        sim_fb_fill_rect(s, 10, 30, SIM_WIDTH - 20, SIM_HEIGHT - 60, 0x30, 0x30, 0x30);
        {
            char album_str[32];
            snprintf(album_str, sizeof(album_str), "Image %d/%d", s->album_image_index + 1, s->album_total_images);
            sim_fb_draw_text(s, 60, SIM_HEIGHT / 2, album_str, SIM_COLOR_TEXT);
        }
        break;

    case SIM_PAGE_AI_DETECT:
        /* AI detection overlay */
        sim_fb_fill_rect(s, 10, 30, SIM_WIDTH - 20, SIM_HEIGHT - 60, 0x20, 0x25, 0x30);
        {
            const char *mode_str = s->ai_mode == 0 ? "FACE" : s->ai_mode == 1 ? "PEDESTRIAN" : "COCO";
            sim_fb_draw_text(s, 80, 35, mode_str, SIM_COLOR_ACCENT);
            /* Simulated detection boxes */
            sim_fb_fill_rect(s, 60, 80, 60, 80, 0x00, 0xFF, 0x00); /* green box */
            sim_fb_fill_rect(s, 62, 82, 56, 76, 0x20, 0x25, 0x30); /* clear inside */
        }
        break;

    case SIM_PAGE_USB_DISK:
        sim_fb_draw_text(s, 50, 100, "USB CONNECTED", SIM_COLOR_ACCENT);
        sim_fb_draw_text(s, 40, 130, "Mass Storage Mode", SIM_COLOR_TEXT);
        break;

    case SIM_PAGE_INTERVAL_CAM:
        sim_fb_draw_text(s, 60, 80, "INTERVAL", SIM_COLOR_TEXT);
        {
            char int_str[32];
            snprintf(int_str, sizeof(int_str), "%d min", s->settings.interval_time);
            sim_fb_draw_text(s, 80, 110, int_str, SIM_COLOR_ACCENT);
        }
        if (s->interval_active) {
            sim_fb_fill_rect(s, 100, 140, 40, 20, SIM_COLOR_GREEN);
            sim_fb_draw_text(s, 102, 144, "ACTIVE", 0, 0, 0);
        } else {
            sim_fb_draw_text(s, 80, 144, "Press to start", SIM_COLOR_GRAY);
        }
        break;

    default:
        break;
    }

    /* Bottom status bar */
    sim_fb_fill_rect(s, 0, SIM_HEIGHT - 16, SIM_WIDTH, 16, SIM_COLOR_HEADER);
    {
        char time_str[16];
        snprintf(time_str, sizeof(time_str), "T=%lldms", (long long)s->sim_time_ms);
        sim_fb_draw_text(s, 5, SIM_HEIGHT - 12, time_str, SIM_COLOR_GRAY);
    }
}

/* ---- ASCII art renderer ---- */
static void sim_render_ascii(const sim_state_t *s) {
    printf("\n");
    printf("┌─────────────────────────────────────────────┐\n");
    printf("│ ESP32-P4X-EYE  [%s]%*s│\n",
        SIM_PAGE_NAMES[s->current_page],
        (int)(30 - strlen(SIM_PAGE_NAMES[s->current_page])), "");
    printf("│ SD:%s  USB:%s  Flash:%s  Zoom:%dx %*s│\n",
        s->sd_card_present ? "Y" : "N",
        s->usb_mounted ? "Y" : "N",
        s->flashlight_on ? "ON " : "OFF",
        s->magnification,
        4, "");
    printf("├─────────────────────────────────────────────┤\n");

    switch (s->current_page) {
    case SIM_PAGE_MAIN:
        for (int i = 0; i < SIM_MAIN_MENU_COUNT; i++) {
            printf("│  %s %-38s │\n",
                i == s->main_menu_cursor ? "▶" : " ",
                SIM_MAIN_MENU_ITEMS[i]);
        }
        for (int i = SIM_MAIN_MENU_COUNT; i < 10; i++) {
            printf("│%45s│\n", "");
        }
        break;

    case SIM_PAGE_CAMERA:
    case SIM_PAGE_VIDEO_MODE:
        printf("│  ┌───────────────────────────────────────┐  │\n");
        printf("│  │                                       │  │\n");
        printf("│  │        [Camera Viewfinder]             │  │\n");
        printf("│  │                                       │  │\n");
        printf("│  │                        Zoom: %dx       │  │\n", s->magnification);
        printf("│  │                                       │  │\n");
        printf("│  └───────────────────────────────────────┘  │\n");
        printf("│  Photos: %d                                  │\n", s->photo_count);
        printf("│  %s                                        │\n",
            s->current_page == SIM_PAGE_VIDEO_MODE ? "● REC" : "     ");
        printf("│%45s│\n", "");
        break;

    case SIM_PAGE_SETTINGS:
        for (int i = 0; i < SIM_SETTINGS_COUNT; i++) {
            const char *val = "";
            switch (i) {
            case 0: val = SIM_RESOLUTION_VALUES[s->settings.resolution]; break;
            case 1: val = s->settings.flash_on ? "On" : "Off"; break;
            case 2: val = s->settings.od_enabled ? "On" : "Off"; break;
            case 3: val = s->settings.gyroscope_on ? "On" : "Off"; break;
            case 4: { static char mb[8]; snprintf(mb, 8, "%dx", s->settings.magnification); val = mb; break; }
            case 5: { static char ib[8]; snprintf(ib, 8, "%dm", s->settings.interval_time); val = ib; break; }
            }
            printf("│  %s %-20s %15s  │\n",
                i == s->settings_cursor ? "▶" : " ",
                SIM_SETTINGS_LABELS[i], val);
        }
        for (int i = SIM_SETTINGS_COUNT; i < 10; i++) {
            printf("│%45s│\n", "");
        }
        break;

    case SIM_PAGE_ALBUM:
        printf("│%45s│\n", "");
        printf("│  ┌───────────────────────────────────────┐  │\n");
        printf("│  │         Image %d / %d                   │  │\n",
            s->album_image_index + 1, s->album_total_images);
        printf("│  │                                       │  │\n");
        printf("│  │         [Photo Preview]               │  │\n");
        printf("│  │                                       │  │\n");
        printf("│  └───────────────────────────────────────┘  │\n");
        printf("│  ← UP    DOWN →                             │\n");
        printf("│%45s│\n", "");
        printf("│%45s│\n", "");
        break;

    case SIM_PAGE_AI_DETECT:
        {
            const char *mode = s->ai_mode == 0 ? "Face" : s->ai_mode == 1 ? "Pedestrian" : "COCO";
            printf("│  Mode: %-37s│\n", mode);
            printf("│  ┌───────────────────────────────────────┐  │\n");
            printf("│  │  ┌──────┐                             │  │\n");
            printf("│  │  │ Det. │                             │  │\n");
            printf("│  │  └──────┘                             │  │\n");
            printf("│  │                                       │  │\n");
            printf("│  └───────────────────────────────────────┘  │\n");
            printf("│%45s│\n", "");
            printf("│%45s│\n", "");
            printf("│%45s│\n", "");
        }
        break;

    default:
        for (int i = 0; i < 10; i++) {
            printf("│%45s│\n", "");
        }
        break;
    }

    printf("├─────────────────────────────────────────────┤\n");
    printf("│ T=%6lldms                                   │\n", (long long)s->sim_time_ms);
    printf("└─────────────────────────────────────────────┘\n");
}

/* ---- PPM screenshot export ---- */
static int sim_save_screenshot(sim_state_t *s, const char *dir) {
    sim_render(s);

    mkdir(dir, 0755);

    char filename[256];
    snprintf(filename, sizeof(filename), "%s/screenshot_%03d_%s.ppm",
        dir, s->screenshot_count, SIM_PAGE_NAMES[s->current_page]);

    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", filename);
        return -1;
    }

    fprintf(f, "P6\n%d %d\n255\n", SIM_WIDTH, SIM_HEIGHT);
    fwrite(s->framebuffer, 1, SIM_WIDTH * SIM_HEIGHT * SIM_BPP, f);
    fclose(f);

    s->screenshot_count++;
    sim_log_action(s, "SCREENSHOT: %s", filename);
    printf("  📸 Saved: %s\n", filename);
    return 0;
}
