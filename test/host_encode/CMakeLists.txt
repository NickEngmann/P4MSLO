cmake_minimum_required(VERSION 3.13)
project(p4mslo_host_encode C)

set(CMAKE_C_STANDARD 11)

# AddressSanitizer + UBSan by default — these are the local proxy for
# the on-device tlsf integrity panic. Add -fsanitize=thread separately
# when validating the shared-tjpgd-workspace mutex.
set(SAN_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SAN_FLAGS} -Wall -Wno-unused-but-set-variable -Wno-format -Wno-unused-variable -Wno-deprecated-declarations")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SAN_FLAGS}")

set(GIF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../factory_demo/main/app/Gif")
set(MOCK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mocks")

add_executable(host_encode
    host_encode_main.c
    gif_simd_stub.c
    ${GIF_DIR}/gif_encoder.c
    ${GIF_DIR}/gif_lzw.c
    ${GIF_DIR}/gif_quantize.c
    ${GIF_DIR}/gif_tjpgd.c
)
target_include_directories(host_encode PRIVATE
    ${MOCK_DIR}
    ${GIF_DIR}
)
target_link_libraries(host_encode PRIVATE m)

add_executable(test_budget
    test_budget.c
    ${MOCK_DIR}/p4_mem_model.c
    ${MOCK_DIR}/p4_budget.c
)
target_include_directories(test_budget PRIVATE ${MOCK_DIR})

add_executable(test_phases
    test_phases.c
    ${MOCK_DIR}/p4_mem_model.c
    ${MOCK_DIR}/p4_phases.c
)
target_include_directories(test_phases PRIVATE ${MOCK_DIR})
