cmake_minimum_required(VERSION 3.16)

project(metapuppet_stream_sender C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(metapuppet_stream_sender
    src/mps_frame_envelope.c
    src/mps_i420_foreground_mask.c
    src/mps_i420_sbs.c
    src/mps_latency_gate.c
    src/mps_packet_writer.c
    src/mps_udp_sender.c
)

target_include_directories(metapuppet_stream_sender
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(metapuppet_stream_sender PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(mps_pcvr_test_sender
    src/mps_pcvr_test_sender.c
)

target_link_libraries(mps_pcvr_test_sender PRIVATE metapuppet_stream_sender)

if(UNIX AND NOT APPLE)
    if(NOT TARGET imt)
        add_subdirectory(
            ${CMAKE_CURRENT_SOURCE_DIR}/../imt
            ${CMAKE_CURRENT_BINARY_DIR}/imt
        )
    endif()

    add_executable(mps_rpicam_sbs_sender
        src/mps_rpicam_sbs_sender.c
    )

    target_link_libraries(mps_rpicam_sbs_sender PRIVATE metapuppet_stream_sender)

    add_executable(mps_imt_rpicam_sbs_sender
        src/mps_imt_rpicam_sbs_sender.c
    )
    set_target_properties(mps_imt_rpicam_sbs_sender PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(mps_imt_rpicam_sbs_sender PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    target_link_libraries(mps_imt_rpicam_sbs_sender
        PRIVATE
            metapuppet_stream_sender
            imt
            pthread
    )

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)
    pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-app-1.0
    )

    add_executable(mps_libcamera_sbs_sender
        src/mps_libcamera_sbs_sender.cpp
    )

    target_link_libraries(mps_libcamera_sbs_sender
        PRIVATE
            metapuppet_stream_sender
            PkgConfig::LIBCAMERA
            PkgConfig::GSTREAMER
            pthread
    )
    target_compile_definitions(mps_libcamera_sbs_sender PRIVATE MPS_ENABLE_GSTREAMER=1)
endif()

if(WIN32)
    target_link_libraries(metapuppet_stream_sender PUBLIC ws2_32)
endif()
