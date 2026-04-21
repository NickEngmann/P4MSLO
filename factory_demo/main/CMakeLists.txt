set(APP_DIR app)

# Collect app core source files
file(GLOB APP_CORE_C_SOURCES ${APP_DIR}/*.c)

# Collect AI module source files
file(GLOB APP_AI_CPP_SOURCES ${APP_DIR}/AI/*.cpp)
file(GLOB APP_AI_C_SOURCES ${APP_DIR}/AI/*.c)

# Collect Video module source files
file(GLOB APP_VIDEO_C_SOURCES ${APP_DIR}/Video/*.c)

# Collect GIF module source files
file(GLOB APP_GIF_C_SOURCES ${APP_DIR}/Gif/*.c)
file(GLOB APP_GIF_ASM_SOURCES ${APP_DIR}/Gif/*.S)

# Collect SPI camera module source files
file(GLOB APP_SPI_C_SOURCES ${APP_DIR}/Spi/*.c)

# Collect PIMSLO pipeline source files
file(GLOB APP_PIMSLO_C_SOURCES ${APP_DIR}/Pimslo/*.c)

# Collect P4 network (WiFi via C6 + HTTP + OTA) source files
file(GLOB APP_NET_C_SOURCES ${APP_DIR}/Net/*.c)

# Combine all app source files
set(APP_ALL_SOURCES
    ${APP_CORE_C_SOURCES}
    ${APP_AI_CPP_SOURCES}
    ${APP_AI_C_SOURCES}
    ${APP_VIDEO_C_SOURCES}
    ${APP_GIF_C_SOURCES}
    ${APP_GIF_ASM_SOURCES}
    ${APP_SPI_C_SOURCES}
    ${APP_PIMSLO_C_SOURCES}
    ${APP_NET_C_SOURCES}
)

set(LV_UI_DIR ui)
file(GLOB_RECURSE LV_UI_SOURCES ${LV_UI_DIR}/*.c)

idf_component_register(SRCS "main.c" ${LV_UI_SOURCES} ${APP_ALL_SOURCES}
                    INCLUDE_DIRS "." ${LV_UI_DIR} ${APP_DIR} ${APP_DIR}/AI ${APP_DIR}/Video ${APP_DIR}/Gif ${APP_DIR}/Spi ${APP_DIR}/Pimslo ${APP_DIR}/Net
                    REQUIRES esp_driver_ppa esp_driver_spi nvs_flash human_face_detect pedestrian_detect coco_detect esp_painter
                             esp_wifi esp_netif esp_http_server app_update esp_https_ota lwip)

# Optimize GIF encoder and LZW at -O2 for maximum performance (rest of project uses -Og)
set_source_files_properties(
    ${APP_DIR}/Gif/gif_encoder.c
    ${APP_DIR}/Gif/gif_lzw.c
    ${APP_DIR}/Gif/gif_quantize.c
    PROPERTIES COMPILE_FLAGS "-O3 -funroll-loops -ffast-math"
)