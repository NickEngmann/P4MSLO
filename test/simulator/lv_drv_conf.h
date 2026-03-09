/**
 * @file lv_drv_conf.h
 * @brief LVGL driver configuration for SDL2 backend
 */

#ifndef LV_DRV_CONF_H
#define LV_DRV_CONF_H

#include "lv_conf.h"

/*-------------------
 *  SDL2 display
 *-------------------*/
#ifndef USE_SDL
    #define USE_SDL 1
#endif

#if USE_SDL
    #define SDL_HOR_RES     240
    #define SDL_VER_RES     240
    #define SDL_ZOOM        3       /* 3x window for desktop visibility */
    #define SDL_INCLUDE_PATH    <SDL2/SDL.h>
    #define SDL_DOUBLE_BUFFERED 0

    /* Use monitor-style fallback names (lv_drivers compat) */
    #define MONITOR_HOR_RES     SDL_HOR_RES
    #define MONITOR_VER_RES     SDL_VER_RES
    #define MONITOR_ZOOM        SDL_ZOOM
    #define MONITOR_SDL_INCLUDE_PATH  SDL_INCLUDE_PATH
#endif

#ifndef USE_SDL_GPU
    #define USE_SDL_GPU 0
#endif

/*-------------------
 *  Monitor (legacy)
 *-------------------*/
#ifndef USE_MONITOR
    #define USE_MONITOR 0
#endif

/*-------------------
 *  Other displays
 *-------------------*/
#ifndef USE_FBDEV
    #define USE_FBDEV 0
#endif

#ifndef USE_DRM
    #define USE_DRM 0
#endif

/*-------------------
 *  Input devices
 *-------------------*/
#ifndef USE_MOUSE
    #define USE_MOUSE 0
#endif

#ifndef USE_MOUSEWHEEL
    #define USE_MOUSEWHEEL 0
#endif

#ifndef USE_KEYBOARD
    #define USE_KEYBOARD 0
#endif

#ifndef USE_EVDEV
    #define USE_EVDEV 0
#endif

#ifndef USE_LIBINPUT
    #define USE_LIBINPUT 0
#endif

#ifndef USE_XKB
    #define USE_XKB 0
#endif

#endif /*LV_DRV_CONF_H*/
