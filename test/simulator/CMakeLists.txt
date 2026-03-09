cmake_minimum_required(VERSION 3.16)
project(p4eye_sim C)

set(CMAKE_C_STANDARD 11)

# -----------------------------------------------------------------
# Paths
# -----------------------------------------------------------------
set(SIM_DIR    ${CMAKE_CURRENT_SOURCE_DIR})
set(REPO_ROOT  ${SIM_DIR}/../..)
set(UI_DIR     ${REPO_ROOT}/factory_demo/main/ui)
set(MOCK_DIR   ${REPO_ROOT}/test/mocks)
set(LVGL_DIR   ${SIM_DIR}/lvgl)
set(DRV_DIR    ${SIM_DIR}/lv_drivers)

# -----------------------------------------------------------------
# SDL2
# -----------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# -----------------------------------------------------------------
# LVGL core sources (all .c under lvgl/src/)
# -----------------------------------------------------------------
file(GLOB_RECURSE LVGL_SRCS ${LVGL_DIR}/src/*.c)

# -----------------------------------------------------------------
# lv_drivers SDL backend
# -----------------------------------------------------------------
set(DRV_SRCS
    ${DRV_DIR}/sdl/sdl.c
    ${DRV_DIR}/sdl/sdl_common.c
)

# -----------------------------------------------------------------
# Real UI sources from factory_demo
# -----------------------------------------------------------------
file(GLOB UI_SCREEN_SRCS   ${UI_DIR}/screens/*.c)
file(GLOB UI_FONT_SRCS     ${UI_DIR}/fonts/*.c)
file(GLOB UI_IMAGE_SRCS    ${UI_DIR}/images/*.c)

set(UI_SRCS
    ${UI_DIR}/ui.c
    ${UI_DIR}/ui_extra.c
    ${UI_DIR}/ui_helpers.c
    ${UI_DIR}/components/ui_comp_hook.c
    ${UI_SCREEN_SRCS}
    ${UI_FONT_SRCS}
    ${UI_IMAGE_SRCS}
)

# -----------------------------------------------------------------
# Simulator sources
# -----------------------------------------------------------------
set(SIM_SRCS
    ${SIM_DIR}/sim_main.c
    ${SIM_DIR}/sim_hal.c
)

# -----------------------------------------------------------------
# Build the simulator executable
# -----------------------------------------------------------------
add_executable(p4eye_sim
    ${SIM_SRCS}
    ${UI_SRCS}
    ${LVGL_SRCS}
    ${DRV_SRCS}
)

# -----------------------------------------------------------------
# Include paths — order matters for header resolution
# -----------------------------------------------------------------
target_include_directories(p4eye_sim PRIVATE
    # Simulator config (lv_conf.h, lv_drv_conf.h) — MUST come first
    ${SIM_DIR}

    # LVGL core headers
    ${LVGL_DIR}
    ${LVGL_DIR}/src

    # lv_drivers headers
    ${DRV_DIR}

    # Real UI headers
    ${UI_DIR}

    # Mock ESP-IDF headers (esp_err.h, esp_log.h, freertos/, etc.)
    ${MOCK_DIR}

    # App headers — the real ones from factory_demo
    ${REPO_ROOT}/factory_demo/main/app
    ${REPO_ROOT}/factory_demo/main/app/Video

    # SDL2
    ${SDL2_INCLUDE_DIRS}
)

# -----------------------------------------------------------------
# Compile definitions
# -----------------------------------------------------------------
target_compile_definitions(p4eye_sim PRIVATE
    # Tell LVGL where to find lv_conf.h
    LV_CONF_INCLUDE_SIMPLE

    # lv_drivers picks up lv_drv_conf.h via LV_CONF_INCLUDE_SIMPLE

    # Tell lv_drivers we include lvgl.h directly (no subdir)
    LV_LVGL_H_INCLUDE_SIMPLE

    # Simulator identifier
    SIMULATOR=1
)

# -----------------------------------------------------------------
# Compiler flags
# -----------------------------------------------------------------
target_compile_options(p4eye_sim PRIVATE
    -Wall
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-field-initializers
    -Wno-sign-compare
    # Suppress warnings from generated SquareLine code and LVGL internals
    -Wno-implicit-function-declaration
    -Wno-int-conversion
    -Wno-pointer-to-int-cast
    -Wno-type-limits
    -O2
)

# -----------------------------------------------------------------
# Link
# -----------------------------------------------------------------
target_link_libraries(p4eye_sim PRIVATE
    ${SDL2_LIBRARIES}
    m
    pthread
)
