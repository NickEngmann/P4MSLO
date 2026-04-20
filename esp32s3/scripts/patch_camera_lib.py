"""
Pre-build script to patch esp32-camera v2.0.4 for PlatformIO/ESP-IDF.

Patches applied:
1. library.json srcFilter: include target/esp32s3/ source files
2. ll_cam.h: add gdma_channel_handle_t field to cam_obj_t struct
3. ll_cam.c: replace raw GDMA register manipulation with proper ESP-IDF
   GDMA API (gdma_new_channel/gdma_del_channel). This fixes the critical
   bug where WiFi steals the camera's DMA channel after connecting.
   See: https://github.com/espressif/esp32-camera/issues/620
4. ov5640.c: clamp oversized framesize to FRAMESIZE_5MP instead of
   returning an error. Matches mega_ccm.c behavior.
   See: https://github.com/espressif/esp32-camera/pull/810

This runs IMMEDIATELY when platformio.ini is parsed (not as a pre-action).
"""
import json
import os
import glob

Import("env")

def patch_now():
    libdeps_dir = env.get("PROJECT_LIBDEPS_DIR", "")
    pioenv = env.get("PIOENV", "xiao_esp32s3")

    search_paths = [
        os.path.join(libdeps_dir, pioenv, "esp32-camera", "library.json"),
        os.path.join(".pio", "libdeps", pioenv, "esp32-camera", "library.json"),
    ]

    for lib_json in search_paths:
        if not os.path.exists(lib_json):
            continue

        lib_dir = os.path.dirname(lib_json)

        # --- Patch 1: srcFilter for target/esp32s3/ ---
        with open(lib_json, "r") as f:
            data = json.load(f)

        src_filter = data.get("build", {}).get("srcFilter", [])
        target_entry = "+<target/esp32s3>"
        if target_entry not in src_filter:
            src_filter.append(target_entry)
            data["build"]["srcFilter"] = src_filter
            with open(lib_json, "w") as f:
                json.dump(data, f, indent=2)
            print("*** Patched library.json to include target/esp32s3/ ***")

            build_dir = env.get("BUILD_DIR", os.path.join(".pio", "build", pioenv))
            for cached in glob.glob(os.path.join(build_dir, "lib*", "libesp32-camera.a")):
                os.remove(cached)
                print(f"*** Removed cached {cached} ***")

        # --- Patch 2: ll_cam.h — add dma_channel_handle field ---
        ll_cam_h = os.path.join(lib_dir, "target", "private_include", "ll_cam.h")
        if os.path.exists(ll_cam_h):
            with open(ll_cam_h, "r") as f:
                h_content = f.read()

            if "gdma_channel_handle_t" not in h_content:
                # Add GDMA include after existing includes
                h_content = h_content.replace(
                    '#include "esp_camera.h"',
                    '#include "esp_camera.h"\n'
                    '#if __has_include("esp_private/gdma.h")\n'
                    '# include "esp_private/gdma.h"\n'
                    '#endif',
                    1
                )
                # Add dma_channel_handle field after dma_intr_handle
                h_content = h_content.replace(
                    "    intr_handle_t dma_intr_handle;//ESP32-S3\n",
                    "    intr_handle_t dma_intr_handle;//ESP32-S3\n"
                    "#if SOC_GDMA_SUPPORTED\n"
                    "    gdma_channel_handle_t dma_channel_handle;//ESP32-S3 GDMA fix\n"
                    "#endif\n",
                    1
                )
                with open(ll_cam_h, "w") as f:
                    f.write(h_content)
                print("*** Patched ll_cam.h: added gdma_channel_handle_t ***")

        # --- Patch 3: ll_cam.c — fix GDMA channel allocation ---
        ll_cam_c = os.path.join(lib_dir, "target", "esp32s3", "ll_cam.c")
        if os.path.exists(ll_cam_c):
            with open(ll_cam_c, "r") as f:
                c_content = f.read()

            if "gdma_new_channel" not in c_content:
                # Add required includes
                c_content = c_content.replace(
                    '#include "soc/gdma_periph.h"',
                    '#include "soc/gdma_periph.h"\n'
                    '#include "hal/clk_gate_ll.h"\n'
                    '#include "esp_private/gdma.h"',
                    1
                )

                # Replace the broken ll_cam_dma_init function
                old_dma_init = '''static esp_err_t ll_cam_dma_init(cam_obj_t *cam)
{
    for (int x = (SOC_GDMA_PAIRS_PER_GROUP - 1); x >= 0; x--) {
        if (GDMA.channel[x].in.link.addr == 0x0) {
            cam->dma_num = x;
            ESP_LOGI(TAG, "DMA Channel=%d", cam->dma_num);
            break;
        }
        if (x == 0) {
            cam_deinit();
            ESP_LOGE(TAG, "Can't found available GDMA channel");
\t\t\treturn ESP_FAIL;
        }
    }

    if (REG_GET_BIT(SYSTEM_PERIP_CLK_EN1_REG, SYSTEM_DMA_CLK_EN) == 0) {
        REG_CLR_BIT(SYSTEM_PERIP_CLK_EN1_REG, SYSTEM_DMA_CLK_EN);
        REG_SET_BIT(SYSTEM_PERIP_CLK_EN1_REG, SYSTEM_DMA_CLK_EN);
        REG_SET_BIT(SYSTEM_PERIP_RST_EN1_REG, SYSTEM_DMA_RST);
        REG_CLR_BIT(SYSTEM_PERIP_RST_EN1_REG, SYSTEM_DMA_RST);
    }

    GDMA.channel[cam->dma_num].in.int_clr.val = ~0;
    GDMA.channel[cam->dma_num].in.int_ena.val = 0;

    GDMA.channel[cam->dma_num].in.conf0.val = 0;
    GDMA.channel[cam->dma_num].in.conf0.in_rst = 1;
    GDMA.channel[cam->dma_num].in.conf0.in_rst = 0;

    //internal SRAM only
    if (!cam->psram_mode) {
        GDMA.channel[cam->dma_num].in.conf0.indscr_burst_en = 1;
        GDMA.channel[cam->dma_num].in.conf0.in_data_burst_en = 1;
    }

    GDMA.channel[cam->dma_num].in.conf1.in_check_owner = 0;
    // GDMA.channel[cam->dma_num].in.conf1.in_ext_mem_bk_size = 2;

    GDMA.channel[cam->dma_num].in.peri_sel.sel = 5;
    //GDMA.channel[cam->dma_num].in.pri.rx_pri = 1;//rx prio 0-15
    //GDMA.channel[cam->dma_num].in.sram_size.in_size = 6;//This register is used to configure the size of L2 Tx FIFO for Rx channel. 0:16 bytes, 1:24 bytes, 2:32 bytes, 3: 40 bytes, 4: 48 bytes, 5:56 bytes, 6: 64 bytes, 7: 72 bytes, 8: 80 bytes.
    //GDMA.channel[cam->dma_num].in.wight.rx_weight = 7;//The weight of Rx channel 0-15
    return ESP_OK;
}'''

                new_dma_init = '''static esp_err_t ll_cam_dma_init(cam_obj_t *cam)
{
    // Use proper ESP-IDF GDMA API to allocate channel.
    // The old code used raw register checks (GDMA.channel[x].in.link.addr == 0x0)
    // which the GDMA driver doesn't know about — WiFi/BLE would steal the channel.
    // Fix from: https://github.com/espressif/esp32-camera/issues/620
    gdma_channel_alloc_config_t rx_alloc_config = {
        .direction = GDMA_CHANNEL_DIRECTION_RX,
    };
    esp_err_t ret = gdma_new_channel(&rx_alloc_config, &cam->dma_channel_handle);
    if (ret != ESP_OK) {
        cam_deinit();
        ESP_LOGE(TAG, "Can't find available GDMA channel");
        return ESP_FAIL;
    }
    int chan_id = -1;
    ret = gdma_get_channel_id(cam->dma_channel_handle, &chan_id);
    if (ret != ESP_OK) {
        cam_deinit();
        ESP_LOGE(TAG, "Can't get GDMA channel number");
        return ESP_FAIL;
    }
    cam->dma_num = chan_id;
    ESP_LOGI(TAG, "DMA Channel=%d", cam->dma_num);

    if (!periph_ll_periph_enabled(PERIPH_GDMA_MODULE)) {
        periph_ll_disable_clk_set_rst(PERIPH_GDMA_MODULE);
        periph_ll_enable_clk_clear_rst(PERIPH_GDMA_MODULE);
    }

    GDMA.channel[cam->dma_num].in.int_clr.val = ~0;
    GDMA.channel[cam->dma_num].in.int_ena.val = 0;

    GDMA.channel[cam->dma_num].in.conf0.val = 0;
    GDMA.channel[cam->dma_num].in.conf0.in_rst = 1;
    GDMA.channel[cam->dma_num].in.conf0.in_rst = 0;

    //internal SRAM only
    if (!cam->psram_mode) {
        GDMA.channel[cam->dma_num].in.conf0.indscr_burst_en = 1;
        GDMA.channel[cam->dma_num].in.conf0.in_data_burst_en = 1;
    }

    GDMA.channel[cam->dma_num].in.conf1.in_check_owner = 0;

    GDMA.channel[cam->dma_num].in.peri_sel.sel = 5;
    return ESP_OK;
}'''
                c_content = c_content.replace(old_dma_init, new_dma_init)

                # Fix ll_cam_deinit to use gdma_del_channel instead of raw register clear
                old_deinit_line = '    GDMA.channel[cam->dma_num].in.link.addr = 0x0;'
                new_deinit_line = ('    if (cam->dma_channel_handle) {\n'
                                   '        gdma_del_channel(cam->dma_channel_handle);\n'
                                   '        cam->dma_channel_handle = NULL;\n'
                                   '    }')
                c_content = c_content.replace(old_deinit_line, new_deinit_line)

                with open(ll_cam_c, "w") as f:
                    f.write(c_content)
                print("*** Patched ll_cam.c: GDMA channel allocation fix applied ***")

        # --- Patch 4: ov5640.c — clamp framesize instead of error ---
        # https://github.com/espressif/esp32-camera/pull/810
        ov5640_c = os.path.join(lib_dir, "sensors", "ov5640.c")
        if os.path.exists(ov5640_c):
            with open(ov5640_c, "r") as f:
                ov_content = f.read()

            old_check = (
                '    if(framesize > FRAMESIZE_QSXGA){\n'
                '        ESP_LOGE(TAG, "Invalid framesize: %u", framesize);\n'
                '        return -1;\n'
                '    }'
            )
            new_check = (
                '    if(framesize > FRAMESIZE_QSXGA){\n'
                '        ESP_LOGW(TAG, "Invalid framesize: %u, clamping to max", framesize);\n'
                '        framesize = FRAMESIZE_QSXGA;\n'
                '        sensor->status.framesize = framesize;\n'
                '    }'
            )
            if old_check in ov_content:
                ov_content = ov_content.replace(old_check, new_check)
                with open(ov5640_c, "w") as f:
                    f.write(ov_content)
                print("*** Patched ov5640.c: clamp framesize to 5MP (PR #810) ***")

            # --- Patch 5 REVERTED: keep stock ov5640.c PLL settings ---
            # Previous versions modified the PLL to double PCLK for higher
            # streaming FPS, but for still-image point-and-shoot captures
            # the stock PLL is stable at all temperatures.
            # The custom PLL pushed the VCO into a marginal region where
            # the phase-lock range shrank as the chip heated up, causing
            # intermittent capture failures (NULL returns from
            # esp_camera_fb_get()). Reverting to stock for thermal stability.
            #
            # If a future change needs higher FPS streaming, prefer
            # increasing XCLK from 20→40MHz over PLL tweaks.

            # Write any other changes made to ov5640.c (none currently)
            with open(ov5640_c, "w") as f:
                f.write(ov_content)

        return

    print("WARNING: esp32-camera library not found — patches not applied")

# Run immediately during script import
patch_now()
