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
