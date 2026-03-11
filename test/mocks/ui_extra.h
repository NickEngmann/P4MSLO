#ifndef UI_EXTRA_H
#define UI_EXTRA_H

#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"

// UI State Machine Types
typedef enum {
    UI_STATE_IDLE = 0,
    UI_STATE_VIEWFINDER,
    UI_STATE_PHOTO_CAPTURED,
    UI_STATE_VIDEO_RECORDING,
    UI_STATE_VIDEO_STOPPED,
    UI_STATE_AI_PROCESSING,
    UI_STATE_MENU,
    UI_STATE_SETTINGS,
    UI_STATE_ALBUM,
    UI_STATE_ERROR,
    UI_STATE_SLEEP,
    UI_STATE_WAKEUP,
    UI_STATE_COUNT
} ui_state_t;

// Page types
typedef enum {
    UI_PAGE_MAIN = 0,
    UI_PAGE_CAMERA,
    UI_PAGE_ALBUM,
    UI_PAGE_SETTINGS,
    UI_PAGE_USB_DISK,
    UI_PAGE_INTERVAL_CAM,
    UI_PAGE_VIDEO_MODE,
    UI_PAGE_AI_DETECT,
    UI_PAGE_PIC_SAVE,
    UI_PAGE_COUNT
} ui_page_t;

// Button states
typedef enum {
    BTN_STATE_RELEASED = 0,
    BTN_STATE_PRESSED,
    BTN_STATE_LONG_PRESS,
    BTN_STATE_COUNT
} btn_state_t;

// Settings info structure
typedef struct {
    char od[8];
    char resolution[8];
    char flash[8];
    char gyroscope[8];
} settings_info_t;

// AI Detect Mode
typedef enum {
    AI_DETECT_NONE = 0,
    AI_DETECT_FACE,
    AI_DETECT_PEDESTRIAN,
    AI_DETECT_COUNT
} ai_detect_mode_t;

// UI context structure
 typedef struct {
    ui_state_t current_state;
    ui_state_t previous_state;
    ui_page_t current_page;
    btn_state_t power_btn;
    btn_state_t ok_btn;
    btn_state_t back_btn;
    int32_t knob_position;
    uint8_t sd_card_present;
    uint8_t usb_connected;
    uint8_t battery_level;
    uint8_t is_ai_mode;
    uint8_t photo_count;
    uint8_t video_count;
    uint32_t last_photo_time;
    uint32_t last_video_time;
    uint32_t interval_seconds;
    uint8_t magnification;
    uint8_t ai_confidence;
} ui_context_t;

// Mock global variables for testing
extern ui_context_t mock_ui_context;
extern ai_detect_mode_t mock_ai_detect_mode;
extern bool mock_ui_initialized;
extern uint8_t mock_sd_card_mounted;
extern uint8_t mock_usb_disk_mounted;
extern uint16_t mock_saved_photo_count;

// UI Functions
void ui_init(void);
void ui_update(void);
void ui_set_state(ui_state_t state);
ui_state_t ui_get_state(void);
void ui_set_page(ui_page_t page);
ui_page_t ui_get_page(void);
void ui_button_power_press(void);
void ui_button_ok_press(void);
void ui_button_back_press(void);
void ui_knob_rotate(int32_t delta);
void ui_sd_detect(uint8_t present);
void ui_usb_detect(uint8_t connected);
void ui_toggle_ai_mode(void);
uint8_t ui_get_ai_mode(void);

// UI Extra Functions
void ui_extra_init(void);
void ui_extra_goto_page(ui_page_t page);
ui_page_t ui_extra_get_current_page(void);
uint8_t ui_extra_is_ui_init(void);
void ui_extra_set_sd_card_mounted(uint8_t mounted);
void ui_extra_set_usb_disk_mounted(uint8_t mounted);

// App Extra Functions
void app_extra_set_magnification_factor(uint8_t factor);
uint8_t app_extra_get_magnification_factor(void);
ai_detect_mode_t ui_extra_get_ai_detect_mode(void);
void app_extra_set_saved_photo_count(uint16_t count);
uint16_t app_extra_get_saved_photo_count(void);

#endif /* UI_EXTRA_H */
