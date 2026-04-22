/*
 * UI Extra Module
 * 
 * This file implements additional UI functionality for the Eye Factory Demo.
 * It handles the UI interactions, page navigation, settings management,
 * and provides interfaces for controlling the camera features.
 * 
 * The main components include:
 * - Page management (camera, interval camera, video, album, settings)
 * - UI element creation and events handling
 * - Settings storage and retrieval
 * - Button event handlers
 * - Camera control interfaces
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include "esp_log.h"
#include "lvgl.h"

#include "ui_extra.h"

#include "app_storage.h"
#include "app_album.h"
#include "app_video_stream.h"
#include "app_isp.h"
#include "app_gifs.h"
#include "app_pimslo.h"

/* Constants */
#define IMG_BASE_ZOOM       60
#define BTN_ZOOM_FACTOR     2.3
#define IMG_ZOOM_FACTOR     2.4
#define IMG_ZOOM_OFFSET     -80

#define MIN_INTERVAL_TIME   5
#define MAX_INTERVAL_TIME   120
#define INTERVAL_TIME_STEP  5
#define MAX_MAGNIFICATION_FACTOR 4
#define MIN_MAGNIFICATION_FACTOR 1

#define DEFAULT_MAGNIFICATION_FACTOR 1
#define DEFAULT_INTERVAL_TIME 30
#define DEFAULT_SAVED_PHOTO_COUNT 0

static const char * TAG = "ui_extra";

/* Global variables */
// UI state variables
static ui_page_t current_page = UI_PAGE_MAIN;
static bool is_scrolling = false;
static bool is_sd_card_mounted = false;
static bool is_usb_disk_mounted = false;
static bool is_video_recording = false;

static bool is_camera_settings_panel_active = false;

static bool is_ui_init = false;

/* Opaque white panel that covers the camera canvas on the MAIN page.
 * Paired with app_video_stream_free_buffers() to reclaim ~7 MB of PSRAM
 * while the user is on the home screen — that headroom is what lets the
 * GIF encoder run without competing with the viewfinder. */
static lv_obj_t *ui_PanelHomeBackground = NULL;

// AI Detection Mode variables
static ai_detect_mode_t current_ai_detect_mode = AI_DETECT_PEDESTRIAN;
static lv_obj_t *ai_mode_label = NULL;

/* Centered "saving..." overlay on the camera screen. Shown while the
 * PIMSLO capture task is busy (GPIO34 trigger → SPI transfer × 4 → SD
 * writes, ~3-14 s depending on how many cameras respond on first try).
 * Hidden as soon as the capture task clears its busy flag. A single
 * LVGL timer polls the state + animates the trailing dots. */
static lv_obj_t  *saving_label  = NULL;
static lv_timer_t *saving_timer = NULL;
static int       saving_dot_step = 0;

/* Delete-confirmation modal for the GIFs gallery. Mirrors the album's
 * ui_PanelImageScreenAlbumDelete in layout but opens / confirms via the
 * ENCODER (BSP_BUTTON_ED — the same physical button that takes photos
 * in the camera app). The MENU button (BSP_BUTTON_1) always just exits
 * the gallery, so the user has a guaranteed way back even if the modal
 * is open.
 *
 * Selection tracking is a plain boolean rather than LV_STATE_FOCUSED so
 * there's zero chance of LVGL's focus machinery racing the initial
 * state. Styles are applied directly based on `delete_yes_selected`. */
static lv_obj_t *ui_PanelGifsDelete         = NULL;
static lv_obj_t *ui_PanelGifsDeleteTitle    = NULL;
static lv_obj_t *ui_BtnGifsDeleteYes        = NULL;
static lv_obj_t *ui_BtnGifsDeleteNo         = NULL;
static lv_obj_t *ui_LblGifsDeleteYes        = NULL;
static lv_obj_t *ui_LblGifsDeleteNo         = NULL;
static bool       delete_yes_selected       = false;

// UI settings
static uint16_t magnification_factor = DEFAULT_MAGNIFICATION_FACTOR;
static uint16_t interval_time = DEFAULT_INTERVAL_TIME;
static uint16_t saved_photo_count = DEFAULT_SAVED_PHOTO_COUNT;
static uint32_t video_recording_seconds = 0;

static uint32_t contrast_percent = DEFAULT_CONTRAST_PERCENT;
static uint32_t saturation_percent = DEFAULT_SATURATION_PERCENT;
static uint32_t brightness_percent = DEFAULT_BRIGHTNESS_PERCENT;
static uint32_t hue_percent = DEFAULT_HUE_PERCENT;

// UI elements
static lv_obj_t *scroll_cont = NULL;
static lv_obj_t *info_label = NULL;
static lv_obj_t *selected_btn = NULL;
static lv_coord_t btn_width = 0;
static lv_coord_t btn_height = 0;

// Timers
static lv_timer_t *lv_popup_timer = NULL;
static lv_timer_t *lv_additional_photo_timer = NULL;
static lv_timer_t *lv_interval_timer = NULL;
static lv_timer_t *lv_video_timer = NULL;
static lv_timer_t *lv_usb_disk_timer = NULL;

// Settings options
static const char* const gyroscope_options[] = {"Off", "On"};
static const char* const od_options[] = {"Off", "On"};
static const char* const resolution_options[] = {"720P", "1080P", "480P"};
static const char* const flash_options[] = {"Off", "On"};

// Settings state
static int current_settings_item = 0;
static lv_obj_t* settings_items[5];
static settings_info_t current_settings;

static lv_obj_t *camera_settings_items[5]; 
static int current_camera_settings_item = 0;

typedef struct {
    const char* const* options;
    int option_count;
    int current_option;
    lv_obj_t* label;
} setting_options_t;

static setting_options_t settings_options[5];

// Page mapping
typedef struct {
    const char *name;
    int page;
} PageMapping;

static const PageMapping page_map[] = {
    {"CAMERA", UI_PAGE_CAMERA},
    {"INTERVAL CAM", UI_PAGE_INTERVAL_CAM},
    {"VIDEO MODE", UI_PAGE_VIDEO_MODE},
    /* "ALBUM" now routes to the PIMSLO gallery (UI_PAGE_GIFS). The
     * legacy P4-photo album (UI_PAGE_ALBUM) still exists in the enum
     * but is no longer exposed from the main menu. */
    {"ALBUM", UI_PAGE_GIFS},
    {"USB DISK", UI_PAGE_USB_DISK},
    {"SETTINGS", UI_PAGE_SETTINGS},
    {NULL, -1}
};

/* Forward declarations */
static void ui_extra_redirect_to_main_page(void);
static void ui_extra_redirect_to_camera_page(void);
static void ui_extra_redirect_to_interval_camera_page(void);
static void ui_extra_redirect_to_video_mode_page(void);
static void ui_extra_redirect_to_album_page(void);
static void ui_extra_redirect_to_usb_disk_page(void);
static void ui_extra_redirect_to_settings_page(void);
static void ui_extra_redirect_to_gifs_page(void);
static void ui_extra_clear_popup_window(void);
static void ui_extra_focus_on_picture_delete(void);
static void ui_extra_update_ai_detect_mode_label(void);
static void ui_extra_change_ai_detect_mode(ai_detect_mode_t mode);

/*-------------------------------------------------*/
/* Settings Management Functions                    */
/*-------------------------------------------------*/

/**
 * @brief Save current settings to storage
 */
static void save_current_settings(void)
{
    settings_info_t *settings = &current_settings;
    uint16_t interval = interval_time;
    uint16_t magnify = magnification_factor;
    
    app_storage_save_settings(settings, interval, magnify);
}

/**
 * @brief Initialize camera settings display
 */
static void init_camera_settings_display(void) {
    // Set initial value of the slider
    lv_slider_set_value(ui_SliderPanelPanelSettingsContrast, contrast_percent, LV_ANIM_OFF);
    lv_slider_set_value(ui_SliderPanelPanelSettingsSaturation, saturation_percent, LV_ANIM_OFF);
    lv_slider_set_value(ui_SliderPanelPanelSettingsBrightness, brightness_percent, LV_ANIM_OFF);
    lv_slider_set_value(ui_SliderPanelPanelSettingsHue, hue_percent, LV_ANIM_OFF);
}

/**
 * @brief Save camera settings
 */
static void save_camera_settings(void)
{
    app_storage_save_camera_settings(contrast_percent, saturation_percent, 
                                    brightness_percent, hue_percent);
    
    app_isp_set_contrast(contrast_percent);
    app_isp_set_saturation(saturation_percent);
    app_isp_set_brightness(brightness_percent);
    app_isp_set_hue(hue_percent);
}

/**
 * @brief Update camera settings focus
 * @param item_index Index of the item to focus
 */
static void update_camera_settings_focus(int item_index) {
    // Clear focus of all items
    for (int i = 0; i < 5; i++) {
        lv_obj_clear_state(camera_settings_items[i], LV_STATE_FOCUSED);
    }
    
    // Set focus of the current item
    lv_obj_add_state(camera_settings_items[item_index], LV_STATE_FOCUSED);
    current_camera_settings_item = item_index;
}

/**
 * @brief Switch to camera settings panel
 */
static void switch_to_camera_settings_panel(void) {
    // Hide main settings panel
    lv_obj_add_flag(ui_PanelSettings, LV_OBJ_FLAG_HIDDEN);
    // Show camera settings panel
    lv_obj_clear_flag(ui_PanelCameraSettings, LV_OBJ_FLAG_HIDDEN);
    
    // Initialize camera settings items
    camera_settings_items[0] = ui_PanelPanelSettingsContrast;
    camera_settings_items[1] = ui_PanelPanelSettingsSaturation;
    camera_settings_items[2] = ui_PanelPanelSettingsBrightness;
    camera_settings_items[3] = ui_PanelPanelSettingsHue;
    camera_settings_items[4] = ui_PanelSettingsMenu; // "back to menu" item
    
    // Initialize camera settings display
    init_camera_settings_display();
    
    // Reset current selected item and focus on the first item
    current_camera_settings_item = 0;
    update_camera_settings_focus(current_camera_settings_item);
    
    // Update flag
    is_camera_settings_panel_active = true;
}

/**
 * @brief Switch to main settings panel
 */
static void switch_to_main_settings_panel(void) {
    // Hide camera settings panel
    lv_obj_add_flag(ui_PanelCameraSettings, LV_OBJ_FLAG_HIDDEN);
    // Show main settings panel
    lv_obj_clear_flag(ui_PanelSettings, LV_OBJ_FLAG_HIDDEN);
    
    // Update flag
    is_camera_settings_panel_active = false;
}

/**
 * @brief Update display for a specific setting
 * @param setting_index Index of the setting to update
 */
static void update_setting_display(int setting_index) {
    if (setting_index < 0 || setting_index >= 5) {
        ESP_LOGW(TAG, "Invalid setting index: %d", setting_index);
        return;
    }
    
    setting_options_t* opt = &settings_options[setting_index];
    const char* current_text = opt->options[opt->current_option];
    
    // Update the label text
    if (opt->label) {
        lv_label_set_text(opt->label, current_text);
    }
    
    // Update the current settings info
    switch (setting_index) {
        case 0:
            current_settings.gyroscope = current_text;
            break;
        case 1:
            current_settings.od = current_text;
            break;
        case 2:
            current_settings.resolution = current_text;
            break;
        case 3:
            current_settings.flash = current_text;
            break;
    }
    
    save_current_settings();

    ESP_LOGD(TAG, "Setting %d updated to: %s", setting_index, current_text);
}

/**
 * @brief Initialize settings options
 */
static void init_settings_options(void) {
    // Gyroscope options
    settings_options[0].options = gyroscope_options;
    settings_options[0].option_count = sizeof(gyroscope_options) / sizeof(gyroscope_options[0]);
    settings_options[0].current_option = 0;    // Set to 0, corresponds to "Off"
    settings_options[0].label = ui_LabelPanelPanelSettingsGyroscopeBody;
    
    // OD options
    settings_options[1].options = od_options;
    settings_options[1].option_count = sizeof(od_options) / sizeof(od_options[0]);
    settings_options[1].current_option = 1;    // Set to 1, corresponds to "On"
    settings_options[1].label = ui_LabelPanelPanelSettingsODBody;
    
    // Resolution options
    settings_options[2].options = resolution_options;
    settings_options[2].option_count = sizeof(resolution_options) / sizeof(resolution_options[0]);
    settings_options[2].current_option = 1;    // Set to 1, corresponds to "1080P"
    settings_options[2].label = ui_LabelPanelPanelSettingsResBody;
    
    // Flash options
    settings_options[3].options = flash_options;
    settings_options[3].option_count = sizeof(flash_options) / sizeof(flash_options[0]);
    settings_options[3].current_option = 1;    // Set to 1, corresponds to "On"
    settings_options[3].label = ui_LabelPanelPanelSettingsFlashBody;
    
    // Menu options
    settings_options[4].options = NULL;
    settings_options[4].option_count = 0;
    settings_options[4].current_option = 0;
    settings_options[4].label = ui_LabelPanelSettingsMenu;
    
    // Initialize the current settings info
    current_settings.gyroscope = gyroscope_options[0]; // Set to "Off"
    current_settings.od = od_options[1];           // Set to "On"
    current_settings.resolution = resolution_options[1];  // Set to "1080P"
    current_settings.flash = flash_options[1];    // Set to "On"
}

/**
 * @brief Initialize settings display
 */
static void init_settings_display(void) {
    for (int i = 0; i < 4; i++) {  // Only update the first four settings items
        update_setting_display(i);
    }
}

/*-------------------------------------------------*/
/* UI Utility Functions                            */
/*-------------------------------------------------*/

/**
 * @brief Set zoom level for an image object
 * @param obj Image object
 * @param zoom Zoom level
 */
static void app_extra_img_set_zoom(lv_obj_t * obj, uint16_t zoom)
{
    if (!obj) return;
    
    lv_img_t * img = (lv_img_t *)obj;
    if(zoom == img->zoom) return;

    if(zoom == 0) zoom = 1;

    lv_coord_t w = lv_obj_get_width(obj);
    lv_coord_t h = lv_obj_get_height(obj);
    lv_area_t a;
    _lv_img_buf_get_transformed_area(&a, w, h, img->angle, img->zoom >> 8, &img->pivot);
    a.x1 += obj->coords.x1 - 1;
    a.y1 += obj->coords.y1 - 1;
    a.x2 += obj->coords.x1 + 1;
    a.y2 += obj->coords.y1 + 1;
    lv_obj_invalidate_area(obj, &a);

    img->zoom = zoom;

    /* Disable invalidations because lv_obj_refresh_ext_draw_size would invalidate
     * the whole ext draw area */
    lv_disp_t * disp = lv_obj_get_disp(obj);
    lv_disp_enable_invalidation(disp, false);
    lv_obj_refresh_ext_draw_size(obj);
    lv_disp_enable_invalidation(disp, true);

    _lv_img_buf_get_transformed_area(&a, w, h, img->angle, img->zoom, &img->pivot);
    a.x1 += obj->coords.x1 - 1;
    a.y1 += obj->coords.y1 - 1;
    a.x2 += obj->coords.x1 + 1;
    a.y2 += obj->coords.y1 + 1;
    lv_obj_invalidate_area(obj, &a);
}

/**
 * @brief Create an image button
 * @param parent Parent object
 * @param img_src Image source
 * @param btn_text Button text
 * @return Created button object
 */
static lv_obj_t * create_img_button(lv_obj_t *parent, const void *img_src, const char *btn_text) {
    if (!parent || !img_src || !btn_text) {
        ESP_LOGW(TAG, "Invalid parameters for create_img_button");
        return NULL;
    }
    
    lv_obj_t * btn = lv_btn_create(parent);
    
    lv_obj_set_user_data(btn, (void *)btn_text);
    
    // Set the button style
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_shadow_spread(btn, 0, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_ofs_x(btn, 0, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 0, 0);
    
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 30);

    // Create and configure the icon
    lv_obj_t * img = lv_img_create(btn);
    lv_img_set_src(img, img_src);
    lv_obj_set_size(img, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    lv_img_set_zoom(img, IMG_BASE_ZOOM);
    lv_obj_refr_size(img);
    lv_img_set_size_mode(img, LV_IMG_SIZE_MODE_REAL);
    lv_obj_add_flag(img, LV_OBJ_FLAG_FLOATING);
    
    return btn;
}

/**
 * @brief Update settings focus
 * @param new_item New selected item index
 */
static void update_settings_focus(int new_item)
{
    // Clear the focus of all settings items
    for (int i = 0; i < 5; i++) {
        lv_event_send(settings_items[i], LV_EVENT_DEFOCUSED, NULL);
    }
    
    // Set the focus of the new selected item
    current_settings_item = new_item;
    lv_event_send(settings_items[current_settings_item], LV_EVENT_FOCUSED, NULL);
    ESP_LOGD(TAG, "Settings: selected item %d", current_settings_item);
}

/*-------------------------------------------------*/
/* Event Callbacks                                 */
/*-------------------------------------------------*/

/**
 * @brief Scroll event callback
 * @param e Event data
 */
static void scroll_event_cb(lv_event_t * e)
{
    is_scrolling = true; 
    lv_obj_t * cont = lv_event_get_target(e);
    lv_area_t cont_a;
    lv_obj_get_coords(cont, &cont_a);
    lv_coord_t cont_y_center = cont_a.y1 + lv_area_get_height(&cont_a) / 2;

    // Calculate radius for circular motion effect
    lv_coord_t r = lv_obj_get_height(cont) * 7 / 10;
    uint32_t child_cnt = lv_obj_get_child_cnt(cont);
    
    // Find the closest child to center
    lv_obj_t * closest_child = NULL;
    lv_coord_t min_diff = LV_COORD_MAX;
    
    // Process each child button
    for(uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t * child = lv_obj_get_child(cont, i);
        lv_area_t child_a;
        lv_obj_get_coords(child, &child_a);

        // Calculate vertical distance from center
        lv_coord_t child_y_center = child_a.y1 + lv_area_get_height(&child_a) / 2;
        lv_coord_t diff_y = LV_ABS(child_y_center - cont_y_center);
        
        if(diff_y < min_diff) {
            min_diff = diff_y;
            closest_child = child;
        }

        // Calculate horizontal offset using circular motion
        lv_sqrt_res_t sqrt_res;
        lv_sqrt(r * r - diff_y * diff_y, &sqrt_res, 0x8000);
        lv_coord_t x = (diff_y >= r) ? r : sqrt_res.i - r;

        // Apply transformations
        lv_obj_set_style_translate_x(child, x, 0);
        lv_obj_set_style_opa(child, LV_OPA_COVER - lv_map(x, 0, r, LV_OPA_TRANSP, LV_OPA_COVER), 0);
        lv_obj_set_size(child, btn_width, btn_height);

        // Reset icon position
        lv_obj_t * img = lv_obj_get_child(child, 0);
        if(img) {
            app_extra_img_set_zoom(img, IMG_BASE_ZOOM);
            lv_obj_refr_size(img);
            lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
        }
    }
    
    // Apply special effects to closest child
    if(closest_child) {
        lv_obj_set_size(closest_child, btn_width * BTN_ZOOM_FACTOR, btn_height * BTN_ZOOM_FACTOR);
        lv_obj_t * img = lv_obj_get_child(closest_child, 0);
        if(img) {
            app_extra_img_set_zoom(img, IMG_BASE_ZOOM * IMG_ZOOM_FACTOR);
            lv_obj_refr_size(img);
            lv_obj_set_pos(img, IMG_ZOOM_OFFSET, 0);
        }
    }
}

/**
 * @brief Scroll end event callback
 * @param e Event data
 */
static void scroll_end_event_cb(lv_event_t * e)
{
    lv_obj_t * cont = lv_event_get_target(e);
    
    // Get the center coordinates of the container
    lv_area_t cont_a;
    lv_obj_get_coords(cont, &cont_a);
    lv_coord_t cont_y_center = cont_a.y1 + lv_area_get_height(&cont_a) / 2;
    
    // Find the closest child to the center
    uint32_t child_cnt = lv_obj_get_child_cnt(cont);
    lv_obj_t * closest_child = NULL;
    lv_coord_t min_diff = LV_COORD_MAX;
    
    for(uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t * child = lv_obj_get_child(cont, i);
        lv_area_t child_a;
        lv_obj_get_coords(child, &child_a);
        
        lv_coord_t child_y_center = child_a.y1 + lv_area_get_height(&child_a) / 2;
        lv_coord_t diff_y = LV_ABS(child_y_center - cont_y_center);
        
        if(diff_y < min_diff) {
            min_diff = diff_y;
            closest_child = child;
        }
    }
    
    // After scroll end, apply the styles again
    if(closest_child) {
        selected_btn = closest_child;
        const char* btn_text = lv_obj_get_user_data(selected_btn);
        if (btn_text) {
            ESP_LOGD(TAG, "selected: %s", btn_text);

            if (info_label) {
                lv_label_set_text(info_label, btn_text);

                if(current_page == UI_PAGE_MAIN) {
                    lv_obj_clear_flag(info_label, LV_OBJ_FLAG_HIDDEN);
                }

                if(strcmp(btn_text, "CAMERA") == 0) {
                    lv_obj_align(info_label, LV_ALIGN_CENTER, -9, 50);
                } else if(strcmp(btn_text, "INTERVAL CAM") == 0) {
                    lv_obj_align(info_label, LV_ALIGN_CENTER, -12, 45);
                } else if(strcmp(btn_text, "VIDEO MODE") == 0) {
                    lv_obj_align(info_label, LV_ALIGN_CENTER, -9, 45);
                } else if(strcmp(btn_text, "GIFS") == 0) {
                    lv_obj_align(info_label, LV_ALIGN_CENTER, -9, 55);
                } else if(strcmp(btn_text, "ALBUM") == 0) {
                    lv_obj_align(info_label, LV_ALIGN_CENTER, -9, 55);
                } else if(strcmp(btn_text, "USB DISK") == 0) {
                    lv_obj_align(info_label, LV_ALIGN_CENTER, -7, 60);
                } else if(strcmp(btn_text, "SETTINGS") == 0) {
                    lv_obj_align(info_label, LV_ALIGN_CENTER, -9, 60);
                }
            }
        }

        // Reset the styles of all children
        for(uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t * child = lv_obj_get_child(cont, i);
            lv_obj_set_size(child, btn_width, btn_height);

            // Reset the icon position to the default position
            lv_obj_t * img = lv_obj_get_child(child, 0);
            if(img) {
                lv_img_set_zoom(img, IMG_BASE_ZOOM);
                lv_obj_refr_size(img);
                lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
            }
        }
        
        // Zoom the selected child
        lv_obj_set_size(closest_child, btn_width * BTN_ZOOM_FACTOR, btn_height * BTN_ZOOM_FACTOR);

        // Set the special position for the center button
        lv_obj_t * img = lv_obj_get_child(closest_child, 0);
        if(img) {
            lv_img_set_zoom(img, IMG_BASE_ZOOM * IMG_ZOOM_FACTOR);
            lv_obj_refr_size(img);
            lv_obj_set_pos(img, IMG_ZOOM_OFFSET, 0);
        }
        
        // Scroll to the view
        lv_obj_scroll_to_view(closest_child, LV_ANIM_ON);
    }

    is_scrolling = false; 
}

/*-------------------------------------------------*/
/* Timer Callbacks                                 */
/*-------------------------------------------------*/

/* Poll the PIMSLO capture state and show/hide "saving..." on the camera
 * screen accordingly. Only active when the user is on a camera-type
 * page; stays hidden everywhere else so it doesn't draw over the
 * gallery, album, etc. */
static void saving_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!saving_label) return;

    ui_page_t page = ui_extra_get_current_page();
    bool on_camera = (page == UI_PAGE_CAMERA ||
                      page == UI_PAGE_INTERVAL_CAM ||
                      page == UI_PAGE_VIDEO_MODE);

    /* Show the overlay ONLY when the pimslo capture task is actually
     * busy. Previously we also kept it up through the camera sensor's
     * 50-frame warm-up so the user didn't see a static last-frame after
     * the capture window closed — but the warm-up also happens on the
     * very first enter into the camera page after a cold boot, where
     * there is no capture at all. Showing "saving" in that case is
     * confusing. If the user wants to see warm-up feedback we should
     * add a distinct "starting camera..." overlay rather than reusing
     * the saving one. */
    bool should_show = on_camera && app_pimslo_is_capturing();

    bool currently_hidden = lv_obj_has_flag(saving_label, LV_OBJ_FLAG_HIDDEN);

    if (should_show) {
        static const char *patterns[6] = {
            "saving", "saving.", "saving..", "saving...",
            "saving..", "saving.",
        };
        lv_label_set_text(saving_label, patterns[saving_dot_step % 6]);
        saving_dot_step++;
        if (currently_hidden) {
            lv_obj_clear_flag(saving_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(saving_label);
        }
    } else if (!currently_hidden) {
        lv_obj_add_flag(saving_label, LV_OBJ_FLAG_HIDDEN);
        saving_dot_step = 0;
    }
}

/* ---- GIFs delete-modal helpers ---------------------------------- */

/* Apply the highlight to whichever button is currently selected. Uses
 * direct bg_opa writes to LV_STATE_DEFAULT so LVGL's focus-state
 * machinery can't interfere. */
static void gifs_delete_apply_selection(void)
{
    if (!ui_BtnGifsDeleteYes || !ui_BtnGifsDeleteNo) return;
    lv_obj_set_style_bg_color(ui_BtnGifsDeleteYes, lv_color_white(), 0);
    lv_obj_set_style_bg_color(ui_BtnGifsDeleteNo,  lv_color_white(), 0);
    lv_obj_set_style_bg_opa(ui_BtnGifsDeleteYes, delete_yes_selected ? 150 : 0, 0);
    lv_obj_set_style_bg_opa(ui_BtnGifsDeleteNo,  delete_yes_selected ? 0   : 150, 0);
}

/* Create the modal as a child of ui_ScreenGifs. Must be called AFTER
 * ui_ScreenGifs is created (SquareLine screen init is lazy). One-shot:
 * guarded by ui_PanelGifsDelete != NULL. */
static void gifs_delete_modal_ensure_created(void)
{
    if (ui_PanelGifsDelete != NULL) return;
    if (ui_ScreenGifs == NULL) return;       /* screen not yet initialized */

    /* 181×185 rounded-grey panel with "Delete?" title and two YES/NO
     * buttons stacked vertically. Visual style matches the album's
     * ui_PanelImageScreenAlbumDelete. */
    ui_PanelGifsDelete = lv_obj_create(ui_ScreenGifs);
    lv_obj_set_width(ui_PanelGifsDelete, 181);
    lv_obj_set_height(ui_PanelGifsDelete, 185);
    lv_obj_align(ui_PanelGifsDelete, LV_ALIGN_CENTER, -13, 4);
    lv_obj_clear_flag(ui_PanelGifsDelete, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_PanelGifsDelete, 30, 0);
    lv_obj_set_style_bg_color(ui_PanelGifsDelete, lv_color_hex(0xC9C9C9), 0);
    lv_obj_set_style_bg_opa(ui_PanelGifsDelete, 180, 0);
    lv_obj_set_style_border_opa(ui_PanelGifsDelete, 0, 0);
    lv_obj_add_flag(ui_PanelGifsDelete, LV_OBJ_FLAG_HIDDEN);

    ui_PanelGifsDeleteTitle = lv_label_create(ui_PanelGifsDelete);
    lv_obj_align(ui_PanelGifsDeleteTitle, LV_ALIGN_CENTER, 0, -55);
    lv_label_set_text(ui_PanelGifsDeleteTitle, "Delete?");
    lv_obj_set_style_text_color(ui_PanelGifsDeleteTitle, lv_color_black(), 0);
    lv_obj_set_style_text_font(ui_PanelGifsDeleteTitle, &lv_font_montserrat_20, 0);

    ui_BtnGifsDeleteYes = lv_btn_create(ui_PanelGifsDelete);
    lv_obj_set_size(ui_BtnGifsDeleteYes, 140, 30);
    lv_obj_align(ui_BtnGifsDeleteYes, LV_ALIGN_CENTER, 0, -5);
    lv_obj_clear_flag(ui_BtnGifsDeleteYes, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ui_BtnGifsDeleteYes, LV_OBJ_FLAG_CLICKABLE);

    ui_LblGifsDeleteYes = lv_label_create(ui_BtnGifsDeleteYes);
    lv_obj_align(ui_LblGifsDeleteYes, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(ui_LblGifsDeleteYes, "YES");
    lv_obj_set_style_text_color(ui_LblGifsDeleteYes, lv_color_hex(0xFF0202), 0);
    lv_obj_set_style_text_font(ui_LblGifsDeleteYes, &lv_font_montserrat_16, 0);

    ui_BtnGifsDeleteNo = lv_btn_create(ui_PanelGifsDelete);
    lv_obj_set_size(ui_BtnGifsDeleteNo, 140, 30);
    lv_obj_align(ui_BtnGifsDeleteNo, LV_ALIGN_CENTER, 0, 38);
    lv_obj_clear_flag(ui_BtnGifsDeleteNo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ui_BtnGifsDeleteNo, LV_OBJ_FLAG_CLICKABLE);

    ui_LblGifsDeleteNo = lv_label_create(ui_BtnGifsDeleteNo);
    lv_obj_align(ui_LblGifsDeleteNo, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(ui_LblGifsDeleteNo, "NO");
    lv_obj_set_style_text_color(ui_LblGifsDeleteNo, lv_color_black(), 0);
    lv_obj_set_style_text_font(ui_LblGifsDeleteNo, &lv_font_montserrat_16, 0);
}

static bool gifs_delete_modal_open(void)
{
    return ui_PanelGifsDelete != NULL &&
           !lv_obj_has_flag(ui_PanelGifsDelete, LV_OBJ_FLAG_HIDDEN);
}

static void gifs_delete_modal_show(void)
{
    gifs_delete_modal_ensure_created();
    if (!ui_PanelGifsDelete) return;
    /* Default to NO (safer — accidental double-press doesn't delete). */
    delete_yes_selected = false;
    gifs_delete_apply_selection();
    lv_obj_clear_flag(ui_PanelGifsDelete, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui_PanelGifsDelete);
}

static void gifs_delete_modal_hide(void)
{
    if (!ui_PanelGifsDelete) return;
    lv_obj_add_flag(ui_PanelGifsDelete, LV_OBJ_FLAG_HIDDEN);
}

static void gifs_delete_modal_toggle_focus(void)
{
    delete_yes_selected = !delete_yes_selected;
    gifs_delete_apply_selection();
}

static bool gifs_delete_modal_yes_focused(void)
{
    return delete_yes_selected;
}

/**
 * @brief Popup timer callback
 * @param timer Timer object
 */
static void pop_up_timer_callback(lv_timer_t * timer)
{
    if(timer->user_data == ui_PanelCanvasPopupCamera) {
        lv_obj_add_flag(ui_PanelCanvasPopupCamera, LV_OBJ_FLAG_HIDDEN);
    
        lv_obj_clear_flag(ui_PanelCanvasMaskCamera, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_LabelCanvasFactor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_LabelCanvas2X, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_LabelCanvas3X, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_ImageCanvasMenu, LV_OBJ_FLAG_HIDDEN);
        
        is_sd_card_mounted ? lv_obj_clear_flag(ui_ImageCanvasSDcard, LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(ui_ImageCanvasNOSDcard, LV_OBJ_FLAG_HIDDEN);

    } else if(timer->user_data == ui_PanelCanvasPopupCameraInterval) {
        lv_obj_add_flag(ui_PanelCanvasPopupCameraInterval, LV_OBJ_FLAG_HIDDEN);

        lv_obj_clear_flag(ui_PanelCanvasMaskCameraInterval, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_LabelCanvasFactor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_LabelCanvas5mplus, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_LabelCanvas5mSub, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_ImageCanvasMenu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_LabelCanvasInvervalTime, LV_OBJ_FLAG_HIDDEN);

        is_sd_card_mounted ? lv_obj_clear_flag(ui_ImageCanvasSDcard, LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(ui_ImageCanvasNOSDcard, LV_OBJ_FLAG_HIDDEN);

    } else if(timer->user_data == ui_PanelCanvasPopupVideoMode) {
        lv_obj_add_flag(ui_PanelCanvasPopupVideoMode, LV_OBJ_FLAG_HIDDEN);

        lv_obj_clear_flag(ui_PanelCanvasMaskVideoMode, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_ImageCanvasMenu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_LabelCanvas2X, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_LabelCanvas3X, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_LabelCanvasFactor, LV_OBJ_FLAG_HIDDEN);

        is_sd_card_mounted ? lv_obj_clear_flag(ui_ImageCanvasSDcard, LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(ui_ImageCanvasNOSDcard, LV_OBJ_FLAG_HIDDEN);
    } else if(timer->user_data == ui_PanelCanvasPopupAICamera) {
        lv_obj_add_flag(ui_PanelCanvasPopupAICamera, LV_OBJ_FLAG_HIDDEN);

        lv_obj_clear_flag(ui_PanelCanvasMaskAICamera, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_LabelCanvasFactor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_ImageCanvasUp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_ImageCanvasDown, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_ImageCanvasMenu, LV_OBJ_FLAG_HIDDEN);
        
        // Show AI mode label after popup disappears
        if (ai_mode_label != NULL) {
            lv_obj_clear_flag(ai_mode_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if(lv_popup_timer){
        lv_timer_del(lv_popup_timer);
        lv_popup_timer = NULL;
    }
}

/**
 * @brief USB disk timer callback
 * @param timer Timer object
 */
static void usb_disk_timer_callback(lv_timer_t * timer)
{
    lv_obj_add_flag(ui_ImageScreenUSBWarning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_ImageScreenUSB, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_ImageScreenUSBSuccess, LV_OBJ_FLAG_HIDDEN);

    if (lv_usb_disk_timer) {
        lv_timer_del(lv_usb_disk_timer);
        lv_usb_disk_timer = NULL;
    }
}

/**
 * @brief Video timer callback
 * @param timer Timer object
 */
static void video_timer_callback(lv_timer_t * timer)
{
    video_recording_seconds++;
    uint32_t hours = video_recording_seconds / 3600;
    uint32_t minutes = (video_recording_seconds % 3600) / 60;
    uint32_t seconds = video_recording_seconds % 60;
    
    if (hours > 0) {
        lv_label_set_text_fmt(ui_LabelRedDotTime, "%02lu:%02lu:%02lu", hours, minutes, seconds);
    } else {
        lv_label_set_text_fmt(ui_LabelRedDotTime, "%02lu:%02lu", minutes, seconds);
    }
}

/**
 * @brief Interval timer callback
 * @param timer Timer object
 */
static void interval_timer_callback(lv_timer_t * timer)
{
    lv_obj_add_flag(ui_PanelInrervalTimePrompt, LV_OBJ_FLAG_HIDDEN);

    if(lv_interval_timer){
        lv_timer_del(lv_interval_timer);
        lv_interval_timer = NULL;
    }
}

/**
 * @brief Additional photo popup callback
 * @param timer Timer object
 */
static void pop_up_additional_photo_callback(lv_timer_t * timer)
{
    if(timer->user_data == ui_PanelCanvasPopupIntervalTimerWarning) {
        lv_obj_add_flag(ui_PanelCanvasPopupIntervalTimerWarning, LV_OBJ_FLAG_HIDDEN);

        app_video_stream_start_interval_photo(interval_time);
    } else if (timer->user_data == ui_PanelCanvasPopupIntervalTimerWarningEnd) {
        lv_obj_add_flag(ui_PanelCanvasPopupIntervalTimerWarningEnd, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(ui_PanelCanvasMaskCameraInterval, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_LabelCanvasFactor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_LabelCanvas5mplus, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_LabelCanvas5mSub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_ImageCanvasMenu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_LabelCanvasInvervalTime, LV_OBJ_FLAG_HIDDEN);

    is_sd_card_mounted ? lv_obj_clear_flag(ui_ImageCanvasSDcard, LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(ui_ImageCanvasNOSDcard, LV_OBJ_FLAG_HIDDEN);
    
    if(lv_additional_photo_timer){
        lv_timer_del(lv_additional_photo_timer);
        lv_additional_photo_timer = NULL;
    }
}

/*-------------------------------------------------*/
/* UI Creation Functions                           */
/*-------------------------------------------------*/

/**
 * @brief Create scroll container
 */
static void lv_scroll_create(void)
{
    // Create main container
    scroll_cont = lv_obj_create(ui_PanelCanvas);
    lv_obj_set_size(scroll_cont, 240, 240);
    lv_obj_align(scroll_cont, LV_ALIGN_CENTER, -65, 0);
    
    // Set container properties
    lv_obj_set_style_pad_row(scroll_cont, 10, 0);
    lv_obj_set_flex_flow(scroll_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_radius(scroll_cont, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(scroll_cont, true, 0);
    
    // Configure scrolling behavior
    lv_obj_set_scroll_dir(scroll_cont, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(scroll_cont, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(scroll_cont, LV_SCROLLBAR_MODE_OFF);
    
    // Add scroll event handlers
    lv_obj_add_event_cb(scroll_cont, scroll_event_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(scroll_cont, scroll_end_event_cb, LV_EVENT_SCROLL_END, NULL);

    // Set container visual style
    lv_obj_set_style_bg_opa(scroll_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(scroll_cont, LV_OPA_TRANSP, 0);

    info_label = lv_label_create(ui_PanelCanvas);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(info_label, lv_color_hex(0x000000), 0);
    lv_obj_align(info_label, LV_ALIGN_CENTER, 3, 50);
    lv_label_set_text(info_label, "");  

    /* Menu entries in display order. "ALBUM" is the PIMSLO gallery
     * (GIFs + p4ms) — the legacy P4-photo album was removed. ALBUM sits
     * right after CAMERA so users can quickly review recent captures. */
    const char* btn_texts[] = {
        "CAMERA", "ALBUM", "INTERVAL CAM", "VIDEO MODE",
        "USB DISK", "SETTINGS"
    };
    const void* img_srcs[] = {
        &ui_img_camera_big_png,
        &ui_img_album_big_png,     /* ALBUM (PIMSLO gallery) */
        &ui_img_interval_big_png,
        &ui_img_video_big_png,
        &ui_img_usb_big_png,
        &ui_img_settings_big_png,
    };
    
    // Use a loop to create all buttons
    lv_obj_t *btn = NULL;
    for (int i = 0; i < sizeof(btn_texts)/sizeof(btn_texts[0]); i++) {
        btn = create_img_button(
            scroll_cont,
            img_srcs[i],
            btn_texts[i]
        );
    }
        
    lv_obj_update_layout(btn);
    btn_width = lv_obj_get_width(btn);
    btn_height = lv_obj_get_height(btn);

    // Initialize scroll position
    lv_event_send(scroll_cont, LV_EVENT_SCROLL, NULL);
    lv_obj_scroll_to_view(lv_obj_get_child(scroll_cont, 0), LV_ANIM_OFF);

    const char* initial_text = lv_obj_get_user_data(lv_obj_get_child(scroll_cont, 0));
    if (initial_text) {
        lv_label_set_text(info_label, initial_text);
    }
}

/*-------------------------------------------------*/
/* Page Management Functions                       */
/*-------------------------------------------------*/

/**
 * @brief Clear all UI elements from the current page
 */
void ui_extra_clear_page(void)
{
    lv_obj_add_flag(ui_ImageCanvasSelect, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_flag(ui_ImageCanvasUp, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_flag(ui_ImageCanvasDown, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_flag(ui_PanelCanvasMaskLarge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelCanvasPopupCamera, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_ImageCanvasNOSDcard, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_flag(ui_ImageCanvasSDcard, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_flag(ui_ImageCanvasMenu, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_flag(ui_LabelCanvas2X, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_LabelCanvas3X, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_LabelCanvasFactor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelCanvasMaskCamera, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelCanvasMaskAICamera, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelCanvasPopupCameraInterval, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelCanvasMaskCameraInterval, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_LabelCanvas5mplus, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_LabelCanvas5mSub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_LabelCanvasInvervalTime, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelCanvasPopupVideoMode, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelCanvasMaskVideoMode, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelCanvasPopupSDWarning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelCanvasPopupIntervalTimerWarning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelCanvasPopupIntervalTimerWarningEnd, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_ImageRedDot, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_flag(ui_LabelRedDotTime, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelSettings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelSettingsMenu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelCameraSettings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelCanvasPopupAICamera, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(scroll_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(info_label, LV_OBJ_FLAG_HIDDEN);
    
    // Hide AI mode label if it exists
    if (ai_mode_label != NULL) {
        lv_obj_add_flag(ai_mode_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Clear popup windows
 */
static void ui_extra_clear_popup_window(void)
{
    if(!lv_obj_has_flag(ui_PanelCanvasPopupCamera, LV_OBJ_FLAG_HIDDEN) || 
       !lv_obj_has_flag(ui_PanelCanvasPopupCameraInterval, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupVideoMode, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupSDWarning, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarning, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarningEnd, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupAICamera, LV_OBJ_FLAG_HIDDEN)) {

            if(lv_popup_timer) {
                lv_timer_ready(lv_popup_timer);
            }

            if(lv_additional_photo_timer) {
                lv_timer_ready(lv_additional_photo_timer);
            }

            lv_obj_add_flag(ui_PanelCanvasPopupCamera, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_PanelCanvasPopupCameraInterval, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_PanelCanvasPopupVideoMode, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_PanelCanvasPopupSDWarning, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_PanelCanvasPopupIntervalTimerWarning, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_PanelCanvasPopupIntervalTimerWarningEnd, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_PanelCanvasPopupAICamera, LV_OBJ_FLAG_HIDDEN);
            
            // Make sure AI mode label is visible when in AI detect page and popup is cleared
            if (current_page == UI_PAGE_AI_DETECT && ai_mode_label != NULL) {
                lv_obj_clear_flag(ai_mode_label, LV_OBJ_FLAG_HIDDEN);
            }
    }
}

/**
 * @brief Called before every non-MAIN redirect. Hides the white home overlay
 * so the destination page renders without the home backdrop behind it.
 *
 * NOTE: camera buffers are NOT allocated here. Only camera-using pages
 * (CAMERA / INTERVAL_CAM / VIDEO_MODE / AI_DETECT) should pull them back
 * in via app_video_stream_realloc_buffers(); pure-display pages (GIFS,
 * ALBUM, USB_DISK, SETTINGS) need the ~7 MB of PSRAM for their own
 * decoders (GIF playback decode buffer is up to 7.4 MB). A bug in an
 * earlier Phase 2 iteration reallocated unconditionally here and the GIF
 * gallery OOM'd on entry; don't reintroduce that.
 */
static void ui_extra_leaving_main(void)
{
    if (ui_PanelHomeBackground) {
        lv_obj_add_flag(ui_PanelHomeBackground, LV_OBJ_FLAG_HIDDEN);
    }
    /* If the previous page was GIFS, its decoder + canvas buffer are
     * still open — stop playback so subsequent camera pages can
     * reallocate their viewfinder. Idempotent.
     *
     * Also flush the cross-GIF canvas cache (up to ~3.5 MB of PSRAM
     * pinned for previously-watched GIFs). The user isn't viewing the
     * gallery anymore; camera / video / encoder paths need that PSRAM. */
    app_gifs_stop();
    app_gifs_flush_cache();
    /* Hide the delete modal if the user was in the middle of confirming
     * when they navigated away, so it doesn't reappear on re-entry. */
    gifs_delete_modal_hide();
    /* Hide the empty-album / SD-error overlay so other pages don't see
     * it leaked into their layout. Refreshed on next gallery entry. */
    app_gifs_refresh_empty_overlay();    /* no-op when count>0 */
    /* Let the background worker resume — user no longer holds the
     * gallery's decoder / PSRAM. */
    app_gifs_set_gallery_open(false);
}

/**
 * @brief Redirect to main page
 */
static void ui_extra_redirect_to_main_page(void)
{
    current_page = UI_PAGE_MAIN;

    /* Stop GIF playback + flush cache if we're coming from the gallery.
     * Matches what leaving_main() does for non-MAIN destinations. */
    app_gifs_stop();
    app_gifs_flush_cache();
    app_gifs_set_gallery_open(false);

    ui_extra_clear_page();

    lv_obj_clear_flag(scroll_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(info_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_ImageCanvasSelect, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_ImageCanvasUp, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_ImageCanvasDown, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_PanelCanvasMaskLarge, LV_OBJ_FLAG_HIDDEN);

    /* Show the white home background; its move_background at init makes sure
     * it sits behind all the scroll/info UI elements. */
    if (ui_PanelHomeBackground) {
        lv_obj_clear_flag(ui_PanelHomeBackground, LV_OBJ_FLAG_HIDDEN);
    }

    _ui_screen_change(&ui_ScreenCamera, LV_SCR_LOAD_ANIM_NONE, 0, 0, ui_ScreenCamera_screen_init);

    /* Free the viewfinder PSRAM so GIF encoding has room to work. The
     * video_stream frame callback has a buffers_freed guard so this is
     * safe to call while frames are still in flight. */
    app_video_stream_free_buffers();
}

/**
 * @brief Redirect to camera page
 */
static void ui_extra_redirect_to_camera_page(void)
{
    ui_extra_leaving_main();
    app_video_stream_realloc_buffers();  /* viewfinder */
    current_page = UI_PAGE_CAMERA;

    ui_extra_clear_page();

    lv_obj_clear_flag(ui_PanelCanvasPopupCamera, LV_OBJ_FLAG_HIDDEN);
    if(!lv_popup_timer){
        lv_popup_timer = lv_timer_create(pop_up_timer_callback, 5000, ui_PanelCanvasPopupCamera);
    }
}

/**
 * @brief Redirect to interval camera page
 */
static void ui_extra_redirect_to_interval_camera_page(void)
{
    ui_extra_leaving_main();
    app_video_stream_realloc_buffers();  /* viewfinder */
    current_page = UI_PAGE_INTERVAL_CAM;

    ui_extra_clear_page();

    lv_obj_clear_flag(ui_PanelCanvasPopupCameraInterval, LV_OBJ_FLAG_HIDDEN);
    if(!lv_popup_timer){
        lv_popup_timer = lv_timer_create(pop_up_timer_callback, 5000, ui_PanelCanvasPopupCameraInterval);
    }
}

/**
 * @brief Redirect to video mode page
 */
static void ui_extra_redirect_to_video_mode_page(void)
{
    ui_extra_leaving_main();
    app_video_stream_realloc_buffers();  /* viewfinder + video encode */
    current_page = UI_PAGE_VIDEO_MODE;

    ui_extra_clear_page();

    lv_obj_clear_flag(ui_PanelCanvasPopupVideoMode, LV_OBJ_FLAG_HIDDEN);
    if(!lv_popup_timer){
        lv_popup_timer = lv_timer_create(pop_up_timer_callback, 5000, ui_PanelCanvasPopupVideoMode);
    }
}

/**
 * @brief Redirect to album page
 */
static void ui_extra_redirect_to_album_page(void)
{
    ui_extra_leaving_main();
    /* Album's JPEG decoder uses a large PSRAM buffer — free camera
     * buffers if we came from a camera page. Idempotent. */
    app_video_stream_free_buffers();
    current_page = UI_PAGE_ALBUM;

    ui_extra_clear_page();

    _ui_screen_change(&ui_ScreenAlbum, LV_SCR_LOAD_ANIM_NONE, 0, 0, ui_ScreenAlbum_screen_init);

    if(is_sd_card_mounted) {
        lv_obj_add_flag(ui_PanelAlbumPopupSDWarning, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(ui_PanelAlbumPopupSDWarning, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Redirect to GIFs page.
 * Auto-scans the SD card for GIFs and starts playback of the newest one
 * immediately on entry. Feature F1 — no more "press play to start" step.
 */
static bool gifs_initialized = false;

static void ui_extra_redirect_to_gifs_page(void)
{
    ui_extra_leaving_main();
    /* Tell the gallery background worker to pause — it's about to try
     * to open a gif_decoder for pre-rendering, which would conflict with
     * the one we're about to open for playback. */
    app_gifs_set_gallery_open(true);
    /* GIF decoder needs up to ~7.4 MB for the decode buffer — free the
     * camera buffers if we're arriving from a camera page. Idempotent. */
    app_video_stream_free_buffers();
    current_page = UI_PAGE_GIFS;

    ui_extra_clear_page();

    _ui_screen_change(&ui_ScreenGifs, LV_SCR_LOAD_ANIM_NONE, 0, 0, ui_ScreenGifs_screen_init);

    if(is_sd_card_mounted) {
        lv_obj_add_flag(ui_PanelGifsPopupSDWarning, LV_OBJ_FLAG_HIDDEN);
        if (!gifs_initialized) {
            app_gifs_init(ui_ImageScreenGifs);
            gifs_initialized = true;
        }
        /* Refresh the gallery's file list (picks up new GIFs + preview jpgs
         * since last visit) and start playing the newest one immediately —
         * but only if a PIMSLO GIF encode isn't currently running. A running
         * encode holds ~7 MB for its scaled_buf, and the playback decoder
         * needs another ~7 MB, which doesn't fit in PSRAM. When that
         * collision happens, skip auto-play so the user sees the static
         * thumbnail instead of a silently-failed-to-start state — they
         * can navigate and the system will retry on the next redirect. */
        app_gifs_scan();
        if (app_gifs_get_count() > 0) {
            if (app_gifs_is_encoding() || app_pimslo_is_encoding()) {
                ESP_LOGI(TAG, "Gallery entry: encoder busy — skipping auto-play");
            } else {
                app_gifs_play_current();
            }
        } else {
            /* Empty gallery — surface the "Album empty" overlay instead
             * of leaving the user staring at a blank canvas. */
            app_gifs_refresh_empty_overlay();
        }
    } else {
        lv_obj_clear_flag(ui_PanelGifsPopupSDWarning, LV_OBJ_FLAG_HIDDEN);
        /* Also surface the SD-error overlay on the canvas itself so
         * it's unambiguous why nothing is showing. */
        app_gifs_refresh_empty_overlay();
    }
}

/**
 * @brief Redirect to USB disk page
 */
static void ui_extra_redirect_to_usb_disk_page(void)
{
    ui_extra_leaving_main();
    current_page = UI_PAGE_USB_DISK;

    ui_extra_clear_page();
    
    _ui_screen_change(&ui_ScreenUSB, LV_SCR_LOAD_ANIM_NONE, 0, 0, ui_ScreenUSB_screen_init);

    if(is_usb_disk_mounted) {
        lv_obj_add_flag(ui_ImageScreenUSB, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_ImageScreenUSBWarning, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_ImageScreenUSBSuccess, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(ui_ImageScreenUSB, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_ImageScreenUSBSuccess, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_ImageScreenUSBWarning, LV_OBJ_FLAG_HIDDEN);
    }

    if(is_sd_card_mounted) {
        lv_obj_add_flag(ui_ImageUSBNOSDcard, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(ui_ImageUSBNOSDcard, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Redirect to settings page
 */
static void ui_extra_redirect_to_settings_page(void)
{
    ui_extra_leaving_main();
    current_page = UI_PAGE_SETTINGS;

    ui_extra_clear_page();
    
    lv_obj_clear_flag(ui_PanelSettings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_PanelSettingsMenu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_ImageCanvasSelect, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_ImageCanvasUp, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_ImageCanvasDown, LV_OBJ_FLAG_HIDDEN);

    // Initialize settings items
    settings_items[0] = ui_PanelPanelSettingsGyroscope;
    settings_items[1] = ui_PanelPanelSettingsLanguage;
    settings_items[2] = ui_PanelPanelSettingsRes;
    settings_items[3] = ui_PanelPanelSettingsFlash;
    settings_items[4] = ui_PanelSettingsMenu;
    
    // Initialize the settings display
    init_settings_display();
    
    // Reset the current selected item and focus the first item
    current_settings_item = 0;
    update_settings_focus(current_settings_item);

    is_camera_settings_panel_active = false;
}

/**
 * @brief Redirect to AI detect page
 */
static void ui_extra_redirect_to_ai_detect_page(void)
{
    ui_extra_leaving_main();
    app_video_stream_realloc_buffers();  /* viewfinder + AI detection buffers */
    current_page = UI_PAGE_AI_DETECT;

    ui_extra_clear_page();

    // Create AI mode label if it doesn't exist
    if (ai_mode_label == NULL) {
        ai_mode_label = lv_label_create(ui_ScreenCamera);
        lv_obj_set_style_text_font(ai_mode_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(ai_mode_label, lv_color_hex(0xFF0000), 0);
        lv_obj_set_style_bg_color(ai_mode_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(ai_mode_label, LV_OPA_70, 0);
        lv_obj_set_style_radius(ai_mode_label, 5, 0);
        lv_obj_set_style_pad_all(ai_mode_label, 5, 0);
        // Position the label at the left bottom corner
        lv_obj_align(ai_mode_label, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    }
    
    // Initially hide AI mode label when popup is shown
    lv_obj_add_flag(ai_mode_label, LV_OBJ_FLAG_HIDDEN);
    
    // Update label text (it will be shown when popup disappears)
    ui_extra_update_ai_detect_mode_label();
    
    // Show popup
    lv_obj_clear_flag(ui_PanelCanvasPopupAICamera, LV_OBJ_FLAG_HIDDEN);
    if(!lv_popup_timer){
        lv_popup_timer = lv_timer_create(pop_up_timer_callback, 5000, ui_PanelCanvasPopupAICamera);
    }
}

/**
 * @brief Update AI detection mode label
 */
static void ui_extra_update_ai_detect_mode_label(void)
{
    if (ai_mode_label == NULL) {
        return;
    }
    
    if (current_ai_detect_mode == AI_DETECT_PEDESTRIAN) {
        lv_label_set_text(ai_mode_label, "Mode: Pedestrian");
    } else {
        lv_label_set_text(ai_mode_label, "Mode: Face");
    }
}

/**
 * @brief Change AI detection mode
 * @param mode New AI detection mode
 */
static void ui_extra_change_ai_detect_mode(ai_detect_mode_t mode)
{
    if (mode >= AI_DETECT_MODE_MAX) {
        mode = 0;
    }
    
    current_ai_detect_mode = mode;
    
    // Update label text
    ui_extra_update_ai_detect_mode_label();
    
    // Ensure label is visible if popup is not visible
    if (ai_mode_label != NULL && lv_obj_has_flag(ui_PanelCanvasPopupAICamera, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(ai_mode_label, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Log the mode change (mode value is only used for UI display)
    ESP_LOGI(TAG, "Changed AI detection mode display to %s", 
             (current_ai_detect_mode == AI_DETECT_PEDESTRIAN) ? "pedestrian" : "face");
}

/**
 * @brief Toggle focus between YES and NO in picture delete dialog
 */
static void ui_extra_focus_on_picture_delete(void)
{
    if(lv_obj_has_state(ui_ButtonPanelImageScreenAlbumDeleteYES, LV_STATE_FOCUSED)) {
        lv_obj_add_state(ui_ButtonPanelImageScreenAlbumDeleteNO, LV_STATE_FOCUSED);
        lv_obj_clear_state(ui_ButtonPanelImageScreenAlbumDeleteYES, LV_STATE_FOCUSED);
    } else {
        lv_obj_clear_state(ui_ButtonPanelImageScreenAlbumDeleteNO, LV_STATE_FOCUSED);
        lv_obj_add_state(ui_ButtonPanelImageScreenAlbumDeleteYES, LV_STATE_FOCUSED);
    }
}

/*-------------------------------------------------*/
/* Public API Functions                            */
/*-------------------------------------------------*/

/**
 * @brief Navigate to specified page
 * @param page Target page enum value
 */
void ui_extra_goto_page(ui_page_t page)
{
    // Save the current page
    current_page = page;
    
    // Redirect to the page
    switch(page) {
        case UI_PAGE_MAIN:
            ui_extra_redirect_to_main_page();
            break;
        case UI_PAGE_CAMERA:
            ui_extra_redirect_to_camera_page();
            break;
        case UI_PAGE_INTERVAL_CAM:
            ui_extra_redirect_to_interval_camera_page();
            break;
        case UI_PAGE_VIDEO_MODE:
            ui_extra_redirect_to_video_mode_page();
            break;
        case UI_PAGE_ALBUM:
            app_album_refresh();
            ui_extra_redirect_to_album_page();
            break;
        case UI_PAGE_USB_DISK:
            ui_extra_redirect_to_usb_disk_page();
            break;
        case UI_PAGE_SETTINGS:
            ui_extra_redirect_to_settings_page();
            break;
        case UI_PAGE_AI_DETECT:
            ui_extra_redirect_to_ai_detect_page();
            break;
        case UI_PAGE_GIFS:
            /* Don't pre-scan here — ui_extra_redirect_to_gifs_page()
             * calls app_gifs_scan() internally. Calling it twice was
             * wasteful and, more importantly, leaves s_ctx.entries in
             * a half-freed state between the two calls where a racing
             * background worker could read a freed pointer. */
            ui_extra_redirect_to_gifs_page();
            break;
        default:
            ui_extra_redirect_to_main_page();
            break;
    }   
}

/**
 * @brief Get current page
 * @return Current page enum value
 */
ui_page_t ui_extra_get_current_page(void)
{
    return current_page;
}

/**
 * @brief Get chosen page from selected button
 * @return Chosen page enum value
 */
ui_page_t ui_extra_get_choosed_page(void)
{
    const char * user_data = lv_obj_get_user_data(selected_btn);

    for (int i = 0; page_map[i].name != NULL; i++) {
        if (strcmp(user_data, page_map[i].name) == 0) {
            return page_map[i].page;
        }
    }

    return UI_PAGE_MAIN;
}

bool ui_extra_is_ui_init(void)
{
    return is_ui_init;
}

/**
 * @brief Get current settings information
 * @return Pointer to settings information structure
 */
settings_info_t* ui_extra_get_settings(void)
{
    return &current_settings;
}

/**
 * @brief Set magnification factor
 * @param factor Magnification factor value
 */
void app_extra_set_magnification_factor(uint16_t factor)
{
    if(factor > MAX_MAGNIFICATION_FACTOR) {
        factor = MAX_MAGNIFICATION_FACTOR;
    } else if(factor < MIN_MAGNIFICATION_FACTOR) {
        factor = MIN_MAGNIFICATION_FACTOR;
    }

    magnification_factor = factor;
    
    lv_label_set_text_fmt(ui_LabelCanvasFactor, "%dX", magnification_factor);

    save_current_settings();
}

/**
 * @brief Get magnification factor
 * @return Current magnification factor value
 */
uint16_t app_extra_get_magnification_factor(void)
{
    return magnification_factor;
}   

/**
 * @brief Set saved photo count
 * @param count Number of saved photos
 */
void app_extra_set_saved_photo_count(uint16_t count)
{
    saved_photo_count = count;
    app_storage_save_photo_count(saved_photo_count);
}

/**
 * @brief Get saved photo count
 * @return Current saved photo count
 */
uint16_t app_extra_get_saved_photo_count(void)
{
    return saved_photo_count;
}

/**
 * @brief Set interval time
 * @param time Interval time in minutes
 */
void app_extra_set_interval_time(uint16_t time)
{
    // Limit time range
    if(time > MAX_INTERVAL_TIME) {
        time = MIN_INTERVAL_TIME;
    } else if(time < MIN_INTERVAL_TIME) {
        time = MAX_INTERVAL_TIME;
    }

    interval_time = time;

    lv_label_set_text_fmt(ui_LabelCanvasInvervalTime, "%dmin", interval_time);
    lv_label_set_text_fmt(ui_LabelPanelInrervalTimePrompt, "%d", interval_time);

    save_current_settings();
}

/**
 * @brief Get interval time
 * @return Current interval time in minutes
 */
uint16_t app_extra_get_interval_time(void)
{
    return interval_time;
}

/**
 * @brief Set SD card mount status
 * @param mounted Whether SD card is mounted
 */
void ui_extra_set_sd_card_mounted(bool mounted)
{
    is_sd_card_mounted = mounted;
    
    if(current_page != UI_PAGE_CAMERA && current_page != UI_PAGE_INTERVAL_CAM && current_page != UI_PAGE_VIDEO_MODE && current_page != UI_PAGE_ALBUM) {
        return;
    }

    if(is_sd_card_mounted) {
        lv_obj_add_flag(ui_ImageCanvasNOSDcard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_ImageCanvasSDcard, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui_ImageCanvasSDcard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_ImageCanvasNOSDcard, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Get SD card mount status
 * @return Whether SD card is mounted
 */
bool ui_extra_get_sd_card_mounted(void)
{
    return is_sd_card_mounted;
}

/**
 * @brief Cancel and delete the popup timer
 *
 * Useful when programmatically switching pages to prevent a popup timer
 * from a previous page firing and revealing stale UI elements.
 */
void ui_extra_cancel_popup_timer(void)
{
    if (lv_popup_timer) {
        lv_timer_del(lv_popup_timer);
        lv_popup_timer = NULL;
    }
}

/**
 * @brief Set USB disk mount status
 * @param mounted Whether USB disk is mounted
 */
void ui_extra_set_usb_disk_mounted(bool mounted)
{
    is_usb_disk_mounted = mounted;
    
    ui_extra_goto_page(UI_PAGE_USB_DISK);
}

/**
 * @brief Get USB disk mount status
 * @return Whether USB disk is mounted
 */
bool ui_extra_get_usb_disk_mounted(void)
{
    return is_usb_disk_mounted;
}

/**
 * @brief Get current AI detection mode
 * 
 * @details Returns the current AI detection mode that will be displayed
 *          in the UI. This can be either pedestrian detection (AI_DETECT_PEDESTRIAN)
 *          or face detection (AI_DETECT_FACE).
 * 
 * @return Current AI detection mode value
 */
ai_detect_mode_t ui_extra_get_ai_detect_mode(void)
{
    return current_ai_detect_mode;
}

/**
 * @brief Set AI detection mode
 * @param mode New AI detection mode
 */
void ui_extra_set_ai_detect_mode(ai_detect_mode_t mode)
{
    ui_extra_change_ai_detect_mode(mode);
}

/**
 * @brief Get gyroscope enabled status
 * @return True if gyroscope is enabled, false otherwise
 */
bool ui_extra_get_gyroscope_enabled(void)
{
    return (strcmp(current_settings.gyroscope, "On") == 0);
}

/**
 * @brief Get popup window visible status
 * @return Whether popup window is visible
 */
bool ui_extra_get_popup_window_visible(void)
{
    if(!lv_obj_has_flag(ui_PanelCanvasPopupCamera, LV_OBJ_FLAG_HIDDEN) || 
       !lv_obj_has_flag(ui_PanelCanvasPopupCameraInterval, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupVideoMode, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupSDWarning, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarning, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarningEnd, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupAICamera, LV_OBJ_FLAG_HIDDEN)) {
        
        return true;
    }

    return false;
}

/**
 * @brief Start interval timer
 */
void ui_extra_start_interval_timer(void)
{
    ui_extra_clear_page();
    lv_label_set_text_fmt(ui_LabelPanelCanvasPopupIntervalTimer, "Starting %d min", interval_time);
    lv_obj_clear_flag(ui_PanelCanvasPopupIntervalTimerWarning, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief Show interval timer warning popup
 */
void ui_extra_popup_interval_timer_warning(void)
{
    if(!(current_page == UI_PAGE_INTERVAL_CAM)) {
        return;
    }
    app_storage_get_photo_count(&saved_photo_count);
    ui_extra_clear_page();
    lv_label_set_text_fmt(ui_LabelPanelCanvasPopupIntervalTimerEnd, "Ended %d min", interval_time);
    lv_label_set_text_fmt(ui_LabelPanelCanvasPopupCameraIntervalTimerWarningEnd, "%d photos saved to \n       SD Card", saved_photo_count);
    lv_obj_clear_flag(ui_PanelCanvasPopupIntervalTimerWarningEnd, LV_OBJ_FLAG_HIDDEN);

    if(!lv_additional_photo_timer){
        lv_additional_photo_timer = lv_timer_create(pop_up_additional_photo_callback, 5000, ui_PanelCanvasPopupIntervalTimerWarningEnd);
    }
}

/**
 * @brief Show SD space warning popup
 */
void ui_extra_popup_camera_sd_space_warning(void)
{
    ui_extra_clear_page();
    lv_obj_clear_flag(ui_PanelCanvasPopupSDSpaceWarning, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief Hide SD space warning popup
 */
void ui_extra_popup_camera_sd_space_warning_end(void)
{
    if(!lv_obj_has_flag(ui_PanelCanvasPopupSDSpaceWarning, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(ui_PanelCanvasPopupSDSpaceWarning, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Handle USB disk page
 * @return Whether USB disk page is handled
 */
bool ui_extra_handle_usb_disk_page(void)
{
    if(ui_extra_get_current_page() == UI_PAGE_USB_DISK) {
        if(!lv_obj_has_flag(ui_ImageScreenUSB, LV_OBJ_FLAG_HIDDEN)) {
            ui_extra_goto_page(UI_PAGE_MAIN);
        } else if (!lv_obj_has_flag(ui_ImageScreenUSBSuccess, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(ui_ImageScreenUSBWarning, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_ImageScreenUSB, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_ImageScreenUSBSuccess, LV_OBJ_FLAG_HIDDEN);

            if (!lv_usb_disk_timer) {
                lv_usb_disk_timer = lv_timer_create(usb_disk_timer_callback, 5000, NULL);
            }
        }
        return true;  
    }
    return false;
}

/**
 * @brief Show picture delete warning popup
 */
void ui_extra_popup_picture_delete_warning(void)
{
    lv_obj_clear_flag(ui_PanelImageScreenAlbumDelete, LV_OBJ_FLAG_HIDDEN);
    ui_extra_focus_on_picture_delete();
}

/**
 * @brief Show picture delete success popup
 */
void ui_extra_popup_picture_delete_success(void)
{
    lv_obj_add_flag(ui_PanelImageScreenAlbumDelete, LV_OBJ_FLAG_HIDDEN);
}

/*-------------------------------------------------*/
/* Button Event Handlers                           */
/*-------------------------------------------------*/

/**
 * @brief Up button handler
 */
void ui_extra_btn_up(void)
{
    // Check if there are any popup windows that need to be cleared
    if(!lv_obj_has_flag(ui_PanelCanvasPopupCamera, LV_OBJ_FLAG_HIDDEN) || 
       !lv_obj_has_flag(ui_PanelCanvasPopupCameraInterval, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupVideoMode, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupSDWarning, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarning, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarningEnd, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupAICamera, LV_OBJ_FLAG_HIDDEN)) {
        
        return;
    }

    switch(current_page) {
        case UI_PAGE_MAIN:
            lv_obj_scroll_by(scroll_cont, 0, 40, LV_ANIM_ON);
            lv_event_send(scroll_cont, LV_EVENT_SCROLL, NULL);
            lv_obj_add_flag(info_label, LV_OBJ_FLAG_HIDDEN);
            break;
            
        case UI_PAGE_SETTINGS:
            if (is_camera_settings_panel_active) {
                // camera settings panel is active
                if(current_camera_settings_item == 0) {
                    // if current is the first camera settings item, switch back to main settings panel when up button is pressed
                    switch_to_main_settings_panel();
                    // focus on flash setting item
                    update_settings_focus(3); // index of flash setting item
                } else if(current_camera_settings_item > 0) {
                    update_camera_settings_focus(current_camera_settings_item - 1);
                }
            } else {
                // main settings panel is active
                if(current_settings_item > 0) {
                    update_settings_focus(current_settings_item - 1);
                }
            }
            break;
            
        case UI_PAGE_CAMERA:
        case UI_PAGE_VIDEO_MODE:
            app_extra_set_magnification_factor(2);
            break;
            
        case UI_PAGE_INTERVAL_CAM:
            app_extra_set_interval_time(interval_time + INTERVAL_TIME_STEP);

            if(lv_obj_has_flag(ui_PanelInrervalTimePrompt, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_PanelInrervalTimePrompt, LV_OBJ_FLAG_HIDDEN);
                if(!lv_interval_timer) {
                    lv_interval_timer = lv_timer_create(interval_timer_callback, 3000, NULL);
                }
            }

            if (lv_interval_timer) {
                lv_timer_reset(lv_interval_timer);
            }
            break;

        case UI_PAGE_ALBUM:
            if(!lv_obj_has_flag(ui_PanelImageScreenAlbumDelete, LV_OBJ_FLAG_HIDDEN)) {
                ui_extra_focus_on_picture_delete();
            }
            break;
            
        case UI_PAGE_AI_DETECT:
            // Switch to previous AI detection mode (cycle back to the last if at first)
            if (current_ai_detect_mode == AI_DETECT_PEDESTRIAN) {
                ui_extra_change_ai_detect_mode(AI_DETECT_FACE);
            } else {
                ui_extra_change_ai_detect_mode(AI_DETECT_PEDESTRIAN);
            }
            break;

        case UI_PAGE_GIFS:
            if (gifs_delete_modal_open()) {
                /* Modal is up — up/down toggles YES/NO focus, same as the
                 * album's delete modal behavior. */
                gifs_delete_modal_toggle_focus();
            } else {
                app_gifs_prev();
                /* Auto-play the just-selected entry (GIF or JPEG preview).
                 * No "press to play" step — navigation is preview-as-you-go,
                 * like scrubbing through videos in a gallery. */
                if (app_gifs_get_count() > 0) app_gifs_play_current();
            }
            break;

        default:
            break;
    }
}

/**
 * @brief Down button handler
 */
void ui_extra_btn_down(void)
{
    // Check if there are any popup windows that need to be cleared
    if(!lv_obj_has_flag(ui_PanelCanvasPopupCamera, LV_OBJ_FLAG_HIDDEN) || 
       !lv_obj_has_flag(ui_PanelCanvasPopupCameraInterval, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupVideoMode, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupSDWarning, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarning, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarningEnd, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupAICamera, LV_OBJ_FLAG_HIDDEN)) {
        
        return;
    }

    switch(current_page) {
        case UI_PAGE_MAIN:
            lv_obj_scroll_by(scroll_cont, 0, -40, LV_ANIM_ON);
            lv_event_send(scroll_cont, LV_EVENT_SCROLL, NULL);
            lv_obj_add_flag(info_label, LV_OBJ_FLAG_HIDDEN);
            break;
            
        case UI_PAGE_SETTINGS:
            if (is_camera_settings_panel_active) {
                // camera settings panel is active
                if(current_camera_settings_item < 4) {
                    update_camera_settings_focus(current_camera_settings_item + 1);
                }
            } else {
                // main settings panel is active
                if(current_settings_item == 3 && settings_items[current_settings_item] == ui_PanelPanelSettingsFlash) {
                    // if current selected item is flash setting item, switch to camera settings panel when down button is pressed
                    switch_to_camera_settings_panel();
                } else if(current_settings_item < 4) {
                    update_settings_focus(current_settings_item + 1);
                }
            }
            break;
            
        case UI_PAGE_CAMERA:
        case UI_PAGE_VIDEO_MODE:
            app_extra_set_magnification_factor(3);
            break;
            
        case UI_PAGE_INTERVAL_CAM:
            app_extra_set_interval_time(interval_time - INTERVAL_TIME_STEP);
            
            if(lv_obj_has_flag(ui_PanelInrervalTimePrompt, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_PanelInrervalTimePrompt, LV_OBJ_FLAG_HIDDEN);
                if(!lv_interval_timer) {
                    lv_interval_timer = lv_timer_create(interval_timer_callback, 3000, NULL);
                }
            }

            if(lv_interval_timer) {
                lv_timer_reset(lv_interval_timer);
            }
            break;

        case UI_PAGE_ALBUM:
            if(!lv_obj_has_flag(ui_PanelImageScreenAlbumDelete, LV_OBJ_FLAG_HIDDEN)) {
                ui_extra_focus_on_picture_delete();
            }
            break;
            
        case UI_PAGE_AI_DETECT:
            // Switch to next AI detection mode (cycle to the first if at last)
            if (current_ai_detect_mode == AI_DETECT_FACE) {
                ui_extra_change_ai_detect_mode(AI_DETECT_PEDESTRIAN);
            } else {
                ui_extra_change_ai_detect_mode(AI_DETECT_FACE);
            }
            break;

        case UI_PAGE_GIFS:
            if (gifs_delete_modal_open()) {
                gifs_delete_modal_toggle_focus();
            } else {
                app_gifs_next();
                /* Auto-play the just-selected entry — see btn_up comment. */
                if (app_gifs_get_count() > 0) app_gifs_play_current();
            }
            break;

        default:
            break;
    }
}

/**
 * @brief Right button handler
 */
void ui_extra_btn_right(void)
{
    switch(current_page) {
        case UI_PAGE_SETTINGS:
            if (is_camera_settings_panel_active && current_camera_settings_item < 4) {
                // camera settings panel is active and current selected item is slider item
                lv_obj_t *slider = NULL;
                uint32_t *value_ptr = NULL;
                
                // get current slider and value pointer
                switch(current_camera_settings_item) {
                    case 0: // contrast
                        slider = ui_SliderPanelPanelSettingsContrast;
                        value_ptr = &contrast_percent;
                        break;
                    case 1: // saturation
                        slider = ui_SliderPanelPanelSettingsSaturation;
                        value_ptr = &saturation_percent;
                        break;
                    case 2: // brightness
                        slider = ui_SliderPanelPanelSettingsBrightness;
                        value_ptr = &brightness_percent;
                        break;
                    case 3: // hue
                        slider = ui_SliderPanelPanelSettingsHue;
                        value_ptr = &hue_percent;
                        break;
                }
                
                if(slider && value_ptr) {
                    // reduce value (minimum is 0)
                    if(*value_ptr >= 5) {
                        *value_ptr -= 5;
                    } else {
                        *value_ptr = 0;
                    }
                    
                    // update slider display
                    lv_slider_set_value(slider, *value_ptr, LV_ANIM_ON);
                    
                    // apply settings
                    switch(current_camera_settings_item) {
                        case 0: app_isp_set_contrast(*value_ptr); break;
                        case 1: app_isp_set_saturation(*value_ptr); break;
                        case 2: app_isp_set_brightness(*value_ptr); break;
                        case 3: app_isp_set_hue(*value_ptr); break;
                    }
                    
                    // save settings
                    save_camera_settings();
                }
            }
            break;
        default:
            break;
    }
}

/**
 * @brief Left button handler
 */
void ui_extra_btn_left(void)
{
    switch(current_page) {
        case UI_PAGE_SETTINGS:
            if (is_camera_settings_panel_active && current_camera_settings_item < 4) {
                // camera settings panel is active and current selected item is slider item
                lv_obj_t *slider = NULL;
                uint32_t *value_ptr = NULL;
                
                // get current slider and value pointer
                switch(current_camera_settings_item) {
                    case 0: // contrast
                        slider = ui_SliderPanelPanelSettingsContrast;
                        value_ptr = &contrast_percent;
                        break;
                    case 1: // saturation
                        slider = ui_SliderPanelPanelSettingsSaturation;
                        value_ptr = &saturation_percent;
                        break;
                    case 2: // brightness
                        slider = ui_SliderPanelPanelSettingsBrightness;
                        value_ptr = &brightness_percent;
                        break;
                    case 3: // hue
                        slider = ui_SliderPanelPanelSettingsHue;
                        value_ptr = &hue_percent;
                        break;
                }
                
                if(slider && value_ptr) {
                    // increase value (maximum is 100)
                    if(*value_ptr <= 95) {
                        *value_ptr += 5;
                    } else {
                        *value_ptr = 100;
                    }
                    
                    // update slider display
                    lv_slider_set_value(slider, *value_ptr, LV_ANIM_ON);
                    
                    // apply settings
                    switch(current_camera_settings_item) {
                        case 0: app_isp_set_contrast(*value_ptr); break;
                        case 1: app_isp_set_saturation(*value_ptr); break;
                        case 2: app_isp_set_brightness(*value_ptr); break;
                        case 3: app_isp_set_hue(*value_ptr); break;
                    }
                    
                    // save settings
                    save_camera_settings();
                }
            }
            break;
        default:
            break;
    }
}

/**
 * @brief Menu button handler
 */
void ui_extra_btn_menu(void)
{
    if(!lv_obj_has_flag(ui_PanelCanvasPopupSDWarning, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    // Check if there are any popup windows that need to be cleared
    if(!lv_obj_has_flag(ui_PanelCanvasPopupCamera, LV_OBJ_FLAG_HIDDEN) || 
       !lv_obj_has_flag(ui_PanelCanvasPopupCameraInterval, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupVideoMode, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupSDWarning, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarning, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarningEnd, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupAICamera, LV_OBJ_FLAG_HIDDEN)) {
        
        ui_extra_clear_popup_window();
        return;
    }

    // Ignore selection during scroll
    if (is_scrolling && current_page == UI_PAGE_MAIN) {
        ESP_LOGI(TAG, "Ignoring selection during scroll");
        return;
    }

    // Perform different actions based on current page
    switch(current_page) {
        case UI_PAGE_MAIN:
            ui_extra_goto_page(ui_extra_get_choosed_page());
            break;
            
        case UI_PAGE_SETTINGS:
            if(is_camera_settings_panel_active && current_camera_settings_item == 4 && camera_settings_items[current_camera_settings_item] == ui_PanelSettingsMenu) {
                // If current setting item is menu item, return to main page
                ui_extra_goto_page(UI_PAGE_MAIN);

                // Apply gyroscope setting
                app_storage_save_gyroscope_setting(strcmp(current_settings.gyroscope, "On") == 0);

                if(strcmp(current_settings.flash, "On") == 0) {
                    app_video_stream_set_flash_light(true);
                } else {
                    app_video_stream_set_flash_light(false);
                }

                app_album_enable_coco_od(strcmp(current_settings.od, "On") == 0);

                lv_obj_add_flag(ui_PanelCameraSettings, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(ui_PanelSettings, LV_OBJ_FLAG_HIDDEN);

                app_video_stream_set_photo_resolution_by_string(current_settings.resolution);
            } else {
                // Otherwise, cycle through options
                setting_options_t* opt = &settings_options[current_settings_item];
                opt->current_option = (opt->current_option + 1) % opt->option_count;
                update_setting_display(current_settings_item);
            }
            break;
            
        case UI_PAGE_ALBUM:
            if(!lv_obj_has_flag(ui_PanelImageScreenAlbumDelete, LV_OBJ_FLAG_HIDDEN)) {
                if(lv_obj_has_state(ui_ButtonPanelImageScreenAlbumDeleteYES, LV_STATE_FOCUSED)) {
                    app_album_delete_current_image();
                    ui_extra_popup_picture_delete_success();
                } else {
                    ui_extra_popup_picture_delete_success();
                }
            } else {
                ui_extra_goto_page(UI_PAGE_MAIN);
            }
            break;
        case UI_PAGE_VIDEO_MODE:
            if(lv_obj_has_flag(ui_ImageRedDot, LV_OBJ_FLAG_HIDDEN)) {
                ui_extra_goto_page(UI_PAGE_MAIN); 
            } else {
                app_video_stream_stop_take_video();

                lv_obj_add_flag(ui_ImageRedDot, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(ui_LabelRedDotTime, LV_OBJ_FLAG_HIDDEN);
                
                // Stop the video recording and pause the timer
                is_video_recording = false;
                if (lv_video_timer) {
                    lv_timer_pause(lv_video_timer);
                }
            }
            break;    
        
        case UI_PAGE_GIFS:
            /* Menu button on the gallery, mirroring the album's two-button
             * confirm model:
             *   - modal CLOSED → exit to main (standard "back" semantics)
             *   - modal OPEN   → act as the SELECTOR: YES deletes,
             *                    NO cancels and closes the modal
             *
             * The encoder / trigger button is the one that OPENS the
             * modal (see btn_encoder below). Either button can then
             * confirm the selection — same as album. */
            if (gifs_delete_modal_open()) {
                if (gifs_delete_modal_yes_focused()) {
                    app_gifs_delete_current();
                }
                gifs_delete_modal_hide();
                if (app_gifs_get_count() > 0) {
                    app_gifs_play_current();
                }
            } else {
                app_gifs_stop();
                ui_extra_goto_page(UI_PAGE_MAIN);
            }
            break;

        default:
            // For other pages, return to main page
            ui_extra_goto_page(UI_PAGE_MAIN);
            ui_extra_popup_camera_sd_space_warning_end();
            if(lv_interval_timer) {
                lv_timer_ready(lv_interval_timer);
            }

            // Hide AI mode label when leaving AI detect page
            if (current_page == UI_PAGE_AI_DETECT && ai_mode_label != NULL) {
                lv_obj_add_flag(ai_mode_label, LV_OBJ_FLAG_HIDDEN);
            }
            break;
    }
}

/**
 * @brief Encoder button handler
 */
void ui_extra_btn_encoder(void)
{
    if(!lv_obj_has_flag(ui_PanelCanvasPopupCamera, LV_OBJ_FLAG_HIDDEN) || 
       !lv_obj_has_flag(ui_PanelCanvasPopupCameraInterval, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupVideoMode, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarning, LV_OBJ_FLAG_HIDDEN) ||
       !lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarningEnd, LV_OBJ_FLAG_HIDDEN)) {

        ui_extra_clear_popup_window();
        return;
    }

    if(!lv_obj_has_flag(ui_PanelCanvasPopupSDWarning, LV_OBJ_FLAG_HIDDEN)) {
        switch(current_page) {
            case UI_PAGE_CAMERA:
                ui_extra_goto_page(UI_PAGE_CAMERA);
                break;
            case UI_PAGE_INTERVAL_CAM:
                ui_extra_goto_page(UI_PAGE_INTERVAL_CAM);
                break;
            case UI_PAGE_VIDEO_MODE:
                ui_extra_goto_page(UI_PAGE_VIDEO_MODE);
                break;
            default:
                break;
        }
       
        ui_extra_clear_popup_window();
        return;
    }

    if(!lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarning, LV_OBJ_FLAG_HIDDEN) || 
       !lv_obj_has_flag(ui_PanelCanvasPopupIntervalTimerWarningEnd, LV_OBJ_FLAG_HIDDEN)) {
        
        ui_extra_goto_page(UI_PAGE_INTERVAL_CAM);
        ui_extra_clear_popup_window();
        return;
    }

    if(!is_sd_card_mounted) {
        switch(current_page) {
            case UI_PAGE_CAMERA:
            case UI_PAGE_INTERVAL_CAM:
            case UI_PAGE_VIDEO_MODE:
                ui_extra_clear_page();
                lv_obj_clear_flag(ui_PanelCanvasPopupSDWarning, LV_OBJ_FLAG_HIDDEN);
                break;
            case UI_PAGE_MAIN:
                ui_extra_btn_menu();
                break;
            default:
                break;
        }
        return;
    }

    // Ignore selection during scroll
    if (is_scrolling && current_page == UI_PAGE_MAIN) {
        ESP_LOGI(TAG, "Ignoring selection during scroll");
        return;
    }

    switch(current_page) {
        case UI_PAGE_MAIN:
            ui_extra_btn_menu();
            break;
        case UI_PAGE_INTERVAL_CAM:
            if(lv_interval_timer) {
                lv_timer_ready(lv_interval_timer);
                vTaskDelay(300 / portTICK_PERIOD_MS);
            }
            
            ui_extra_start_interval_timer();
            
            if(!lv_additional_photo_timer){
                lv_additional_photo_timer = lv_timer_create(pop_up_additional_photo_callback, 7000, ui_PanelCanvasPopupIntervalTimerWarning);
            }
            break;
        case UI_PAGE_CAMERA:
            // Check if we can store a new image
            if (!app_album_can_store_new_image()) {
                // Show warning to user that storage is full or low
                ESP_LOGE(TAG, "Cannot store more images");
                ui_extra_popup_camera_sd_space_warning();
                return;
            } else {
                ui_extra_popup_camera_sd_space_warning_end();
            }

            app_video_stream_take_photo();
            break;
        case UI_PAGE_ALBUM:
            if(lv_obj_has_flag(ui_PanelImageScreenAlbumDelete, LV_OBJ_FLAG_HIDDEN)) {
                ui_extra_popup_picture_delete_warning();
                ui_extra_focus_on_picture_delete();
            } else {
                if(lv_obj_has_state(ui_ButtonPanelImageScreenAlbumDeleteYES, LV_STATE_FOCUSED)) {
                    app_album_delete_current_image();
                    ui_extra_popup_picture_delete_success();
                } else {
                    ui_extra_popup_picture_delete_success();
                }
            }
            break;
        case UI_PAGE_VIDEO_MODE:
            if(lv_obj_has_flag(ui_ImageRedDot, LV_OBJ_FLAG_HIDDEN)) {

                // Check if we can store a new image
                if (!app_video_stream_can_store_new_mp4(100)) {
                    // Show warning to user that storage is full or low
                    ui_extra_popup_camera_sd_space_warning();
                    ESP_LOGE(TAG, "Cannot store more mp4 videos");
                    return;
                } else {
                    ui_extra_popup_camera_sd_space_warning_end();
                }

                app_video_stream_take_video();

                lv_obj_clear_flag(ui_ImageRedDot, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(ui_LabelRedDotTime, LV_OBJ_FLAG_HIDDEN);
                
                // Reset the video recording seconds and start the timer
                video_recording_seconds = 0;
                lv_label_set_text(ui_LabelRedDotTime, "00:00");
                is_video_recording = true;
                
                if (!lv_video_timer) {
                    lv_video_timer = lv_timer_create(video_timer_callback, 1000, NULL);
                } else {
                    lv_timer_resume(lv_video_timer);
                }
            } else {
                app_video_stream_stop_take_video();

                lv_obj_add_flag(ui_ImageRedDot, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(ui_LabelRedDotTime, LV_OBJ_FLAG_HIDDEN);
                
                // Stop the video recording and pause the timer
                is_video_recording = false;
                if (lv_video_timer) {
                    lv_timer_pause(lv_video_timer);
                }
            }
            break;

        case UI_PAGE_GIFS:
            /* Encoder/trigger on the gallery: same button that takes
             * photos in the camera app. First press opens the delete
             * modal; second press confirms (YES → delete, NO → cancel).
             * The menu button is the always-available exit. Empty
             * gallery → nothing to delete, no-op. */
            if (app_gifs_get_count() == 0) break;

            if (!gifs_delete_modal_open()) {
                app_gifs_stop();        /* pause playback while modal visible */
                gifs_delete_modal_show();
            } else {
                bool did_delete = gifs_delete_modal_yes_focused();
                if (did_delete) {
                    app_gifs_delete_current();
                }
                gifs_delete_modal_hide();
                if (app_gifs_get_count() > 0) {
                    app_gifs_play_current();
                }
            }
            break;

        default:
            break;
    }
}

/**
 * @brief Initialize the UI extra module
 */
void ui_extra_init(void)
{
    ui_init();

    init_settings_options();

    lv_scroll_create();

    /* Create the opaque white home-screen overlay. Sized to cover the entire
     * 240x240 camera canvas. Children (scroll/info/arrows) stay on top since
     * they're siblings under ui_ScreenCamera and were registered first. */
    ui_PanelHomeBackground = lv_obj_create(ui_ScreenCamera);
    lv_obj_set_size(ui_PanelHomeBackground, 240, 240);
    lv_obj_set_align(ui_PanelHomeBackground, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ui_PanelHomeBackground, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_PanelHomeBackground, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_PanelHomeBackground, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_PanelHomeBackground, 0,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_PanelHomeBackground, 0,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    /* Pull it to the back so the scroll UI (created above) floats on top */
    lv_obj_move_background(ui_PanelHomeBackground);
    lv_obj_add_flag(ui_PanelHomeBackground, LV_OBJ_FLAG_HIDDEN);

    /* "saving..." overlay shown on the camera screen whenever the PIMSLO
     * capture task is busy. A 300 ms LVGL timer polls app_pimslo_is_capturing
     * and shows / hides the label, also animating the trailing dots. */
    saving_label = lv_label_create(ui_ScreenCamera);
    lv_obj_set_style_bg_color(saving_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(saving_label, LV_OPA_70, 0);
    lv_obj_set_style_text_color(saving_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(saving_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_all(saving_label, 10, 0);
    lv_obj_set_style_radius(saving_label, 6, 0);
    lv_obj_align(saving_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(saving_label, "saving");
    lv_obj_add_flag(saving_label, LV_OBJ_FLAG_HIDDEN);
    /* Poll every 100 ms so the overlay appears immediately when the
     * capture flag flips and animates smoothly (was 300 ms — user
     * noticed the lag on trigger press). The label update itself is
     * cheap; the real work comes from what the capture task is doing
     * on Core 0. */
    saving_timer = lv_timer_create(saving_timer_cb, 100, NULL);

    /* The GIFs "Delete?" modal is created lazily in
     * ui_extra_redirect_to_gifs_page() the first time the gallery page is
     * entered — its parent ui_ScreenGifs is a lazy-init SquareLine screen
     * that doesn't exist yet at ui_extra_init time. */

    // reset flag
    is_camera_settings_panel_active = false;

    // Initialize AI detection mode
    current_ai_detect_mode = AI_DETECT_PEDESTRIAN;
    ai_mode_label = NULL;

    // load settings
    settings_info_t settings;
    uint16_t loaded_interval_time;
    uint16_t loaded_magnification;
    uint32_t loaded_contrast;
    uint32_t loaded_saturation;
    uint32_t loaded_brightness;
    uint32_t loaded_hue;
    
    // Set default values
    settings.gyroscope = gyroscope_options[0];     // Set to "Off"
    settings.od = od_options[1];           // Set to "On"
    settings.resolution = resolution_options[1];  // Set to "1080P"
    settings.flash = flash_options[1];     // Set to "On"

    loaded_interval_time = DEFAULT_INTERVAL_TIME;
    loaded_magnification = DEFAULT_MAGNIFICATION_FACTOR;

    loaded_contrast = DEFAULT_CONTRAST_PERCENT;
    loaded_saturation = DEFAULT_SATURATION_PERCENT;
    loaded_brightness = DEFAULT_BRIGHTNESS_PERCENT;
    loaded_hue = DEFAULT_HUE_PERCENT;
    
    // Load settings from NVS
    esp_err_t err = app_storage_load_settings(&settings, &loaded_interval_time, &loaded_magnification);
    ESP_LOGD(TAG, "loaded_interval_time: %d, loaded_magnification: %d", loaded_interval_time, loaded_magnification);
    if (err == ESP_OK) {
        // Apply loaded settings
        // Update gyroscope settings
        for (int i = 0; i < settings_options[0].option_count; i++) {
            if (strcmp(settings.gyroscope, settings_options[0].options[i]) == 0) {
                settings_options[0].current_option = i;
                break;
            }
        }
        
        // Update od settings
        for (int i = 0; i < settings_options[1].option_count; i++) {
            if (strcmp(settings.od, settings_options[1].options[i]) == 0) {
                settings_options[1].current_option = i;
                break;
            }
        }
        
        // Update resolution settings
        for (int i = 0; i < settings_options[2].option_count; i++) {
            if (strcmp(settings.resolution, settings_options[2].options[i]) == 0) {
                settings_options[2].current_option = i;
                break;
            }
        }
        
        // Update flash settings
        for (int i = 0; i < settings_options[3].option_count; i++) {
            if (strcmp(settings.flash, settings_options[3].options[i]) == 0) {
                settings_options[3].current_option = i;
                break;
            }
        }
        
        // Update current settings
        current_settings.gyroscope = settings.gyroscope;
        current_settings.od = settings.od;
        current_settings.resolution = settings.resolution;
        current_settings.flash = settings.flash;

        if(strcmp(current_settings.flash, "On") == 0) {
            app_video_stream_set_flash_light(true);
        } else {
            app_video_stream_set_flash_light(false);
        }

        app_album_enable_coco_od(strcmp(current_settings.od, "On") == 0);

        app_video_stream_set_photo_resolution_by_string(current_settings.resolution);
        
        // Update interval time and magnification
        interval_time = loaded_interval_time;
        magnification_factor = loaded_magnification;
        
        is_video_recording = false;
        video_recording_seconds = 0;

        // Update the display
        lv_label_set_text_fmt(ui_LabelCanvasFactor, "%dX", magnification_factor);
        lv_label_set_text_fmt(ui_LabelCanvasInvervalTime, "%dmin", interval_time);

        // Update display
        init_settings_display();
    } else {
        // First time boot or NVS error, use default values
        ESP_LOGW(TAG, "Failed to load settings from NVS (%s), using default values", esp_err_to_name(err));
        
        // Update current settings with default values
        current_settings.gyroscope = gyroscope_options[0];     // Set to "Off"
        current_settings.od = od_options[1];           // Set to "On"
        current_settings.resolution = resolution_options[1];  // Set to "1080P"
        current_settings.flash = flash_options[1];     // Set to "On"

        if(strcmp(current_settings.flash, "On") == 0) {
            app_video_stream_set_flash_light(true);
        } else {
            app_video_stream_set_flash_light(false);
        }

        app_album_enable_coco_od(strcmp(current_settings.od, "On") == 0);

        app_video_stream_set_photo_resolution_by_string(current_settings.resolution);
        
        // Use default interval time and magnification
        interval_time = loaded_interval_time;
        magnification_factor = loaded_magnification;
        
        is_video_recording = false;
        video_recording_seconds = 0;

        // Update the display
        lv_label_set_text_fmt(ui_LabelCanvasFactor, "%dX", magnification_factor);
        lv_label_set_text_fmt(ui_LabelCanvasInvervalTime, "%dmin", interval_time);

        // Update display
        init_settings_display();
        
        // Save default settings to NVS for next boot
        save_current_settings();
    }

    // Load camera settings
    err = app_storage_load_camera_settings(&loaded_contrast, &loaded_saturation, 
                                          &loaded_brightness, &loaded_hue);
    if (err == ESP_OK) {
        // Update camera settings
        contrast_percent = loaded_contrast;
        saturation_percent = loaded_saturation;
        brightness_percent = loaded_brightness;
        hue_percent = loaded_hue;
    }
    init_camera_settings_display();

    // Redirect to the main page
    ui_extra_goto_page(UI_PAGE_MAIN);

    is_ui_init = true;
}