/**
 * @file ui_ScreenGifs.c
 * @brief GIFs screen — displays and plays GIF animations
 *
 * Follows the ui_ScreenAlbum pattern: canvas for frame display,
 * menu/up/down buttons on the right side, SD card warning panel.
 */

#include "../ui.h"

void ui_ScreenGifs_screen_init(void)
{
    ui_ScreenGifs = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenGifs, LV_OBJ_FLAG_SCROLLABLE);

    /* Main canvas for GIF frame display (240x240) */
    ui_ImageScreenGifs = lv_canvas_create(ui_ScreenGifs);
    lv_obj_set_width(ui_ImageScreenGifs, 240);
    lv_obj_set_height(ui_ImageScreenGifs, 240);
    lv_obj_set_align(ui_ImageScreenGifs, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_ImageScreenGifs, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_ImageScreenGifs, LV_OBJ_FLAG_SCROLLABLE);

    /* Menu button (right side, top) */
    ui_ImageGifsMenu = lv_img_create(ui_ImageScreenGifs);
    lv_img_set_src(ui_ImageGifsMenu, &ui_img_button_menu_png);
    lv_obj_set_width(ui_ImageGifsMenu, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_ImageGifsMenu, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_ImageGifsMenu, 100);
    lv_obj_set_y(ui_ImageGifsMenu, -47);
    lv_obj_set_align(ui_ImageGifsMenu, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_ImageGifsMenu, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_ImageGifsMenu, LV_OBJ_FLAG_SCROLLABLE);

    /* Up button (right side, middle) */
    ui_ImageGifsUp = lv_img_create(ui_ImageScreenGifs);
    lv_img_set_src(ui_ImageGifsUp, &ui_img_up_button_png);
    lv_obj_set_width(ui_ImageGifsUp, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_ImageGifsUp, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_ImageGifsUp, 100);
    lv_obj_set_y(ui_ImageGifsUp, 0);
    lv_obj_set_align(ui_ImageGifsUp, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_ImageGifsUp, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_ImageGifsUp, LV_OBJ_FLAG_SCROLLABLE);

    /* Down button (right side, bottom) */
    ui_ImageGifsDown = lv_img_create(ui_ImageScreenGifs);
    lv_img_set_src(ui_ImageGifsDown, &ui_img_button_down_png);
    lv_obj_set_width(ui_ImageGifsDown, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_ImageGifsDown, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_ImageGifsDown, 100);
    lv_obj_set_y(ui_ImageGifsDown, 55);
    lv_obj_set_align(ui_ImageGifsDown, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_ImageGifsDown, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_ImageGifsDown, LV_OBJ_FLAG_SCROLLABLE);

    /* SD card warning panel (centered, hidden by default) */
    ui_PanelGifsPopupSDWarning = lv_obj_create(ui_ImageScreenGifs);
    lv_obj_set_width(ui_PanelGifsPopupSDWarning, 181);
    lv_obj_set_height(ui_PanelGifsPopupSDWarning, 140);
    lv_obj_set_align(ui_PanelGifsPopupSDWarning, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_PanelGifsPopupSDWarning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_PanelGifsPopupSDWarning, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_PanelGifsPopupSDWarning, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_PanelGifsPopupSDWarning, lv_color_hex(0xDDDDDD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_PanelGifsPopupSDWarning, 240, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Warning title */
    lv_obj_t *warn_title = lv_label_create(ui_PanelGifsPopupSDWarning);
    lv_label_set_text(warn_title, "Warning");
    lv_obj_set_style_text_color(warn_title, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(warn_title, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(warn_title, LV_ALIGN_TOP_MID);
    lv_obj_set_y(warn_title, 10);

    /* Warning message */
    lv_obj_t *warn_msg = lv_label_create(ui_PanelGifsPopupSDWarning);
    lv_label_set_text(warn_msg, "No SDCard detected\nInsert SDCard to\ncontinue");
    lv_obj_set_style_text_font(warn_msg, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(warn_msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(warn_msg, LV_ALIGN_CENTER);
    lv_obj_set_y(warn_msg, 10);
}
