/**
 * @file lv_conf.h
 * @brief LVGL configuration for the ESP32-P4X-EYE SDL2 simulator
 *
 * Matches hardware settings: 240x240 RGB565 display
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH     16
#define LV_COLOR_16_SWAP   1

/*====================
   MEMORY SETTINGS
 *====================*/
#define LV_MEM_CUSTOM      1
#if LV_MEM_CUSTOM == 1
    #define LV_MEM_CUSTOM_INCLUDE   <stdlib.h>
    #define LV_MEM_CUSTOM_ALLOC     malloc
    #define LV_MEM_CUSTOM_FREE      free
    #define LV_MEM_CUSTOM_REALLOC   realloc
#endif

/*====================
   HAL SETTINGS
 *====================*/
#define LV_TICK_CUSTOM     1
#if LV_TICK_CUSTOM
    #define LV_TICK_CUSTOM_INCLUDE  <SDL2/SDL.h>
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR  (SDL_GetTicks())
#endif

/*====================
   DISPLAY SETTINGS
 *====================*/
#define LV_HOR_RES_MAX          240
#define LV_VER_RES_MAX          240
#define LV_DPI_DEF              130

/*====================
   FEATURE CONFIG
 *====================*/
#define LV_USE_LOG              1
#if LV_USE_LOG
    #define LV_LOG_LEVEL        LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF       1
#endif

#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

/*====================
   FONT USAGE
 *====================*/
#define LV_FONT_MONTSERRAT_8    0
#define LV_FONT_MONTSERRAT_10   0
#define LV_FONT_MONTSERRAT_12   0
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_18   1
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_22   0
#define LV_FONT_MONTSERRAT_24   0
#define LV_FONT_MONTSERRAT_26   0
#define LV_FONT_MONTSERRAT_28   0
#define LV_FONT_MONTSERRAT_30   0
#define LV_FONT_MONTSERRAT_32   0
#define LV_FONT_MONTSERRAT_34   0
#define LV_FONT_MONTSERRAT_36   0
#define LV_FONT_MONTSERRAT_38   0
#define LV_FONT_MONTSERRAT_40   0
#define LV_FONT_MONTSERRAT_42   0
#define LV_FONT_MONTSERRAT_44   0
#define LV_FONT_MONTSERRAT_46   0
#define LV_FONT_MONTSERRAT_48   0

#define LV_FONT_MONTSERRAT_12_SUBPX      0
#define LV_FONT_MONTSERRAT_28_COMPRESSED  0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW  0
#define LV_FONT_SIMSUN_16_CJK            0
#define LV_FONT_UNSCII_8                 0
#define LV_FONT_UNSCII_16               0

#define LV_FONT_DEFAULT    &lv_font_montserrat_14

#define LV_FONT_FMT_TXT_LARGE  0
#define LV_USE_FONT_COMPRESSED  0
#define LV_USE_FONT_SUBPX      0

/*====================
   TEXT SETTINGS
 *====================*/
#define LV_TXT_ENC             LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS     " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN  0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN  3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3
#define LV_TXT_COLOR_CMD       "#"

/*====================
   WIDGET USAGE
 *====================*/
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     1
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1
#define LV_USE_TABLE      1

/*====================
   EXTRA WIDGETS
 *====================*/
#define LV_USE_ANIMIMG    1
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     1
#define LV_USE_KEYBOARD   1
#define LV_USE_LED        0
#define LV_USE_LIST       0
#define LV_USE_MENU       0
#define LV_USE_METER      0
#define LV_USE_MSGBOX     1
#define LV_USE_SPAN       0
#define LV_USE_SPINBOX    1
#define LV_USE_SPINNER    0
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0

/*====================
   THEMES
 *====================*/
#define LV_USE_THEME_DEFAULT    1
#define LV_USE_THEME_BASIC      1

/*====================
   LAYOUTS
 *====================*/
#define LV_USE_FLEX     1
#define LV_USE_GRID     1

/*====================
   IMAGE CACHE
 *====================*/
#define LV_IMG_CACHE_DEF_SIZE   5

/*====================
   DRAW
 *====================*/
#define LV_USE_DRAW_MASKS  1
#define LV_DRAW_COMPLEX    1

/*====================
   GPU
 *====================*/
#define LV_USE_GPU_STM32_DMA2D  0
#define LV_USE_GPU_NXP_PXP      0
#define LV_USE_GPU_NXP_VG_LITE  0
#define LV_USE_GPU_SDL          0

/*====================
   MISC
 *====================*/
#define LV_USE_SNAPSHOT   0
#define LV_BUILD_EXAMPLES 0

#define LV_SPRINTF_CUSTOM 0
#define LV_USE_PERF_MONITOR   0
#define LV_USE_MEM_MONITOR    0

#endif /*LV_CONF_H*/
