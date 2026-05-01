#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "mps_udp_sender.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static void mps_sleep_ms(uint32_t ms) { Sleep(ms); }
#else
#include <unistd.h>
static void mps_sleep_ms(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, 0);
}
#endif

static uint64_t mps_now_ns(void)
{
    struct timespec ts;
#ifdef _WIN32
    timespec_get(&ts, TIME_UTC);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void fill_sbs_rgba(uint8_t* frame, uint32_t width, uint32_t height, uint64_t frame_index)
{
    uint32_t y;
    const uint32_t half = width / 2u;
    const uint32_t marker = (uint32_t)((frame_index * 7u) % half);

    for (y = 0; y < height; ++y) {
        uint32_t x;
        for (x = 0; x < width; ++x) {
            const uint32_t i = ((size_t)y * width + x) * 4u;
            const uint32_t band = ((x / 32u) + (y / 32u)) & 1u;
            const int left = x < half;
            uint8_t r = left ? 16u : 190u;
            uint8_t g = left ? 60u : 24u;
            uint8_t b = left ? 210u : 18u;

            if (band) {
                r = (uint8_t)(r / 2u);
                g = (uint8_t)(g / 2u);
                b = (uint8_t)(b / 2u);
            }
            if ((left && x >= marker && x < marker + 8u) ||
                (!left && x >= half + marker && x < half + marker + 8u)) {
                r = 255u;
                g = 255u;
                b = 255u;
            }

            frame[i + 0u] = r;
            frame[i + 1u] = g;
            frame[i + 2u] = b;
            frame[i + 3u] = 255u;
        }
    }
}

int main(int argc, char** argv)
{
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : 5004u;
    const uint32_t width = argc > 3 ? (uint32_t)atoi(argv[3]) : 1280u;
    const uint32_t height = argc > 4 ? (uint32_t)atoi(argv[4]) : 360u;
    const uint32_t fps = argc > 5 ? (uint32_t)atoi(argv[5]) : 30u;
    const size_t frame_size = (size_t)width * height * 4u;
    uint8_t* frame = 0;
    MpsUdpSender sender;
    uint64_t frame_index = 0;
    int rc;

    if (width < 2u || height < 2u || fps == 0u) {
        fprintf(stderr, "usage: %s [host] [port] [width] [height] [fps]\n", argv[0]);
        return 1;
    }

    frame = (uint8_t*)malloc(frame_size);
    if (!frame) {
        return 2;
    }

    rc = mps_udp_sender_open(&sender, host, port, 1u);
    if (rc != 0) {
        fprintf(stderr, "sender open failed: %d\n", rc);
        free(frame);
        return 3;
    }

    printf("sending Raw RGBA SBS to %s:%u %ux%u@%u\n", host, port, width, height, fps);
    for (;;) {
        fill_sbs_rgba(frame, width, height, frame_index);
        rc = mps_udp_sender_send_frame(&sender,
                                       frame,
                                       frame_size,
                                       frame_index,
                                       mps_now_ns(),
                                       MPS_PACKET_FLAG_RAW_RGBA | MPS_PACKET_FLAG_KEYFRAME);
        if (rc != 0) {
            fprintf(stderr, "send failed: %d\n", rc);
            break;
        }
        frame_index += 1u;
        mps_sleep_ms(1000u / fps);
    }

    mps_udp_sender_close(&sender);
    free(frame);
    return 0;
}
