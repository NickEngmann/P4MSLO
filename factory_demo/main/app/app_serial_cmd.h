/**
 * @file app_serial_cmd.h
 * @brief Serial command interface for automated testing
 *
 * Listens for newline-terminated commands on the console UART and
 * dispatches them to the appropriate handlers. This enables automated
 * testing from a host machine without physical button presses.
 *
 * Commands:
 *   ping                 — responds "pong"
 *   status               — print current page, SD state, free heap
 *   menu_goto <page>     — navigate to a page (camera, album, settings, gifs, etc.)
 *   btn_up / btn_down    — simulate button presses
 *   btn_encoder           — simulate encoder press
 *   btn_menu              — simulate menu button
 *   gifs_create [delay]   — create GIF from album photos (delay in ms, default 500)
 *   gifs_list              — list GIF files on SD card
 *   gifs_play              — play current GIF
 *   gifs_stop              — stop GIF playback
 *   sd_ls [path]           — list files in SD card directory
 *   sd_stat <path>         — print file size and first bytes (for validation)
 *   sd_hexdump <path> <offset> <len> — hex dump a portion of a file
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the serial command listener task
 */
esp_err_t app_serial_cmd_init(void);

#ifdef __cplusplus
}
#endif
