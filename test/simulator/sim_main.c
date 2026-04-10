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

/* Album functions — declared in app_album.h, implemented in sim_hal.c */
#include "esp_err.h"
esp_err_t app_album_init(void *parent);
esp_err_t app_album_next_image(void);
esp_err_t app_album_prev_image(void);

/* PNG output */
#include <png.h>

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
static void save_framebuffer_png(const char *filename);

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

        /* Setup display with headless flush — direct mode renders into s_fb */
        static lv_disp_draw_buf_t draw_buf;
        lv_disp_draw_buf_init(&draw_buf, s_fb, NULL, DISP_HOR_RES * DISP_VER_RES);

        static lv_disp_drv_t disp_drv;
        lv_disp_drv_init(&disp_drv);
        disp_drv.draw_buf    = &draw_buf;
        disp_drv.flush_cb    = headless_flush_cb;
        disp_drv.hor_res     = DISP_HOR_RES;
        disp_drv.ver_res     = DISP_VER_RES;
        disp_drv.direct_mode = 1;  /* Render directly into s_fb */
        lv_disp_drv_register(&disp_drv);
    }

    /* Initialize the REAL UI */
    ui_init();
    ui_extra_init();

    /* Initialize album canvas with the real UI object (mirrors app_storage.c) */
    app_album_init(ui_ImageScreenAlbum);

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
/* Screenshot output directory (relative to build/) */
/*-------------------------------------------------*/
#define SCREENSHOT_DIR "../screenshots"

/*-------------------------------------------------*/
/* Screenshot mode: comprehensive page capture      */
/*-------------------------------------------------*/
static void run_screenshot_mode(void)
{
    int screenshot_num = 0;
    char filename[256];

    /* Create screenshots directory */
    (void)system("mkdir -p " SCREENSHOT_DIR);

    /* Helper: pump LVGL for N milliseconds to let rendering settle */
    #define PUMP_MS(ms) do { \
        uint32_t _start = SDL_GetTicks(); \
        while (SDL_GetTicks() - _start < (uint32_t)(ms)) { \
            lv_timer_handler(); \
            SDL_Delay(1); \
        } \
    } while(0)

    #define SCREENSHOT(label) do { \
        PUMP_MS(500); \
        snprintf(filename, sizeof(filename), SCREENSHOT_DIR "/%02d_%s.png", screenshot_num++, (label)); \
        save_framebuffer_png(filename); \
        printf("[SCREENSHOT] %s\n", filename); \
    } while(0)

    /* Helper: navigate to a page cleanly (cancel stale timers, go via main) */
    #define GOTO_PAGE(page) do { \
        ui_extra_cancel_popup_timer(); \
        ui_extra_goto_page(UI_PAGE_MAIN); \
        PUMP_MS(300); \
        ui_extra_cancel_popup_timer(); \
        ui_extra_goto_page(page); \
        PUMP_MS(500); \
    } while(0)

    /* Simulate SD card inserted so pages don't block on SD warnings */
    ui_extra_set_sd_card_mounted(true);
    PUMP_MS(200);

    /* ===== 1. MAIN MENU — scroll through all items ===== */
    printf("\n[SIM] === Main Menu Screenshots ===\n");
    /* Menu order: Camera(0), Interval Cam(1), Video Mode(2),
       AI Detect(3), Album(4), USB Disk(5), Settings(6) */
    SCREENSHOT("main_menu_camera");

    ui_extra_btn_down();
    SCREENSHOT("main_menu_interval_cam");

    ui_extra_btn_down();
    SCREENSHOT("main_menu_video_mode");

    ui_extra_btn_down();
    SCREENSHOT("main_menu_ai_detect");

    ui_extra_btn_down();
    SCREENSHOT("main_menu_album");

    ui_extra_btn_down();
    SCREENSHOT("main_menu_usb_disk");

    ui_extra_btn_down();
    SCREENSHOT("main_menu_settings");

    /* ===== 2. CAMERA PAGE ===== */
    printf("\n[SIM] === Camera Page Screenshots ===\n");
    GOTO_PAGE(UI_PAGE_CAMERA);
    SCREENSHOT("camera_main");

    /* Down button interaction */
    ui_extra_btn_down();
    PUMP_MS(300);
    SCREENSHOT("camera_btn_down");

    /* Up button interaction */
    ui_extra_btn_up();
    PUMP_MS(300);
    SCREENSHOT("camera_btn_up");

    /* Encoder press (take photo) */
    ui_extra_btn_encoder();
    PUMP_MS(300);
    SCREENSHOT("camera_take_photo");

    /* Zoom via knob */
    ui_extra_btn_right();
    PUMP_MS(300);
    SCREENSHOT("camera_zoom_in");

    ui_extra_btn_left();
    PUMP_MS(300);
    SCREENSHOT("camera_zoom_out");

    /* ===== 3. ALBUM PAGE ===== */
    printf("\n[SIM] === Album Page Screenshots ===\n");
    GOTO_PAGE(UI_PAGE_ALBUM);
    SCREENSHOT("album_photo1");

    /* Browse through fake photos via direct album API */
    app_album_next_image();
    SCREENSHOT("album_photo2");

    app_album_next_image();
    SCREENSHOT("album_photo3");

    app_album_next_image();
    SCREENSHOT("album_photo4");

    app_album_prev_image();
    SCREENSHOT("album_photo3_back");

    /* ===== 4. SETTINGS PAGE ===== */
    printf("\n[SIM] === Settings Page Screenshots ===\n");
    GOTO_PAGE(UI_PAGE_SETTINGS);
    SCREENSHOT("settings_main");

    /* Scroll through settings items */
    ui_extra_btn_down();
    PUMP_MS(300);
    SCREENSHOT("settings_item2");

    ui_extra_btn_down();
    PUMP_MS(300);
    SCREENSHOT("settings_item3");

    ui_extra_btn_down();
    PUMP_MS(300);
    SCREENSHOT("settings_item4");

    ui_extra_btn_down();
    PUMP_MS(300);
    SCREENSHOT("settings_item5");

    ui_extra_btn_down();
    PUMP_MS(300);
    SCREENSHOT("settings_item6");

    /* Toggle a setting */
    ui_extra_btn_right();
    PUMP_MS(300);
    SCREENSHOT("settings_toggle_right");

    ui_extra_btn_left();
    PUMP_MS(300);
    SCREENSHOT("settings_toggle_left");

    /* Encoder press on setting */
    ui_extra_btn_encoder();
    PUMP_MS(300);
    SCREENSHOT("settings_encoder_press");

    /* ===== 5. VIDEO MODE PAGE ===== */
    printf("\n[SIM] === Video Mode Screenshots ===\n");
    GOTO_PAGE(UI_PAGE_VIDEO_MODE);
    SCREENSHOT("video_mode_main");

    /* Start/stop recording */
    ui_extra_btn_encoder();
    PUMP_MS(300);
    SCREENSHOT("video_mode_record");

    ui_extra_btn_down();
    PUMP_MS(300);
    SCREENSHOT("video_mode_btn_down");

    ui_extra_btn_up();
    PUMP_MS(300);
    SCREENSHOT("video_mode_btn_up");

    /* ===== 6. AI DETECT PAGE ===== */
    printf("\n[SIM] === AI Detection Screenshots ===\n");
    GOTO_PAGE(UI_PAGE_AI_DETECT);
    SCREENSHOT("ai_detect_main");

    /* Cycle through AI modes */
    ui_extra_btn_encoder();
    PUMP_MS(300);
    SCREENSHOT("ai_detect_mode2");

    ui_extra_btn_encoder();
    PUMP_MS(300);
    SCREENSHOT("ai_detect_mode3");

    /* Zoom via knob */
    ui_extra_btn_right();
    PUMP_MS(300);
    SCREENSHOT("ai_detect_zoom_in");

    ui_extra_btn_left();
    PUMP_MS(300);
    SCREENSHOT("ai_detect_zoom_out");

    /* ===== 7. INTERVAL CAMERA PAGE ===== */
    printf("\n[SIM] === Interval Camera Screenshots ===\n");
    GOTO_PAGE(UI_PAGE_INTERVAL_CAM);
    SCREENSHOT("interval_cam_main");

    /* Start interval */
    ui_extra_btn_encoder();
    PUMP_MS(300);
    SCREENSHOT("interval_cam_start");

    ui_extra_btn_up();
    PUMP_MS(300);
    SCREENSHOT("interval_cam_btn_up");

    ui_extra_btn_down();
    PUMP_MS(300);
    SCREENSHOT("interval_cam_btn_down");

    ui_extra_btn_right();
    PUMP_MS(300);
    SCREENSHOT("interval_cam_time_plus");

    ui_extra_btn_left();
    PUMP_MS(300);
    SCREENSHOT("interval_cam_time_minus");

    /* ===== 8. USB DISK PAGE ===== */
    printf("\n[SIM] === USB Disk Screenshots ===\n");
    GOTO_PAGE(UI_PAGE_USB_DISK);
    SCREENSHOT("usb_disk_main");

    /* ===== 9. FINAL — return to main ===== */
    printf("\n[SIM] === Final State ===\n");
    GOTO_PAGE(UI_PAGE_MAIN);
    SCREENSHOT("final_main_menu");

    printf("\n[SIM] Screenshot sequence complete: %d screenshots saved to " SCREENSHOT_DIR "/\n", screenshot_num);

    #undef PUMP_MS
    #undef SCREENSHOT
    #undef GOTO_PAGE
}

/*-------------------------------------------------*/
/* PNG framebuffer dump via libpng                  */
/*-------------------------------------------------*/
static void save_framebuffer_png(const char *filename)
{
    /* Force a full render cycle */
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);

    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "[SIM] Failed to open %s for writing\n", filename);
        return;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(f); return; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(f); return; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        return;
    }

    png_init_io(png, f);
    png_set_IHDR(png, info, DISP_HOR_RES, DISP_VER_RES, 8,
                 PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    uint8_t row[DISP_HOR_RES * 3];
    for (int y = 0; y < DISP_VER_RES; y++) {
        for (int x = 0; x < DISP_HOR_RES; x++) {
            lv_color_t c = s_fb[y * DISP_HOR_RES + x];
            uint8_t r5 = c.ch.red;
            uint8_t g6 = LV_COLOR_GET_G(c);
            uint8_t b5 = c.ch.blue;
            row[x * 3 + 0] = (r5 << 3) | (r5 >> 2);
            row[x * 3 + 1] = (g6 << 2) | (g6 >> 4);
            row[x * 3 + 2] = (b5 << 3) | (b5 >> 2);
        }
        png_write_row(png, row);
    }

    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(f);
}
