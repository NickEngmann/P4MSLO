/**
 * @file ui_extra.c
 * @brief UI Extra Module Implementation for Testing
 * 
 * Mock implementation of UI extra functions for testing purposes.
 */

#include "ui_extra.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_log.h"

// Global mock state
ui_context_t mock_ui_context = {0};
ai_detect_mode_t mock_ai_detect_mode = AI_DETECT_NONE;
bool mock_ui_initialized = false;
uint8_t mock_sd_card_mounted = 0;
uint8_t mock_usb_disk_mounted = 0;
uint16_t mock_saved_photo_count = 0;

static const char *TAG = "ui_extra";

void ui_init(void) {
    mock_ui_initialized = true;
    mock_ui_context.current_state = UI_STATE_IDLE;
    mock_ui_context.current_page = UI_PAGE_MAIN;
    mock_ui_context.knob_position = 0;
    mock_ui_context.power_btn = BTN_STATE_RELEASED;
    mock_ui_context.ok_btn = BTN_STATE_RELEASED;
    mock_ui_context.back_btn = BTN_STATE_RELEASED;
    ESP_LOGI(TAG, "UI initialized");
}

void ui_update(void) {
    if (!mock_ui_initialized) {
        return;
    }
    // Simulate UI update logic
}

void ui_set_state(ui_state_t state) {
    mock_ui_context.previous_state = mock_ui_context.current_state;
    mock_ui_context.current_state = state;
}

ui_state_t ui_get_state(void) {
    return mock_ui_context.current_state;
}

void ui_set_page(ui_page_t page) {
    mock_ui_context.current_page = page;
}

ui_page_t ui_get_page(void) {
    return mock_ui_context.current_page;
}

void ui_button_power_press(void) {
    mock_ui_context.power_btn = BTN_STATE_PRESSED;
}

void ui_button_ok_press(void) {
    mock_ui_context.ok_btn = BTN_STATE_PRESSED;
}

void ui_button_back_press(void) {
    mock_ui_context.back_btn = BTN_STATE_PRESSED;
}

void ui_knob_rotate(int32_t delta) {
    mock_ui_context.knob_position += delta;
}

void ui_sd_detect(uint8_t present) {
    mock_ui_context.sd_card_present = present;
    mock_sd_card_mounted = present ? 1 : 0;
}

void ui_usb_detect(uint8_t connected) {
    mock_ui_context.usb_connected = connected;
    mock_usb_disk_mounted = connected ? 1 : 0;
}

void ui_toggle_ai_mode(void) {
    mock_ui_context.is_ai_mode = !mock_ui_context.is_ai_mode;
}

uint8_t ui_get_ai_mode(void) {
    return mock_ui_context.is_ai_mode;
}

// UI Extra Functions
void ui_extra_goto_page(ui_page_t page) {
    ui_set_page(page);
}

ui_page_t ui_extra_get_current_page(void) {
    return mock_ui_context.current_page;
}

uint8_t ui_extra_is_ui_init(void) {
    return mock_ui_initialized;
}

void ui_extra_set_sd_card_mounted(uint8_t mounted) {
    mock_sd_card_mounted = mounted;
    ui_sd_detect(mounted);
}

void ui_extra_set_usb_disk_mounted(uint8_t mounted) {
    mock_usb_disk_mounted = mounted;
    ui_usb_detect(mounted);
}

// App Extra Functions
void app_extra_set_magnification_factor(uint8_t factor) {
    mock_ui_context.magnification = factor;
}

uint8_t app_extra_get_magnification_factor(void) {
    return mock_ui_context.magnification;
}

ai_detect_mode_t ui_extra_get_ai_detect_mode(void) {
    return mock_ai_detect_mode;
}

void app_extra_set_saved_photo_count(uint16_t count) {
    mock_saved_photo_count = count;
}

uint16_t app_extra_get_saved_photo_count(void) {
    return mock_saved_photo_count;
}
