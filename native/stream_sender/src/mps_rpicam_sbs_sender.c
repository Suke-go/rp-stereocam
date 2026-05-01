#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "mps_udp_sender.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#error "mps_rpicam_sbs_sender is intended to run on Raspberry Pi/Linux."
#endif

static uint64_t mps_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int read_exact(FILE* pipe, uint8_t* data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const size_t n = fread(data + offset, 1u, size - offset, pipe);
        if (n == 0u) {
            if (ferror(pipe)) {
                return -errno;
            }
            return -1;
        }
        offset += n;
    }
    return 0;
}

static uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0u;
    }
    if (value > 255) {
        return 255u;
    }
    return (uint8_t)value;
}

static void yuv420_eye_to_rgba_sbs(const uint8_t* yuv,
                                   uint8_t* rgba,
                                   uint32_t eye_width,
                                   uint32_t eye_height,
                                   uint32_t dest_x_offset)
{
    const size_t y_plane_size = (size_t)eye_width * eye_height;
    const uint8_t* y_plane = yuv;
    const uint8_t* u_plane = yuv + y_plane_size;
    const uint8_t* v_plane = u_plane + y_plane_size / 4u;
    const uint32_t sbs_width = eye_width * 2u;
    uint32_t y;

    for (y = 0; y < eye_height; ++y) {
        uint32_t x;
        for (x = 0; x < eye_width; ++x) {
            const uint32_t chroma_index = (y / 2u) * (eye_width / 2u) + (x / 2u);
            const int yy = (int)y_plane[(size_t)y * eye_width + x];
            const int uu = (int)u_plane[chroma_index] - 128;
            const int vv = (int)v_plane[chroma_index] - 128;
            const int c = yy - 16;
            const int d = uu;
            const int e = vv;
            const int r = (298 * c + 409 * e + 128) >> 8;
            const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            const int b = (298 * c + 516 * d + 128) >> 8;
            const size_t out = ((size_t)y * sbs_width + dest_x_offset + x) * 4u;
            rgba[out + 0u] = clamp_u8(r);
            rgba[out + 1u] = clamp_u8(g);
            rgba[out + 2u] = clamp_u8(b);
            rgba[out + 3u] = 255u;
        }
    }
}

static FILE* open_rpicam_pipe(uint32_t camera_index, uint32_t width, uint32_t height, uint32_t fps, const char* log_path)
{
    char command[512];
    const int written = snprintf(command,
                                 sizeof(command),
                                 "rpicam-vid --camera %u --width %u --height %u --framerate %u "
                                 "--codec yuv420 --timeout 0 --nopreview --output - 2>%s",
                                 camera_index,
                                 width,
                                 height,
                                 fps,
                                 log_path);
    if (written < 0 || (size_t)written >= sizeof(command)) {
        return NULL;
    }
    return popen(command, "r");
}

int main(int argc, char** argv)
{
    const char* host = argc > 1 ? argv[1] : "192.168.137.1";
    const uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : 5004u;
    const uint32_t eye_width = argc > 3 ? (uint32_t)atoi(argv[3]) : 640u;
    const uint32_t eye_height = argc > 4 ? (uint32_t)atoi(argv[4]) : 360u;
    const uint32_t fps = argc > 5 ? (uint32_t)atoi(argv[5]) : 30u;
    const uint32_t left_camera = argc > 6 ? (uint32_t)atoi(argv[6]) : 1u;
    const uint32_t right_camera = argc > 7 ? (uint32_t)atoi(argv[7]) : 0u;
    const size_t yuv_size = (size_t)eye_width * eye_height * 3u / 2u;
    const size_t rgba_size = (size_t)eye_width * 2u * eye_height * 4u;
    uint8_t* left_yuv = NULL;
    uint8_t* right_yuv = NULL;
    uint8_t* sbs_rgba = NULL;
    FILE* left_pipe = NULL;
    FILE* right_pipe = NULL;
    MpsUdpSender sender;
    uint64_t frame_index = 0;
    int rc = 0;

    if (eye_width < 160u || eye_height < 120u || (eye_width & 1u) != 0u || (eye_height & 1u) != 0u || fps == 0u) {
        fprintf(stderr,
                "usage: %s [host] [port] [eye_width] [eye_height] [fps] [left_camera] [right_camera]\n",
                argv[0]);
        return 1;
    }
    if (left_camera == right_camera) {
        fprintf(stderr, "left_camera and right_camera must differ\n");
        return 1;
    }

    left_yuv = (uint8_t*)malloc(yuv_size);
    right_yuv = (uint8_t*)malloc(yuv_size);
    sbs_rgba = (uint8_t*)malloc(rgba_size);
    if (!left_yuv || !right_yuv || !sbs_rgba) {
        rc = 2;
        goto cleanup;
    }

    rc = mps_udp_sender_open(&sender, host, port, 2u);
    if (rc != 0) {
        fprintf(stderr, "sender open failed: %d\n", rc);
        rc = 3;
        goto cleanup;
    }

    left_pipe = open_rpicam_pipe(left_camera, eye_width, eye_height, fps, "/tmp/metapuppet_left_rpicam.log");
    right_pipe = open_rpicam_pipe(right_camera, eye_width, eye_height, fps, "/tmp/metapuppet_right_rpicam.log");
    if (!left_pipe || !right_pipe) {
        fprintf(stderr, "failed to start rpicam-vid pipes\n");
        rc = 4;
        goto cleanup;
    }

    fprintf(stderr,
            "streaming live cameras to %s:%u left=%u right=%u eye=%ux%u sbs=%ux%u@%u\n",
            host,
            port,
            left_camera,
            right_camera,
            eye_width,
            eye_height,
            eye_width * 2u,
            eye_height,
            fps);

    for (;;) {
        rc = read_exact(left_pipe, left_yuv, yuv_size);
        if (rc != 0) {
            fprintf(stderr, "left camera read failed: %d; see /tmp/metapuppet_left_rpicam.log\n", rc);
            break;
        }
        rc = read_exact(right_pipe, right_yuv, yuv_size);
        if (rc != 0) {
            fprintf(stderr, "right camera read failed: %d; see /tmp/metapuppet_right_rpicam.log\n", rc);
            break;
        }

        yuv420_eye_to_rgba_sbs(left_yuv, sbs_rgba, eye_width, eye_height, 0u);
        yuv420_eye_to_rgba_sbs(right_yuv, sbs_rgba, eye_width, eye_height, eye_width);

        rc = mps_udp_sender_send_frame(&sender,
                                       sbs_rgba,
                                       rgba_size,
                                       frame_index,
                                       mps_now_ns(),
                                       MPS_PACKET_FLAG_RAW_RGBA | MPS_PACKET_FLAG_KEYFRAME);
        if (rc != 0) {
            fprintf(stderr, "send failed: %d\n", rc);
            break;
        }
        frame_index += 1u;
    }

cleanup:
    if (left_pipe) {
        pclose(left_pipe);
    }
    if (right_pipe) {
        pclose(right_pipe);
    }
    mps_udp_sender_close(&sender);
    free(left_yuv);
    free(right_yuv);
    free(sbs_rgba);
    return rc == 0 ? 0 : rc;
}
