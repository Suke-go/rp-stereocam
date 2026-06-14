#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef _WIN32
#error "mps_imt_test_sender targets Linux/Pi only."
#endif

#include "imt.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int fd;
    struct sockaddr_in addr;
} SendCtx;

static int emit_datagram(const uint8_t* data, size_t len, void* user)
{
    SendCtx* ctx = (SendCtx*)user;
    ssize_t sent = sendto(ctx->fd, data, len, 0,
                          (const struct sockaddr*)&ctx->addr, sizeof(ctx->addr));
    return (sent == (ssize_t)len) ? 0 : -1;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void fill_sbs_rgba(uint8_t* frame, uint32_t width, uint32_t height, uint64_t idx)
{
    const uint32_t half = width / 2u;
    const uint32_t marker = (uint32_t)((idx * 7u) % half);
    uint32_t y;
    for (y = 0; y < height; ++y) {
        uint32_t x;
        for (x = 0; x < width; ++x) {
            const uint32_t i = (y * width + x) * 4u;
            const uint32_t band = ((x / 32u) + (y / 32u)) & 1u;
            const int left = (x < half);
            uint8_t r = left ? 16u : 190u;
            uint8_t g = left ? 60u : 24u;
            uint8_t b = left ? 210u : 18u;
            if (band) { r /= 2u; g /= 2u; b /= 2u; }
            if ((left && x >= marker && x < marker + 8u) ||
                (!left && x >= half + marker && x < half + marker + 8u)) {
                r = 255u; g = 255u; b = 255u;
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
    const char* host    = argc > 1 ? argv[1]                    : "192.168.2.198";
    const uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2])   : 5004u;
    const uint32_t width  = argc > 3 ? (uint32_t)atoi(argv[3]) : 1280u;
    const uint32_t height = argc > 4 ? (uint32_t)atoi(argv[4]) : 360u;
    const uint32_t fps    = argc > 5 ? (uint32_t)atoi(argv[5]) : 30u;

    if (width < 2u || height < 2u || fps == 0u) {
        fprintf(stderr, "usage: %s [host] [port] [width] [height] [fps]\n", argv[0]);
        return 1;
    }

    const size_t frame_size = (size_t)width * height * 4u;
    uint8_t* frame = (uint8_t*)malloc(frame_size);
    if (!frame) { return 2; }

    ImtMap map;
    if (imt_map_init(&map, width, height, 32u) != 0) { free(frame); return 3; }
    imt_map_fill(&map, 128u);

    ImtPacketizer pkt;
    if (imt_packetizer_init(&pkt, 0, 0) != 0) {
        imt_map_destroy(&map); free(frame); return 4;
    }

    SendCtx ctx;
    ctx.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx.fd < 0) {
        imt_packetizer_destroy(&pkt); imt_map_destroy(&map); free(frame); return 5;
    }
    memset(&ctx.addr, 0, sizeof(ctx.addr));
    ctx.addr.sin_family = AF_INET;
    ctx.addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &ctx.addr.sin_addr) != 1) {
        close(ctx.fd); imt_packetizer_destroy(&pkt); imt_map_destroy(&map); free(frame); return 6;
    }

    fprintf(stderr, "mps-imt-test-sender → %s:%u  %ux%u RGBA @ %u fps\n",
            host, port, width, height, fps);

    const long frame_ns = (long)(1000000000UL / fps);
    uint64_t frame_seq = 0;
    for (;;) {
        fill_sbs_rgba(frame, width, height, frame_seq);
        imt_packetizer_send_frame(&pkt,
                                  frame, (uint32_t)frame_size,
                                  frame_seq, now_ns(),
                                  IMT_PACKET_FLAG_KEYFRAME,
                                  NULL, 0,
                                  emit_datagram, &ctx);
        frame_seq += 1u;
        struct timespec ts = {0, frame_ns};
        nanosleep(&ts, NULL);
    }

    close(ctx.fd);
    imt_packetizer_destroy(&pkt);
    imt_map_destroy(&map);
    free(frame);
    return 0;
}
