/**
 * @brief UI extra mock for host-based testing
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    UI_PAGE_MAIN = 0,
    UI_PAGE_CAMERA,
    UI_PAGE_SETTINGS,
    UI_PAGE_ALBUM,
    UI_PAGE_USB_DISK,
    UI_PAGE_INTERVAL_CAM,
    UI_PAGE_VIDEO_MODE,
    UI_PAGE_AI_DETECT,
    UI_PAGE_PIC_SAVE,
} ui_page_t;

typedef struct {
    const char *od;
    const char *resolution;
    const char *flash;
    const char *gyroscope;
} settings_info_t;

/* Mock state */
static ui_page_t mock_current_page = UI_PAGE_MAIN;
static int mock_magnification_factor = 1;
static bool mock_ui_initialized = false;
static int mock_ai_detect_mode = 0;
static bool mock_sd_card_mounted = false;
static bool mock_usb_disk_mounted = false;
static uint16_t mock_saved_photo_count = 0;

/* Stubs that track state */
static inline ui_page_t ui_extra_get_current_page(void) { return mock_current_page; }
static inline void ui_extra_goto_page(ui_page_t page) { mock_current_page = page; }
static inline bool ui_extra_is_ui_init(void) { return mock_ui_initialized; }
static inline int ui_extra_get_ai_detect_mode(void) { return mock_ai_detect_mode; }

static inline int app_extra_get_magnification_factor(void) { return mock_magnification_factor; }
static inline void app_extra_set_magnification_factor(int f) { mock_magnification_factor = f; }

static inline void ui_extra_init(void) { mock_ui_initialized = true; }
static inline void ui_extra_btn_menu(void) {}
static inline void ui_extra_btn_up(void) {}
static inline void ui_extra_btn_down(void) {}
static inline void ui_extra_btn_left(void) {}
static inline void ui_extra_btn_right(void) {}
static inline void ui_extra_btn_encoder(void) {}
static inline bool ui_extra_handle_usb_disk_page(void) { return false; }

static inline void ui_extra_set_sd_card_mounted(bool m) { mock_sd_card_mounted = m; }
static inline void ui_extra_set_usb_disk_mounted(bool m) { mock_usb_disk_mounted = m; }
static inline void ui_extra_clear_page(void) {}
static inline void ui_extra_popup_interval_timer_warning(void) {}
static inline void app_extra_set_saved_photo_count(uint16_t c) { mock_saved_photo_count = c; }

/* Extern UI objects (stubs) */
static void *ui_PanelImageScreenAlbumDelete = NULL;
static void *ui_ImageScreenAlbum = NULL;
