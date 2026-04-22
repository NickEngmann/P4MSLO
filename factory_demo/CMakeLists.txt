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

# Suppress warnings only for esp-dl component
idf_component_get_property(esp_dl_lib espressif__esp-dl COMPONENT_LIB)
if(esp_dl_lib)
    target_compile_options(${esp_dl_lib} PRIVATE
        -Wno-unused-variable
        -Wno-unused-but-set-variable
    )
    message(STATUS "Applied warning suppression to esp-dl component")
endif()

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
    ${CMAKE_CURRENT_SOURCE_DIR}/main/app/Gif/gif_tjpgd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/main/app/Gif/gif_encoder.c
    ${CMAKE_CURRENT_SOURCE_DIR}/main/app/Gif/gif_decoder.c
)
set_source_files_properties(${P4MSLO_O2_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-O2")
message(STATUS "Selective -O2 on 3 hot-path GIF/JPEG source files")
