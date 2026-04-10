/**
 * @file app_serial_cmd.c
 * @brief Serial command interface for automated testing
 *
 * Runs a FreeRTOS task that reads newline-terminated commands from stdin
 * (console UART) and dispatches them. Responses are printed to stdout
 * prefixed with "CMD>" for easy parsing by the host.
 */

#include "app_serial_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "ui_extra.h"
#include "app_gifs.h"

static const char *TAG = "serial_cmd";

#define CMD_BUF_SIZE 256
#define RESPONSE_PREFIX "CMD>"

static void cmd_respond(const char *fmt, ...)
{
    printf(RESPONSE_PREFIX);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

/* ---- Command handlers ---- */

static void cmd_ping(void)
{
    cmd_respond("pong");
}

static void cmd_status(void)
{
    ui_page_t page = ui_extra_get_current_page();
    const char *page_names[] = {
        "MAIN", "CAMERA", "INTERVAL_CAM", "VIDEO_MODE",
        "ALBUM", "USB_DISK", "SETTINGS", "AI_DETECT", "GIFS"
    };
    const char *page_name = (page < UI_PAGE_MAX) ? page_names[page] : "UNKNOWN";

    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    bool sd_mounted = ui_extra_get_sd_card_mounted();

    cmd_respond("page=%s sd=%s free_heap=%zu free_psram=%zu gifs_count=%d gifs_encoding=%d gifs_playing=%d",
                page_name, sd_mounted ? "yes" : "no",
                free_heap, free_psram,
                app_gifs_get_count(),
                app_gifs_is_encoding() ? 1 : 0,
                app_gifs_is_playing() ? 1 : 0);
}

static void cmd_menu_goto(const char *arg)
{
    ui_page_t page = UI_PAGE_MAIN;

    if (strcmp(arg, "main") == 0)           page = UI_PAGE_MAIN;
    else if (strcmp(arg, "camera") == 0)     page = UI_PAGE_CAMERA;
    else if (strcmp(arg, "interval") == 0)   page = UI_PAGE_INTERVAL_CAM;
    else if (strcmp(arg, "video") == 0)      page = UI_PAGE_VIDEO_MODE;
    else if (strcmp(arg, "album") == 0)      page = UI_PAGE_ALBUM;
    else if (strcmp(arg, "usb") == 0)        page = UI_PAGE_USB_DISK;
    else if (strcmp(arg, "settings") == 0)   page = UI_PAGE_SETTINGS;
    else if (strcmp(arg, "ai_detect") == 0)  page = UI_PAGE_AI_DETECT;
    else if (strcmp(arg, "gifs") == 0)       page = UI_PAGE_GIFS;
    else {
        cmd_respond("error: unknown page '%s'", arg);
        return;
    }

    bsp_display_lock(0);
    ui_extra_goto_page(page);
    bsp_display_unlock();

    cmd_respond("ok page=%s", arg);
}

static void cmd_btn(const char *which)
{
    bsp_display_lock(0);
    if (strcmp(which, "up") == 0)          ui_extra_btn_up();
    else if (strcmp(which, "down") == 0)   ui_extra_btn_down();
    else if (strcmp(which, "encoder") == 0) ui_extra_btn_encoder();
    else if (strcmp(which, "menu") == 0)   ui_extra_btn_menu();
    else if (strcmp(which, "left") == 0)   ui_extra_btn_left();
    else if (strcmp(which, "right") == 0)  ui_extra_btn_right();
    else {
        bsp_display_unlock();
        cmd_respond("error: unknown button '%s'", which);
        return;
    }
    bsp_display_unlock();
    cmd_respond("ok btn=%s", which);
}

static void cmd_gifs_create(const char *arg)
{
    int delay = 500;
    if (arg && *arg) delay = atoi(arg);
    if (delay <= 0) delay = 500;

    esp_err_t ret = app_gifs_create_from_album(delay);
    cmd_respond("%s gifs_create delay=%d", ret == ESP_OK ? "ok" : "error", delay);
}

static void cmd_gifs_list(void)
{
    app_gifs_scan();
    int count = app_gifs_get_count();
    cmd_respond("gifs_count=%d", count);
}

static void cmd_sd_ls(const char *path)
{
    if (!path || !*path) path = "/sdcard";

    DIR *dir = opendir(path);
    if (!dir) {
        cmd_respond("error: cannot open '%s'", path);
        return;
    }

    cmd_respond("ls %s", path);
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        /* Get file size */
        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%.255s/%.255s", path, entry->d_name);
        struct stat st;
        long size = -1;
        if (stat(fullpath, &st) == 0) size = st.st_size;

        cmd_respond("  %s %s size=%ld",
                     (entry->d_type == DT_DIR) ? "DIR" : "FILE",
                     entry->d_name, size);
        count++;
    }
    closedir(dir);
    cmd_respond("total=%d", count);
}

static void cmd_sd_stat(const char *path)
{
    if (!path || !*path) {
        cmd_respond("error: no path");
        return;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        cmd_respond("error: cannot stat '%s'", path);
        return;
    }

    /* Read first 16 bytes for format identification */
    FILE *f = fopen(path, "rb");
    if (!f) {
        cmd_respond("error: cannot open '%s'", path);
        return;
    }

    uint8_t header[16];
    int n = fread(header, 1, 16, f);
    fclose(f);

    /* Format header as hex string */
    char hex[48];
    for (int i = 0; i < n; i++) {
        sprintf(hex + i * 3, "%02X ", header[i]);
    }
    if (n > 0) hex[n * 3 - 1] = '\0';

    /* Check if it's a GIF */
    const char *type = "unknown";
    if (n >= 6 && header[0] == 'G' && header[1] == 'I' && header[2] == 'F') {
        type = (header[3] == '8' && header[4] == '9') ? "GIF89a" : "GIF87a";
    } else if (n >= 2 && header[0] == 0xFF && header[1] == 0xD8) {
        type = "JPEG";
    }

    cmd_respond("path=%s size=%ld type=%s header=[%s]", path, (long)st.st_size, type, hex);
}

static void cmd_sd_hexdump(const char *args)
{
    char path[256] = {0};
    long offset = 0, len = 64;

    if (sscanf(args, "%255s %ld %ld", path, &offset, &len) < 1) {
        cmd_respond("error: usage: sd_hexdump <path> [offset] [len]");
        return;
    }
    if (len > 256) len = 256;

    FILE *f = fopen(path, "rb");
    if (!f) {
        cmd_respond("error: cannot open '%s'", path);
        return;
    }

    fseek(f, offset, SEEK_SET);
    uint8_t buf[256];
    int n = fread(buf, 1, len, f);
    fclose(f);

    cmd_respond("hexdump %s offset=%ld len=%d", path, offset, n);
    for (int i = 0; i < n; i += 16) {
        char line[80];
        int pos = 0;
        pos += sprintf(line + pos, "%04lX: ", offset + i);
        for (int j = 0; j < 16 && (i + j) < n; j++) {
            pos += sprintf(line + pos, "%02X ", buf[i + j]);
        }
        cmd_respond("%s", line);
    }
}

static void cmd_sd_rm(const char *path)
{
    if (!path || !*path) {
        cmd_respond("error: no path");
        return;
    }
    if (unlink(path) == 0) {
        cmd_respond("ok deleted %s", path);
    } else {
        cmd_respond("error: cannot delete '%s'", path);
    }
}

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void cmd_sd_base64(const char *args)
{
    char path[256] = {0};
    long offset = 0, len = 512;

    if (sscanf(args, "%255s %ld %ld", path, &offset, &len) < 1) {
        cmd_respond("error: usage: sd_base64 <path> [offset] [len]");
        return;
    }
    if (len > 4096) len = 4096;

    FILE *f = fopen(path, "rb");
    if (!f) {
        cmd_respond("error: cannot open '%s'", path);
        return;
    }

    fseek(f, offset, SEEK_SET);
    uint8_t buf[4096];
    int n = fread(buf, 1, len, f);
    fclose(f);

    /* Base64 encode and output */
    cmd_respond("base64 %s offset=%ld len=%d", path, offset, n);

    char out[80];
    int opos = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)buf[i] << 16;
        if (i + 1 < n) v |= (uint32_t)buf[i + 1] << 8;
        if (i + 2 < n) v |= buf[i + 2];

        out[opos++] = b64_table[(v >> 18) & 0x3F];
        out[opos++] = b64_table[(v >> 12) & 0x3F];
        out[opos++] = (i + 1 < n) ? b64_table[(v >> 6) & 0x3F] : '=';
        out[opos++] = (i + 2 < n) ? b64_table[v & 0x3F] : '=';

        if (opos >= 76) {
            out[opos] = '\0';
            cmd_respond("%s", out);
            opos = 0;
        }
    }
    if (opos > 0) {
        out[opos] = '\0';
        cmd_respond("%s", out);
    }
    cmd_respond("end_base64");
}

/* ---- Command dispatcher ---- */

static void dispatch_command(char *line)
{
    /* Trim whitespace */
    while (*line == ' ' || *line == '\t') line++;
    char *end = line + strlen(line) - 1;
    while (end > line && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';

    if (*line == '\0') return;

    ESP_LOGI(TAG, "Command: '%s'", line);

    /* Parse command and argument */
    char *arg = strchr(line, ' ');
    if (arg) {
        *arg = '\0';
        arg++;
        while (*arg == ' ') arg++;
    }

    if (strcmp(line, "ping") == 0) {
        cmd_ping();
    } else if (strcmp(line, "status") == 0) {
        cmd_status();
    } else if (strcmp(line, "menu_goto") == 0) {
        cmd_menu_goto(arg ? arg : "main");
    } else if (strcmp(line, "btn_up") == 0) {
        cmd_btn("up");
    } else if (strcmp(line, "btn_down") == 0) {
        cmd_btn("down");
    } else if (strcmp(line, "btn_encoder") == 0) {
        cmd_btn("encoder");
    } else if (strcmp(line, "btn_menu") == 0) {
        cmd_btn("menu");
    } else if (strcmp(line, "btn_left") == 0) {
        cmd_btn("left");
    } else if (strcmp(line, "btn_right") == 0) {
        cmd_btn("right");
    } else if (strcmp(line, "gifs_create") == 0) {
        cmd_gifs_create(arg);
    } else if (strcmp(line, "gifs_list") == 0) {
        cmd_gifs_list();
    } else if (strcmp(line, "gifs_play") == 0) {
        bsp_display_lock(0);
        app_gifs_play_current();
        bsp_display_unlock();
        cmd_respond("ok gifs_play");
    } else if (strcmp(line, "gifs_stop") == 0) {
        app_gifs_stop();
        cmd_respond("ok gifs_stop");
    } else if (strcmp(line, "sd_ls") == 0) {
        cmd_sd_ls(arg);
    } else if (strcmp(line, "sd_stat") == 0) {
        cmd_sd_stat(arg);
    } else if (strcmp(line, "sd_hexdump") == 0) {
        cmd_sd_hexdump(arg ? arg : "");
    } else if (strcmp(line, "sd_rm") == 0) {
        cmd_sd_rm(arg);
    } else if (strcmp(line, "sd_base64") == 0) {
        cmd_sd_base64(arg ? arg : "");
    } else {
        cmd_respond("error: unknown command '%s'", line);
    }
}

/* ---- Serial listener task ---- */

static void serial_cmd_task(void *param)
{
    char buf[CMD_BUF_SIZE];
    int pos = 0;

    /* Disable line buffering on stdin so we get characters immediately */
    setvbuf(stdin, NULL, _IONBF, 0);

    /* Configure USB-Serial/JTAG VFS driver for non-blocking reads */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);

    ESP_LOGI(TAG, "Serial command interface ready");
    cmd_respond("ready");

    while (1) {
        /* fgetc blocks until data is available — that's fine in a dedicated task */
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                buf[pos] = '\0';
                dispatch_command(buf);
                pos = 0;
            }
        } else if (pos < CMD_BUF_SIZE - 1) {
            buf[pos++] = (char)c;
        }
    }
}

esp_err_t app_serial_cmd_init(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        serial_cmd_task, "serial_cmd", 4096, NULL, 3, NULL, 0);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial command task");
        return ESP_FAIL;
    }

    return ESP_OK;
}
