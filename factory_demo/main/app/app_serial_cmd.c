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
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_vfs_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "ui_extra.h"
#include "app_video_stream.h"
#include "app_gifs.h"
#include "app_pimslo.h"
#include "app_p4_net.h"
#include "spi_camera.h"

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

/* ---- GPIO trigger for external cameras ---- */

#define TRIGGER_GPIO  34
static bool trigger_gpio_initialized = false;

static void trigger_init(void)
{
    if (trigger_gpio_initialized) return;
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TRIGGER_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT,  /* Push-pull output + readable */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)TRIGGER_GPIO, 1);  /* Start HIGH (not triggering) */
    trigger_gpio_initialized = true;
    ESP_LOGI(TAG, "Trigger GPIO %d initialized (push-pull, pull-up), config ret=%d", TRIGGER_GPIO, ret);
}

/* ---- Command handlers ---- */

static void cmd_ping(void)
{
    cmd_respond("pong");
}

static void cmd_trigger(const char *arg)
{
    int pulse_ms = 200;
    if (arg && *arg) pulse_ms = atoi(arg);
    if (pulse_ms <= 0) pulse_ms = 200;
    if (pulse_ms > 5000) pulse_ms = 5000;

    trigger_init();

    /* Read GPIO state before */
    int before = gpio_get_level((gpio_num_t)TRIGGER_GPIO);

    /* Pulse LOW */
    gpio_set_level((gpio_num_t)TRIGGER_GPIO, 0);
    int during = gpio_get_level((gpio_num_t)TRIGGER_GPIO);
    vTaskDelay(pdMS_TO_TICKS(pulse_ms));

    /* Release HIGH */
    gpio_set_level((gpio_num_t)TRIGGER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    int after = gpio_get_level((gpio_num_t)TRIGGER_GPIO);

    cmd_respond("ok trigger gpio=%d pulse_ms=%d levels: before=%d during=%d after=%d",
                TRIGGER_GPIO, pulse_ms, before, during, after);
}

static void cmd_gpio_read(const char *arg)
{
    int pin = TRIGGER_GPIO;
    if (arg && *arg) pin = atoi(arg);

    /* Configure as input to read */
    int level = gpio_get_level((gpio_num_t)pin);
    cmd_respond("gpio %d = %d", pin, level);
}

#ifndef P4MSLO_FIRMWARE_VERSION
#define P4MSLO_FIRMWARE_VERSION "unknown"
#endif

static void cmd_status(void)
{
    ui_page_t page = ui_extra_get_current_page();
    /* Kept in sync with ui_page_t in ui_extra.h. UI_PAGE_AI_DETECT was
     * removed in the AI-stripping commit so there is no "AI_DETECT"
     * entry — index 7 is GIFS directly. */
    const char *page_names[] = {
        "MAIN", "CAMERA", "INTERVAL_CAM", "VIDEO_MODE",
        "ALBUM", "USB_DISK", "SETTINGS", "GIFS"
    };
    const char *page_name = (page < UI_PAGE_MAX) ? page_names[page] : "UNKNOWN";

    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    bool sd_mounted = ui_extra_get_sd_card_mounted();

    cmd_respond("fw=%s page=%s sd=%s free_heap=%zu free_psram=%zu gifs_count=%d gifs_encoding=%d gifs_playing=%d pimslo_queue=%d pimslo_encoding=%d pimslo_capturing=%d",
                P4MSLO_FIRMWARE_VERSION,
                page_name, sd_mounted ? "yes" : "no",
                free_heap, free_psram,
                app_gifs_get_count(),
                app_gifs_is_encoding() ? 1 : 0,
                app_gifs_is_playing() ? 1 : 0,
                app_pimslo_get_queue_depth(),
                app_pimslo_is_encoding() ? 1 : 0,
                app_pimslo_is_capturing() ? 1 : 0);
}

static void cmd_menu_goto(const char *arg)
{
    ui_page_t page = UI_PAGE_MAIN;
    const char *resp_name = NULL;

    /* Response name matches the status command's page_names[] casing
     * (uppercase) so tests can regex `page=(\w+)` and compare against
     * `MAIN` etc. without worrying about which command produced the
     * log line. Previous behaviour echoed the user's argument
     * verbatim (lowercase) which caused test 07 / 08 to read
     * `page=main` while they expected `page=MAIN`. */
    if (strcmp(arg, "main") == 0)            { page = UI_PAGE_MAIN;         resp_name = "MAIN"; }
    else if (strcmp(arg, "camera") == 0)     { page = UI_PAGE_CAMERA;       resp_name = "CAMERA"; }
    else if (strcmp(arg, "interval") == 0)   { page = UI_PAGE_INTERVAL_CAM; resp_name = "INTERVAL_CAM"; }
    else if (strcmp(arg, "video") == 0)      { page = UI_PAGE_VIDEO_MODE;   resp_name = "VIDEO_MODE"; }
    else if (strcmp(arg, "album") == 0)      { page = UI_PAGE_ALBUM;        resp_name = "ALBUM"; }
    else if (strcmp(arg, "usb") == 0)        { page = UI_PAGE_USB_DISK;     resp_name = "USB_DISK"; }
    else if (strcmp(arg, "settings") == 0)   { page = UI_PAGE_SETTINGS;     resp_name = "SETTINGS"; }
    else if (strcmp(arg, "gifs") == 0)       { page = UI_PAGE_GIFS;         resp_name = "GIFS"; }
    else {
        cmd_respond("error: unknown page '%s'", arg);
        return;
    }

    bsp_display_lock(0);
    ui_extra_goto_page(page);
    bsp_display_unlock();

    cmd_respond("ok page=%s", resp_name);
}

static void cmd_btn(const char *which)
{
    bsp_display_lock(0);
    if (strcmp(which, "up") == 0)          ui_extra_btn_up();
    else if (strcmp(which, "down") == 0)   ui_extra_btn_down();
    else if (strcmp(which, "encoder") == 0) ui_extra_btn_encoder();
    /* "menu" and "trigger" are aliases — both refer to the physical
     * trigger/photo button at the top of the P4-EYE body. "menu" is
     * the historical name from the BSP; "trigger" is the user-facing
     * name. They drive ui_extra_btn_menu() which: takes a photo on
     * camera pages, opens/confirms the delete modal on the gallery. */
    else if (strcmp(which, "menu") == 0)    ui_extra_btn_menu();
    else if (strcmp(which, "trigger") == 0) ui_extra_btn_menu();
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
    int frames = 4;
    if (arg && *arg) {
        /* Parse: "delay [frames]" */
        char *space = strchr(arg, ' ');
        delay = atoi(arg);
        if (space) frames = atoi(space + 1);
    }
    if (delay <= 0) delay = 500;
    if (frames <= 0) frames = 4;

    esp_err_t ret = app_gifs_create_from_album(delay, frames);
    cmd_respond("%s gifs_create delay=%d frames=%d", ret == ESP_OK ? "ok" : "error", delay, frames);
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
    } else if (strcmp(line, "trigger") == 0) {
        cmd_trigger(arg);
    } else if (strcmp(line, "spi_init") == 0) {
        esp_err_t ret = spi_camera_init();
        cmd_respond("%s spi_init", ret == ESP_OK ? "ok" : "error");
    } else if (strcmp(line, "spi_capture") == 0) {
        /* Trigger cameras, then receive JPEG via SPI from camera #1 */
        spi_camera_init();
        trigger_init();

        /* Pulse GPIO34 to trigger capture */
        gpio_set_level((gpio_num_t)TRIGGER_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level((gpio_num_t)TRIGGER_GPIO, 1);
        ESP_LOGI(TAG, "Trigger sent, waiting for capture...");
        vTaskDelay(pdMS_TO_TICKS(500));  /* Wait for S3 to capture */

        /* Receive JPEG via SPI */
        uint8_t *jpeg_buf = NULL;
        size_t jpeg_size = 0;
        uint32_t transfer_ms = 0;

        esp_err_t ret = spi_camera_receive_jpeg(0, &jpeg_buf, &jpeg_size, &transfer_ms);
        if (ret == ESP_OK && jpeg_buf) {
            /* Verify JPEG */
            bool valid = (jpeg_size >= 2 && jpeg_buf[0] == 0xFF && jpeg_buf[1] == 0xD8);

            /* Optionally save to SD card for verification */
            if (valid) {
                mkdir("/sdcard/pimslo", 0755);
                FILE *f = fopen("/sdcard/pimslo/spi_test.jpg", "wb");
                if (f) {
                    fwrite(jpeg_buf, 1, jpeg_size, f);
                    fclose(f);
                }
            }

            cmd_respond("ok spi_capture size=%zu transfer_ms=%lu valid=%s",
                        jpeg_size, (unsigned long)transfer_ms,
                        valid ? "yes" : "no");
            free(jpeg_buf);
        } else {
            cmd_respond("error spi_capture ret=0x%x", ret);
        }
    } else if (strcmp(line, "spi_capture_all") == 0) {
        /* Capture all 4 cameras and save pos{1..4}.jpg — NO GIF encoding.
         * Used by scripts/test_spi_integrity.py to avoid serial-port flood. */
        spi_camera_init();
        trigger_init();

        uint8_t *jpeg_bufs[4] = {NULL};
        size_t jpeg_sizes[4] = {0};
        uint32_t capture_ms = 0;

        esp_err_t ret = spi_camera_capture_all(jpeg_bufs, jpeg_sizes, &capture_ms);

        int saved = 0;
        mkdir("/sdcard/pimslo", 0755);
        for (int i = 0; i < 4; i++) {
            if (jpeg_bufs[i] && jpeg_sizes[i] > 0) {
                char path[64];
                snprintf(path, sizeof(path), "/sdcard/pimslo/pos%d.jpg", i + 1);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fwrite(jpeg_bufs[i], 1, jpeg_sizes[i], f);
                    fclose(f);
                    saved++;
                }
                free(jpeg_bufs[i]);
            }
        }
        cmd_respond("ok spi_capture_all capture_ms=%lu saved=%d ret=0x%x",
                    (unsigned long)capture_ms, saved, ret);
    } else if (strcmp(line, "spi_pimslo") == 0) {
        /* Full pipeline: trigger → SPI receive all 4 → save to SD → encode GIF */
        spi_camera_init();
        trigger_init();

        uint8_t *jpeg_bufs[4] = {NULL};
        size_t jpeg_sizes[4] = {0};
        uint32_t capture_ms = 0;

        esp_err_t ret = spi_camera_capture_all(jpeg_bufs, jpeg_sizes, &capture_ms);

        /* Save received JPEGs to SD for the pimslo encoder */
        int saved = 0;
        mkdir("/sdcard/pimslo", 0755);
        for (int i = 0; i < 4; i++) {
            if (jpeg_bufs[i] && jpeg_sizes[i] > 0) {
                char path[64];
                snprintf(path, sizeof(path), "/sdcard/pimslo/pos%d.jpg", i + 1);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fwrite(jpeg_bufs[i], 1, jpeg_sizes[i], f);
                    fclose(f);
                    saved++;
                }
                free(jpeg_bufs[i]);
            }
        }

        if (saved >= 2) {
            cmd_respond("ok spi_pimslo capture_ms=%lu saved=%d encoding...",
                        (unsigned long)capture_ms, saved);
            /* Trigger PIMSLO GIF encoding */
            int delay = 150;
            float parallax = 0.05f;
            if (arg && *arg) sscanf(arg, "%d %f", &delay, &parallax);
            app_gifs_create_pimslo(delay, parallax);
        } else {
            cmd_respond("error spi_pimslo capture_ms=%lu saved=%d",
                        (unsigned long)capture_ms, saved);
        }
    } else if (strcmp(line, "photo_btn") == 0) {
        /* Simulate the physical trigger press on the camera page:
         * fires app_video_stream_take_photo() which sets is_take_photo,
         * and the next frame callback executes the full path — flash
         * pulse, screen-dim feedback, P4 JPEG save AND the SPI pimslo
         * capture request. Previously this command only called
         * app_pimslo_request_capture() which bypassed the visual
         * feedback and the P4 photo save. Now tests exercise the same
         * path a user gets from pressing the trigger. */
        if (ui_extra_get_current_page() == UI_PAGE_CAMERA) {
            app_video_stream_take_photo();
            cmd_respond("ok photo_btn (take_photo scheduled)");
        } else {
            /* Off-camera: pimslo request only; no P4 photo. */
            app_pimslo_request_capture();
            cmd_respond("ok photo_btn (capture queued, no viewfinder)");
        }
    } else if (strcmp(line, "pimslo") == 0) {
        /* Create PIMSLO GIF from /sdcard/pimslo/pos{1-4}.jpg */
        int delay = 150;
        float parallax = 0.05f;
        if (arg && *arg) {
            sscanf(arg, "%d %f", &delay, &parallax);
        }
        esp_err_t ret = app_gifs_create_pimslo(delay, parallax);
        cmd_respond("%s pimslo delay=%d parallax=%.2f",
                    ret == ESP_OK ? "ok" : "error", delay, parallax);
    } else if (strcmp(line, "sd_write") == 0) {
        /* Write raw binary data to SD card file.
         * Usage: sd_write <path> <size>
         * Then send exactly <size> raw bytes immediately after newline. */
        if (!arg || !*arg) {
            cmd_respond("error: usage: sd_write <path> <size>");
        } else {
            char path[256] = {0};
            long size = 0;
            sscanf(arg, "%255s %ld", path, &size);
            if (size <= 0 || size > 2000000) {
                cmd_respond("error: invalid size %ld (max 2MB)", size);
            } else {
                /* Create parent directory if needed */
                char dir[256];
                char *lastslash = strrchr(path, '/');
                if (lastslash) {
                    int dirlen = lastslash - path;
                    strncpy(dir, path, dirlen);
                    dir[dirlen] = '\0';
                    mkdir(dir, 0755);
                }

                FILE *f = fopen(path, "wb");
                if (!f) {
                    cmd_respond("error: cannot create %s", path);
                } else {
                    cmd_respond("ready %ld", size);
                    long remaining = size;
                    uint8_t *buf = malloc(4096);
                    if (buf) {
                        while (remaining > 0) {
                            int to_read = remaining > 4096 ? 4096 : (int)remaining;
                            int n = read(STDIN_FILENO, buf, to_read);
                            if (n <= 0) {
                                vTaskDelay(pdMS_TO_TICKS(5));
                                continue;
                            }
                            fwrite(buf, 1, n, f);
                            remaining -= n;
                        }
                        free(buf);
                    }
                    fclose(f);
                    cmd_respond("ok wrote %ld bytes to %s", size, path);
                }
            }
        }
    } else if (strcmp(line, "gpio_read") == 0) {
        cmd_gpio_read(arg);
    } else if (strcmp(line, "cam_wifi_on") == 0 ||
               strcmp(line, "cam_wifi_off") == 0 ||
               strcmp(line, "cam_reboot") == 0 ||
               strcmp(line, "cam_identify") == 0) {
        /* cam_wifi_on N | cam_wifi_off N | cam_reboot N | cam_identify N
         * N = 1-4 (camera index 0-3 + 1), or "all" for broadcast */
        uint8_t cmd_byte = 0;
        if      (strcmp(line, "cam_wifi_on")  == 0) cmd_byte = SPI_CAM_CMD_WIFI_ON;
        else if (strcmp(line, "cam_wifi_off") == 0) cmd_byte = SPI_CAM_CMD_WIFI_OFF;
        else if (strcmp(line, "cam_reboot")   == 0) cmd_byte = SPI_CAM_CMD_REBOOT;
        else                                         cmd_byte = SPI_CAM_CMD_IDENTIFY;

        spi_camera_init();
        if (arg && strcmp(arg, "all") == 0) {
            int ok = 0;
            for (int i = 0; i < SPI_CAM_COUNT; i++) {
                if (spi_camera_send_control(i, cmd_byte) == ESP_OK) ok++;
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            cmd_respond("ok %s all (%d/%d sent)", line, ok, SPI_CAM_COUNT);
        } else {
            int n = arg ? atoi(arg) : 0;
            if (n < 1 || n > SPI_CAM_COUNT) {
                cmd_respond("error %s: usage N (1-%d) | all", line, SPI_CAM_COUNT);
            } else {
                esp_err_t ret = spi_camera_send_control(n - 1, cmd_byte);
                cmd_respond("%s %s cam=%d cmd=0x%02X ret=0x%x",
                            ret == ESP_OK ? "ok" : "error",
                            line, n, cmd_byte, ret);
            }
        }
    } else if (strcmp(line, "fast_capture") == 0) {
        /* fast_capture [on|off|status]
         * Toggles the Phase 4 fast-capture mode. Without args, prints state. */
        if (!arg || !*arg || strcmp(arg, "status") == 0) {
            cmd_respond("fast_capture=%s", app_pimslo_get_fast_mode() ? "on" : "off");
        } else if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0) {
            esp_err_t r = app_pimslo_set_fast_mode(true);
            cmd_respond("%s fast_capture=on", r == ESP_OK ? "ok" : "error");
        } else if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
            esp_err_t r = app_pimslo_set_fast_mode(false);
            cmd_respond("%s fast_capture=off", r == ESP_OK ? "ok" : "error");
        } else {
            cmd_respond("error fast_capture: usage on|off|status");
        }
    } else if (strcmp(line, "cam_ae") == 0) {
        /* cam_ae [N]  → read AE gain + exposure from one camera or all */
        spi_camera_init();
        int start = 0, end = SPI_CAM_COUNT;
        if (arg && *arg) {
            int n = atoi(arg);
            if (n >= 1 && n <= SPI_CAM_COUNT) { start = n - 1; end = n; }
        }
        for (int i = start; i < end; i++) {
            uint16_t g = 0;
            uint32_t e = 0;
            esp_err_t r = spi_camera_read_exposure(i, &g, &e);
            if (r == ESP_OK) {
                cmd_respond("cam %d ae: gain=%u exposure=%lu",
                            i + 1, (unsigned)g, (unsigned long)e);
            } else {
                cmd_respond("cam %d ae: read failed (0x%x)", i + 1, r);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        cmd_respond("ok cam_ae");
    } else if (strcmp(line, "cam_sync_ae") == 0) {
        /* cam_sync_ae [ref]  → read ref camera AE and broadcast to the rest.
         * Default ref = 2 (our most-reliable cam). */
        spi_camera_init();
        int ref = arg && *arg ? atoi(arg) : 2;
        if (ref < 1 || ref > SPI_CAM_COUNT) {
            cmd_respond("error cam_sync_ae: ref must be 1-%d", SPI_CAM_COUNT);
        } else {
            esp_err_t r = spi_camera_sync_exposure(ref - 1);
            cmd_respond("%s cam_sync_ae ref=%d", r == ESP_OK ? "ok" : "error", ref);
        }
    } else if (strcmp(line, "cam_af") == 0) {
        /* cam_af  → broadcast AUTOFOCUS to all and poll for AF_LOCKED */
        spi_camera_init();
        esp_err_t r = spi_camera_autofocus_all(2000);
        cmd_respond("%s cam_af", r == ESP_OK ? "ok" : "error");
    } else if (strcmp(line, "wifi_start") == 0) {
        esp_err_t r = app_p4_net_start();
        char ip[16] = {0};
        app_p4_net_get_ip(ip, sizeof(ip));
        cmd_respond("%s wifi_start connected=%d ip=%s",
                    r == ESP_OK ? "ok" : "error",
                    app_p4_net_is_connected() ? 1 : 0, ip);
    } else if (strcmp(line, "wifi_stop") == 0) {
        esp_err_t r = app_p4_net_stop();
        cmd_respond("%s wifi_stop", r == ESP_OK ? "ok" : "error");
    } else if (strcmp(line, "wifi_status") == 0) {
        char ip[16] = {0};
        app_p4_net_get_ip(ip, sizeof(ip));
        cmd_respond("wifi connected=%d ip=%s",
                    app_p4_net_is_connected() ? 1 : 0, ip);
    } else if (strcmp(line, "cam_status") == 0) {
        /* cam_status [N]  → query one camera, or all if no arg */
        spi_camera_init();
        int start = 0, end = SPI_CAM_COUNT;
        if (arg && *arg) {
            int n = atoi(arg);
            if (n >= 1 && n <= SPI_CAM_COUNT) { start = n - 1; end = n; }
        }
        for (int i = start; i < end; i++) {
            uint8_t s = 0;
            esp_err_t r = spi_camera_query_status(i, &s);
            if (r == ESP_OK) {
                cmd_respond("cam %d: 0x%02X jpeg=%d wifi=%d connected=%d af=%d",
                            i + 1, s,
                            (s & SPI_CAM_STATUS_JPEG_READY)    ? 1 : 0,
                            (s & SPI_CAM_STATUS_WIFI_ACTIVE)   ? 1 : 0,
                            (s & SPI_CAM_STATUS_WIFI_CONNECTED) ? 1 : 0,
                            (s & SPI_CAM_STATUS_AF_LOCKED)     ? 1 : 0);
            } else {
                cmd_respond("cam %d: no response (0x%x)", i + 1, r);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        cmd_respond("ok cam_status");
    } else if (strcmp(line, "status") == 0) {
        cmd_status();
    } else if (strcmp(line, "heap_caps") == 0) {
        size_t dma_int_free  = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        size_t dma_int_large = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        size_t int_free      = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t int_large     = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        size_t psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t psram_large   = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        cmd_respond("heap_caps dma_int=%zu (largest=%zu) int=%zu (largest=%zu) psram=%zu (largest=%zu)",
                    dma_int_free, dma_int_large, int_free, int_large,
                    psram_free, psram_large);
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
    } else if (strcmp(line, "btn_trigger") == 0) {
        /* Clearer alias for the physical trigger/photo button. */
        cmd_btn("trigger");
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
    /* 8 KB sufficed under -Og. A transient experiment with main-wide
     * -O2 needed 16 KB (inlined locals in the video_stream_free_buffers
     * → snprintf chain overflowed), but we reverted to per-file -O2 on
     * just the pure-math Gif sources — no inlining changes reach this
     * task. 8 KB is plenty; internal RAM is tight and larger task
     * stacks were starving xTaskCreate. */
    BaseType_t ret = xTaskCreatePinnedToCore(
        serial_cmd_task, "serial_cmd", 8192, NULL, 3, NULL, 0);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial command task");
        return ESP_FAIL;
    }

    return ESP_OK;
}
