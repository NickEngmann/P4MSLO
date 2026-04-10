/**
 * @file sim_main.c
 * @brief SDL2 main loop for the ESP32-P4X-EYE LVGL simulator
 *
 * Initializes LVGL with the SDL2 display driver, loads the REAL UI code
 * (SquareLine Studio generated + ui_extra state machine), and maps
 * keyboard input to the physical button handlers.
 *
 * Usage:
 *   ./p4eye_sim                  # Interactive SDL2 window
 *   ./p4eye_sim --screenshot     # Headless: run button sequence, dump PPM screenshots, exit
 *
 * Keyboard mapping (interactive mode):
 *   Arrow Up/Down  = Scroll / Navigate
 *   Arrow Left     = Knob left
 *   Arrow Right    = Knob right
 *   Enter/Return   = Encoder press (select)
 *   Escape/M       = Menu button
 *   Space          = Take photo (encoder on camera page)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "lvgl.h"
#include "ui.h"
#include "ui_extra.h"
#include "sim_hal.h"

/* SDL2 — included for both interactive and headless (tick source) */
#include <SDL2/SDL.h>

/* lv_drivers SDL display/input */
#include "lv_drivers/sdl/sdl.h"

/*-------------------------------------------------*/
/* Display dimensions                               */
/*-------------------------------------------------*/
#define DISP_HOR_RES    240
#define DISP_VER_RES    240

/*-------------------------------------------------*/
/* Forward declarations                             */
/*-------------------------------------------------*/
static void setup_display(void);
static void run_interactive(void);
static void run_screenshot_mode(void);
static void save_framebuffer_ppm(const char *filename);

/* Framebuffer for headless screenshot capture */
static lv_color_t s_fb[DISP_HOR_RES * DISP_VER_RES];

/* Flag: true if headless screenshot mode */
static bool s_screenshot_mode = false;

/*-------------------------------------------------*/
/* Headless flush callback (no SDL window)          */
/*-------------------------------------------------*/
static void headless_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    int32_t x, y;
    for (y = area->y1; y <= area->y2; y++) {
        for (x = area->x1; x <= area->x2; x++) {
            s_fb[y * DISP_HOR_RES + x] = *color_p;
            color_p++;
        }
    }
    lv_disp_flush_ready(disp_drv);
}

/*-------------------------------------------------*/
/* Main                                             */
/*-------------------------------------------------*/
int main(int argc, char **argv)
{
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--screenshot") == 0) {
            s_screenshot_mode = true;
        }
    }

    /* Initialize simulated hardware state */
    sim_hal_reset();

    /* Initialize LVGL */
    lv_init();

    if (!s_screenshot_mode) {
        /* Interactive mode: use SDL2 display */
        sdl_init();
        setup_display();
    } else {
        /* Headless mode: SDL needed for tick only, no window */
        SDL_Init(SDL_INIT_TIMER);

        /* Setup display with headless flush */
        static lv_disp_draw_buf_t draw_buf;
        static lv_color_t buf1[DISP_HOR_RES * 10];
        lv_disp_draw_buf_init(&draw_buf, buf1, NULL, DISP_HOR_RES * 10);

        static lv_disp_drv_t disp_drv;
        lv_disp_drv_init(&disp_drv);
        disp_drv.draw_buf  = &draw_buf;
        disp_drv.flush_cb  = headless_flush_cb;
        disp_drv.hor_res   = DISP_HOR_RES;
        disp_drv.ver_res   = DISP_VER_RES;
        lv_disp_drv_register(&disp_drv);
    }

    /* Initialize the REAL UI */
    ui_init();
    ui_extra_init();

    printf("[SIM] UI initialized. Page=%d\n", ui_extra_get_current_page());

    if (s_screenshot_mode) {
        run_screenshot_mode();
    } else {
        printf("[SIM] Interactive mode — keyboard controls:\n");
        printf("  Up/Down    = Navigate\n");
        printf("  Left/Right = Knob\n");
        printf("  Enter      = Select (Encoder)\n");
        printf("  Escape/M   = Menu\n");
        printf("  Q          = Quit\n");
        run_interactive();
    }

    return 0;
}

/*-------------------------------------------------*/
/* Display setup (interactive SDL mode)             */
/*-------------------------------------------------*/
static void setup_display(void)
{
    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[DISP_HOR_RES * DISP_VER_RES];
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, DISP_HOR_RES * DISP_VER_RES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf  = &draw_buf;
    disp_drv.flush_cb  = sdl_display_flush;
    disp_drv.hor_res   = DISP_HOR_RES;
    disp_drv.ver_res   = DISP_VER_RES;
    lv_disp_drv_register(&disp_drv);
}

/*-------------------------------------------------*/
/* Interactive SDL event loop                       */
/*-------------------------------------------------*/
static void run_interactive(void)
{
    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                quit = true;
                break;
            }
            if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                    case SDLK_q:
                        quit = true;
                        break;
                    case SDLK_ESCAPE:
                    case SDLK_m:
                        printf("[KEY] MENU\n");
                        ui_extra_btn_menu();
                        break;
                    case SDLK_UP:
                        printf("[KEY] UP\n");
                        ui_extra_btn_up();
                        break;
                    case SDLK_DOWN:
                        printf("[KEY] DOWN\n");
                        ui_extra_btn_down();
                        break;
                    case SDLK_LEFT:
                        printf("[KEY] LEFT (knob)\n");
                        ui_extra_btn_left();
                        break;
                    case SDLK_RIGHT:
                        printf("[KEY] RIGHT (knob)\n");
                        ui_extra_btn_right();
                        break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                    case SDLK_SPACE:
                        printf("[KEY] ENCODER\n");
                        ui_extra_btn_encoder();
                        break;
                    default:
                        break;
                }
            }
        }
        lv_timer_handler();
        SDL_Delay(5);
    }
}

/*-------------------------------------------------*/
/* Screenshot mode: comprehensive page capture      */
/*-------------------------------------------------*/
static void run_screenshot_mode(void)
{
    int screenshot_num = 0;
    char filename[256];

    /* Create screenshots directory */
    (void)system("mkdir -p screenshots");

    /* Helper: pump LVGL for N milliseconds to let rendering settle */
    #define PUMP_MS(ms) do { \
        uint32_t _start = SDL_GetTicks(); \
        while (SDL_GetTicks() - _start < (uint32_t)(ms)) { \
            lv_timer_handler(); \
            SDL_Delay(1); \
        } \
    } while(0)

    #define SCREENSHOT(label) do { \
        PUMP_MS(300); \
        snprintf(filename, sizeof(filename), "screenshots/%02d_%s.ppm", screenshot_num++, (label)); \
        save_framebuffer_ppm(filename); \
        printf("[SCREENSHOT] %s\n", filename); \
    } while(0)

    /* ===== 1. MAIN MENU — all button icons visible ===== */
    printf("\n[SIM] === Main Menu Screenshots ===\n");
    SCREENSHOT("main_menu_camera_selected");

    /* Scroll through each main menu item */
    ui_extra_btn_down();
    SCREENSHOT("main_menu_item2");

    ui_extra_btn_down();
    SCREENSHOT("main_menu_item3");

    ui_extra_btn_down();
    SCREENSHOT("main_menu_item4");

    ui_extra_btn_down();
    SCREENSHOT("main_menu_item5");

    ui_extra_btn_down();
    SCREENSHOT("main_menu_item6");

    /* Wrap back to top */
    ui_extra_btn_down();
    SCREENSHOT("main_menu_wrap_to_top");

    /* ===== 2. CAMERA PAGE ===== */
    printf("\n[SIM] === Camera Page Screenshots ===\n");
    ui_extra_btn_encoder();
    SCREENSHOT("camera_main");

    /* Camera with popup (down button shows camera popup) */
    ui_extra_btn_down();
    PUMP_MS(200);
    SCREENSHOT("camera_popup_down");

    /* Camera with up button popup */
    ui_extra_btn_up();
    PUMP_MS(200);
    SCREENSHOT("camera_popup_up");

    /* Camera encoder press (take photo / select) */
    ui_extra_btn_encoder();
    PUMP_MS(200);
    SCREENSHOT("camera_encoder_press");

    /* Back to main */
    ui_extra_btn_menu();
    PUMP_MS(200);

    /* ===== 3. ALBUM PAGE ===== */
    printf("\n[SIM] === Album Page Screenshots ===\n");
    /* Navigate to album (item 2 from top — depends on menu order) */
    ui_extra_btn_down();
    PUMP_MS(100);
    ui_extra_btn_down();
    PUMP_MS(100);
    ui_extra_btn_down();
    PUMP_MS(100);
    ui_extra_btn_down();
    PUMP_MS(100);
    ui_extra_btn_encoder();
    SCREENSHOT("album_page_main");

    /* Browse album images */
    ui_extra_btn_down();
    PUMP_MS(200);
    SCREENSHOT("album_next_image");

    ui_extra_btn_down();
    PUMP_MS(200);
    SCREENSHOT("album_next_image2");

    ui_extra_btn_up();
    PUMP_MS(200);
    SCREENSHOT("album_prev_image");

    /* Back to main */
    ui_extra_btn_menu();
    PUMP_MS(200);

    /* ===== 4. SETTINGS PAGE ===== */
    printf("\n[SIM] === Settings Page Screenshots ===\n");
    /* Navigate to settings */
    for (int i = 0; i < 6; i++) {
        ui_extra_btn_down();
        PUMP_MS(100);
    }
    ui_extra_btn_encoder();
    SCREENSHOT("settings_main");

    /* Scroll through all settings items */
    ui_extra_btn_down();
    SCREENSHOT("settings_item2");

    ui_extra_btn_down();
    SCREENSHOT("settings_item3");

    ui_extra_btn_down();
    SCREENSHOT("settings_item4");

    /* Toggle a setting (right button) */
    ui_extra_btn_right();
    PUMP_MS(200);
    SCREENSHOT("settings_toggle_right");

    /* Toggle back (left button) */
    ui_extra_btn_left();
    PUMP_MS(200);
    SCREENSHOT("settings_toggle_left");

    /* Continue scrolling settings */
    ui_extra_btn_down();
    SCREENSHOT("settings_item5");

    ui_extra_btn_down();
    SCREENSHOT("settings_item6");

    /* Encoder press on a settings item */
    ui_extra_btn_encoder();
    PUMP_MS(200);
    SCREENSHOT("settings_encoder_press");

    /* Back to main */
    ui_extra_btn_menu();
    PUMP_MS(200);

    /* ===== 5. VIDEO MODE PAGE ===== */
    printf("\n[SIM] === Video Mode Screenshots ===\n");
    /* Navigate to video mode (scroll to correct menu item) */
    ui_extra_btn_down();
    PUMP_MS(100);
    ui_extra_btn_down();
    PUMP_MS(100);
    ui_extra_btn_encoder();
    PUMP_MS(300);
    SCREENSHOT("video_mode_main");

    /* Video mode interactions */
    ui_extra_btn_encoder();
    PUMP_MS(200);
    SCREENSHOT("video_mode_encoder");

    ui_extra_btn_down();
    PUMP_MS(200);
    SCREENSHOT("video_mode_down");

    ui_extra_btn_up();
    PUMP_MS(200);
    SCREENSHOT("video_mode_up");

    /* Back to main */
    ui_extra_btn_menu();
    PUMP_MS(200);

    /* ===== 6. AI DETECT PAGE ===== */
    printf("\n[SIM] === AI Detection Screenshots ===\n");
    /* Navigate to AI detect */
    ui_extra_btn_down();
    PUMP_MS(100);
    ui_extra_btn_down();
    PUMP_MS(100);
    ui_extra_btn_down();
    PUMP_MS(100);
    ui_extra_btn_encoder();
    PUMP_MS(300);
    SCREENSHOT("ai_detect_main");

    /* Cycle through AI modes */
    ui_extra_btn_encoder();
    PUMP_MS(300);
    SCREENSHOT("ai_detect_mode2");

    ui_extra_btn_encoder();
    PUMP_MS(300);
    SCREENSHOT("ai_detect_mode3");

    /* AI detect with knob (zoom) */
    ui_extra_btn_right();
    PUMP_MS(200);
    SCREENSHOT("ai_detect_knob_right");

    ui_extra_btn_left();
    PUMP_MS(200);
    SCREENSHOT("ai_detect_knob_left");

    /* Back to main */
    ui_extra_btn_menu();
    PUMP_MS(200);

    /* ===== 7. INTERVAL CAMERA PAGE ===== */
    printf("\n[SIM] === Interval Camera Screenshots ===\n");
    /* Navigate to interval camera */
    ui_extra_btn_down();
    PUMP_MS(100);
    ui_extra_btn_down();
    PUMP_MS(100);
    ui_extra_btn_down();
    PUMP_MS(100);
    ui_extra_btn_down();
    PUMP_MS(100);
    ui_extra_btn_encoder();
    PUMP_MS(300);
    SCREENSHOT("interval_cam_main");

    /* Interval interactions */
    ui_extra_btn_encoder();
    PUMP_MS(300);
    SCREENSHOT("interval_cam_encoder");

    ui_extra_btn_up();
    PUMP_MS(200);
    SCREENSHOT("interval_cam_up");

    ui_extra_btn_down();
    PUMP_MS(200);
    SCREENSHOT("interval_cam_down");

    ui_extra_btn_right();
    PUMP_MS(200);
    SCREENSHOT("interval_cam_time_plus");

    ui_extra_btn_left();
    PUMP_MS(200);
    SCREENSHOT("interval_cam_time_minus");

    /* Back to main */
    ui_extra_btn_menu();
    PUMP_MS(200);

    /* ===== 8. FINAL STATE — return to main ===== */
    printf("\n[SIM] === Final State ===\n");
    SCREENSHOT("final_main_menu");

    printf("\n[SIM] Screenshot sequence complete: %d screenshots saved to screenshots/\n", screenshot_num);

    #undef PUMP_MS
    #undef SCREENSHOT
}

/*-------------------------------------------------*/
/* PPM framebuffer dump                             */
/*-------------------------------------------------*/
static void save_framebuffer_ppm(const char *filename)
{
    /* Force a full render cycle */
    lv_refr_now(NULL);

    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "[SIM] Failed to open %s for writing\n", filename);
        return;
    }

    fprintf(f, "P6\n%d %d\n255\n", DISP_HOR_RES, DISP_VER_RES);

    for (int i = 0; i < DISP_HOR_RES * DISP_VER_RES; i++) {
        lv_color_t c = s_fb[i];
        /* LVGL RGB565 with LV_COLOR_16_SWAP=1: green split across bytes */
        uint8_t r5 = c.ch.red;
        uint8_t g6 = LV_COLOR_GET_G(c);
        uint8_t b5 = c.ch.blue;
        uint8_t r = (r5 << 3) | (r5 >> 2);
        uint8_t g = (g6 << 2) | (g6 >> 4);
        uint8_t b = (b5 << 3) | (b5 >> 2);
        fputc(r, f);
        fputc(g, f);
        fputc(b, f);
    }

    fclose(f);
}
