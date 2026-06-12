cmake_minimum_required(VERSION 3.16)

project(imt C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(imt
    src/imt_assembler.c
    src/imt_fec.c
    src/imt_map.c
    src/imt_pace.c
    src/imt_packetize.c
    src/imt_qp.c
    src/imt_sched.c
    src/imt_wire.c
)

target_include_directories(imt
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(imt PRIVATE /W4)
else()
    target_compile_options(imt PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(imt PUBLIC m)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()

    add_executable(imt_tests
        tests/imt_tests.c
    )
    target_link_libraries(imt_tests PRIVATE imt)

    add_executable(imt_trace_replay
        tools/imt_trace_replay.c
    )
    target_link_libraries(imt_trace_replay PRIVATE imt)

    if(MSVC)
        target_compile_options(imt_tests PRIVATE /W4)
        target_compile_options(imt_trace_replay PRIVATE /W4)
    else()
        target_compile_options(imt_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(imt_trace_replay PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME imt_tests COMMAND imt_tests)
endif()
