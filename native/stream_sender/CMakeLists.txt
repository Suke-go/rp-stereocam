cmake_minimum_required(VERSION 3.16)

project(metapuppet_stream_sender C CXX)

include(CTest)

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

    option(MPS_ENABLE_X264 "Enable H.264 encoding via libx264" OFF)
    if(MPS_ENABLE_X264)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(X264 REQUIRED x264)
        target_sources(mps_imt_rpicam_sbs_sender PRIVATE src/mps_x264_encoder.c)
        target_include_directories(mps_imt_rpicam_sbs_sender PRIVATE ${X264_INCLUDE_DIRS})
        target_link_libraries(mps_imt_rpicam_sbs_sender PRIVATE ${X264_LIBRARIES})
        target_compile_definitions(mps_imt_rpicam_sbs_sender PRIVATE MPS_X264_ENABLED=1)
    endif()

    # ── Dual independent-eye H.264 sender (no SBS, no importance map) ──────
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X264_STEREO REQUIRED x264)

    add_executable(mps_stereo_x264_sender
        src/mps_stereo_x264_sender.c
        src/mps_x264_encoder.c
    )
    set_target_properties(mps_stereo_x264_sender PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(mps_stereo_x264_sender PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    target_include_directories(mps_stereo_x264_sender PRIVATE
        ${X264_STEREO_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(mps_stereo_x264_sender
        PRIVATE
            imt
            pthread
            ${X264_STEREO_LIBRARIES}
    )

    if(BUILD_TESTING)
        add_executable(mps_x264_encoder_test
            tests/mps_x264_encoder_test.c
            src/mps_x264_encoder.c
        )
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(mps_x264_encoder_test PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        target_include_directories(mps_x264_encoder_test PRIVATE
            ${X264_STEREO_INCLUDE_DIRS}
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
        target_link_libraries(mps_x264_encoder_test PRIVATE
            imt
            ${X264_STEREO_LIBRARIES}
        )
        add_test(NAME mps_x264_encoder_test COMMAND mps_x264_encoder_test)
    endif()

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)
    pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-app-1.0
    )

    target_sources(mps_stereo_x264_sender PRIVATE
        src/mps_libcamera_stereo_capture.cpp
    )
    target_link_libraries(mps_stereo_x264_sender PRIVATE PkgConfig::LIBCAMERA)

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

    # ── RVM / ONNX Runtime (optional) ──────────────────────────────────────
    option(MPS_ENABLE_RVM "Enable RVM person segmentation via ONNX Runtime" OFF)

    if(MPS_ENABLE_RVM)
        if(NOT DEFINED ONNXRUNTIME_ROOT)
            set(ONNXRUNTIME_ROOT "/usr/local" CACHE PATH "ONNX Runtime install prefix")
        endif()
        find_library(ORT_LIB onnxruntime
            PATHS "${ONNXRUNTIME_ROOT}/lib"
                  "${ONNXRUNTIME_ROOT}/lib/aarch64-linux-gnu"
                  "${ONNXRUNTIME_ROOT}/lib/x86_64-linux-gnu"
            NO_DEFAULT_PATH)
        if(NOT ORT_LIB)
            find_library(ORT_LIB onnxruntime)  # fallback: let CMake search defaults
        endif()
        find_path(ORT_INC onnxruntime_c_api.h
            PATHS "${ONNXRUNTIME_ROOT}/include"
                  "${ONNXRUNTIME_ROOT}/include/onnxruntime"
            NO_DEFAULT_PATH)
        if(ORT_LIB AND ORT_INC)
            message(STATUS "RVM enabled: ${ORT_LIB}")
            target_sources(mps_imt_rpicam_sbs_sender PRIVATE
                src/mps_rvm_importance.c)
            target_include_directories(mps_imt_rpicam_sbs_sender PRIVATE
                include "${ORT_INC}")
            target_link_libraries(mps_imt_rpicam_sbs_sender PRIVATE "${ORT_LIB}" m)
            target_compile_definitions(mps_imt_rpicam_sbs_sender PRIVATE
                MPS_RVM_ENABLED=1)
        else()
            message(WARNING "MPS_ENABLE_RVM=ON but ONNX Runtime not found at ${ONNXRUNTIME_ROOT}")
        endif()
    endif()
endif()

if(WIN32)
    target_link_libraries(metapuppet_stream_sender PUBLIC ws2_32)
endif()
