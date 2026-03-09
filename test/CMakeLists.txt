cmake_minimum_required(VERSION 3.10)
project(p4mslo_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-function -Wno-unused-variable -g")

# Mock include path takes priority over system includes
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/mocks)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Each test is a standalone executable
set(TEST_SOURCES
    test_nvs_storage.c
    test_gpio_bsp.c
    test_ui_state.c
    test_ai_buffers.c
    test_sleep_wakeup.c
)

foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
endforeach()

# Custom target to run all tests
add_custom_target(run_tests
    COMMAND ${CMAKE_COMMAND} -E echo "===== Running All P4MSLO Tests ====="
    DEPENDS ${TEST_NAMES}
)

foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_custom_command(TARGET run_tests POST_BUILD
        COMMAND $<TARGET_FILE:${TEST_NAME}>
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Running ${TEST_NAME}..."
    )
endforeach()
