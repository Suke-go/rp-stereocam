cmake_minimum_required(VERSION 3.16)

project(mp_edge_camera C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MP_EDGE_ENABLE_NEON "Enable ARM NEON optimized code paths" ON)

add_library(mp_edge_core
    src/mp_clock_sync.c
    src/mp_edge_config.c
    src/mp_foreground.c
    src/mp_frame_ring.c
    src/mp_mask.c
    src/mp_preprocess.c
    src/mp_sbs_packer.c
    src/mp_security.c
    src/mp_stereo_sync.c
    src/mp_sync_monitor.c
    src/mp_stats.c
)

target_include_directories(mp_edge_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MP_EDGE_ENABLE_NEON)
    target_compile_definitions(mp_edge_core PUBLIC MP_EDGE_ENABLE_NEON=1)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mp_edge_core PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|arm")
        target_compile_options(mp_edge_core PRIVATE -O3)
    endif()
elseif(MSVC)
    target_compile_options(mp_edge_core PRIVATE /std:c11 /experimental:c11atomics)
endif()

add_executable(mp_edge_camera src/main.c)
target_link_libraries(mp_edge_camera PRIVATE mp_edge_core)

if(MSVC)
    target_compile_options(mp_edge_camera PRIVATE /std:c11 /experimental:c11atomics)
endif()
