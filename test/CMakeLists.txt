cmake_minimum_required(VERSION 3.16)
project(p4mslo_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Add mock headers
include_directories(${CMAKE_SOURCE_DIR}/mocks)

# Test files
set(TEST_SOURCES
    test_nvs_storage.c
    test_gpio_bsp.c
    test_ui_state.c
    test_ai_buffers.c
    test_sleep_wakeup.c
)

# Create test executables
foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_link_libraries(${TEST_NAME} m)
    set_target_properties(${TEST_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endforeach()

# Add simulator test
add_subdirectory(simulator)

# Custom target to run all tests
add_custom_target(run_tests
    COMMAND ${CMAKE_COMMAND} -E echo "Running all tests..."
    COMMAND for t in ${CMAKE_BINARY_DIR}/test_*; do [ -x "$$t" ] && echo "Running $$t" && $$t || true; done
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS ${TEST_SOURCES}
    COMMENT "Running all unit tests"
)
