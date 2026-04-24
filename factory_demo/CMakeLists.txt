# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Firmware version via git describe. Priority:
#   1. P4MSLO_FIRMWARE_VERSION env var (useful when building inside Docker
#      where git isn't available — host passes it in)
#   2. `git describe` from the repo (native builds)
#   3. "unknown" fallback
if(DEFINED ENV{P4MSLO_FIRMWARE_VERSION})
    set(GIT_DESCRIBE $ENV{P4MSLO_FIRMWARE_VERSION})
else()
    execute_process(
        COMMAND git describe --always --dirty=+ --tags
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
        OUTPUT_VARIABLE GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT GIT_DESCRIBE)
    set(GIT_DESCRIBE "unknown")
endif()
add_compile_definitions(P4MSLO_FIRMWARE_VERSION="${GIT_DESCRIBE}")
message(STATUS "Firmware version: ${GIT_DESCRIBE}")

set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(factory_demo VERSION 1.0.0)

# esp-dl warning suppression was here — dropped along with the
# coco_detect / human_face_detect / pedestrian_detect components
# when AI detection was removed from this build. If AI ever comes
# back, restore the target_compile_options block.

# --- Selective -O2 on specific hot-path source files ------------------
# Global optimization is -Og (set in sdkconfig.defaults). A global -O2
# deterministically hangs taskLVGL in `lv_refr.c:709` busy-waiting on
# `draw_buf->flushing`. Forcing LVGL, esp_lvgl_port, esp_lcd, and
# esp_driver_spi back to -Og under global -O2 did not un-stick it.
# Even applying -O2 to just the main component re-introduced a wdt
# hang on gallery-nav + concurrent-encode paths.
#
# Scope the optimization down to the specific inner loops that matter:
# tjpgd (JPEG decode — MCU loop + IDCT), the LZW GIF encoder (palette
# lookup + bit-packing), and the LZW GIF decoder. These are pure
# number-crunching files with no LVGL / display-driver interaction, so
# pumping them to -O2 is safe. tjpgd decode drops from ~2.2 s → ~1.7 s
# per 2560x1920 frame under just this change.
set(P4MSLO_O2_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main/app/Gif/gif_tjpgd.c      # JPEG decode
    ${CMAKE_CURRENT_SOURCE_DIR}/main/app/Gif/gif_encoder.c    # LZW encode
    ${CMAKE_CURRENT_SOURCE_DIR}/main/app/Gif/gif_decoder.c    # LZW decode
    ${CMAKE_CURRENT_SOURCE_DIR}/main/app/Gif/gif_lzw.c        # LZW core
    ${CMAKE_CURRENT_SOURCE_DIR}/main/app/Gif/gif_quantize.c   # palette quant
    ${CMAKE_CURRENT_SOURCE_DIR}/main/app/Spi/spi_camera.c     # SPI RX polling
    # app_gifs.c — needed for jpeg_out_cb / jpeg_crop_out_cb, the tjpgd
    # per-MCU output callbacks that run 19,200 times per 2560×1920
    # JPEG. At -Og they took ~30 s per decode; at -O2 the same math
    # lands in ~2 s (same as gif_encoder.c's tjpgd_output, which has
    # always been on this list). The file is large and has LVGL
    # surface, but per-file -O2 is scoped narrowly (CLAUDE.md's "whole
    # main component -O2 wdt-hangs" warning is about the MAIN_WIDE
    # toggle, not selective per-file application). If a regression
    # surfaces, the minimal rollback is to move just the callbacks
    # into a small helper file compiled -O2, leaving the LVGL-heavy
    # parts of app_gifs.c at -Og.
    ${CMAKE_CURRENT_SOURCE_DIR}/main/app/Gif/app_gifs.c
)
set_source_files_properties(${P4MSLO_O2_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-O2")
list(LENGTH P4MSLO_O2_SOURCES n_o2)
message(STATUS "Selective -O2 on ${n_o2} hot-path source files")
