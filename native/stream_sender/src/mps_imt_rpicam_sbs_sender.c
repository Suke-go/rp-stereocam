#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef _WIN32
#error "mps_imt_rpicam_sbs_sender targets Linux/Pi only."
#endif

#include "imt.h"

#include <arpa/inet.h>
#include <errno.h>
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

static int read_exact(FILE* pipe, uint8_t* data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const size_t n = fread(data + offset, 1u, size - offset, pipe);
        if (n == 0u) {
            return ferror(pipe) ? -errno : -1;
        }
        offset += n;
    }
    return 0;
}

static uint8_t clamp_u8(int v)
{
    return (v < 0) ? 0u : (v > 255) ? 255u : (uint8_t)v;
}

/* xform bits: 0=none, 1=flip_h, 2=flip_v, 3=rotate180 (flip_h|flip_v) */
static void yuv420_to_rgba_sbs(const uint8_t* yuv,
                                uint8_t* rgba,
                                uint32_t eye_w,
                                uint32_t eye_h,
                                uint32_t dest_x,
                                int xform)
{
    const int flip_h = (xform & 1) != 0;
    const int flip_v = (xform & 2) != 0;
    const uint8_t* y_p = yuv;
    const uint8_t* u_p = yuv + (size_t)eye_w * eye_h;
    const uint8_t* v_p = u_p + (size_t)eye_w * eye_h / 4u;
    const uint32_t sbs_w = eye_w * 2u;
    uint32_t y;
    for (y = 0; y < eye_h; ++y) {
        const uint32_t sy = flip_v ? (eye_h - 1u - y) : y;
        uint32_t x;
        for (x = 0; x < eye_w; ++x) {
            const uint32_t sx = flip_h ? (eye_w - 1u - x) : x;
            const uint32_t ci = (sy / 2u) * (eye_w / 2u) + (sx / 2u);
            const int yy = y_p[(size_t)sy * eye_w + sx];
            const int c = yy - 16;
            const int d = (int)u_p[ci] - 128;
            const int e = (int)v_p[ci] - 128;
            const size_t out = ((size_t)y * sbs_w + dest_x + x) * 4u;
            rgba[out + 0u] = clamp_u8((298 * c + 409 * e + 128) >> 8);
            rgba[out + 1u] = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
            rgba[out + 2u] = clamp_u8((298 * c + 516 * d + 128) >> 8);
            rgba[out + 3u] = 255u;
        }
    }
}

static FILE* open_cam(uint32_t idx, uint32_t w, uint32_t h, uint32_t fps)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "rpicam-vid --camera %u --width %u --height %u --framerate %u "
             "--codec yuv420 --timeout 0 --nopreview --output - 2>/tmp/cam%u.log",
             idx, w, h, fps, idx);
    return popen(cmd, "r");
}

int main(int argc, char** argv)
{
    const char* host    = argc > 1 ? argv[1]                    : "192.168.137.1";
    const uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2])   : 5004u;
    const uint32_t eye_w  = argc > 3 ? (uint32_t)atoi(argv[3]) : 640u;
    const uint32_t eye_h  = argc > 4 ? (uint32_t)atoi(argv[4]) : 360u;
    const uint32_t fps    = argc > 5 ? (uint32_t)atoi(argv[5]) : 30u;
    const uint32_t left_cam   = argc > 6 ? (uint32_t)atoi(argv[6]) : 1u;
    const uint32_t right_cam  = argc > 7 ? (uint32_t)atoi(argv[7]) : 0u;
    /* xform: 0=none 1=flip_h 2=flip_v 3=rotate180 */
    const int left_xform  = argc > 8 ? atoi(argv[8])  : 0;
    const int right_xform = argc > 9 ? atoi(argv[9])  : 0;

    if (eye_w < 2u || eye_h < 2u || fps == 0u || left_cam == right_cam) {
        fprintf(stderr,
                "usage: %s [host] [port] [eye_w] [eye_h] [fps] [left_cam] [right_cam] [left_xform] [right_xform]\n"
                "  xform: 0=none 1=flip_h 2=flip_v 3=rotate180\n",
                argv[0]);
        return 1;
    }

    const size_t yuv_size  = (size_t)eye_w * eye_h * 3u / 2u;
    const size_t rgba_size = (size_t)eye_w * 2u * eye_h * 4u;

    uint8_t* left_yuv  = (uint8_t*)malloc(yuv_size);
    uint8_t* right_yuv = (uint8_t*)malloc(yuv_size);
    uint8_t* sbs_rgba  = (uint8_t*)malloc(rgba_size);
    if (!left_yuv || !right_yuv || !sbs_rgba) {
        fprintf(stderr, "malloc failed\n");
        free(left_yuv); free(right_yuv); free(sbs_rgba);
        return 2;
    }

    ImtMap map;
    if (imt_map_init(&map, eye_w * 2u, eye_h, 32u) != 0) {
        fprintf(stderr, "imt_map_init failed\n");
        free(left_yuv); free(right_yuv); free(sbs_rgba);
        return 3;
    }
    imt_map_fill(&map, 128u);

    ImtPacketizer pkt;
    if (imt_packetizer_init(&pkt, 0, 0) != 0) {
        fprintf(stderr, "imt_packetizer_init failed\n");
        imt_map_destroy(&map);
        free(left_yuv); free(right_yuv); free(sbs_rgba);
        return 4;
    }

    SendCtx ctx;
    ctx.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx.fd < 0) { return 5; }
    memset(&ctx.addr, 0, sizeof(ctx.addr));
    ctx.addr.sin_family = AF_INET;
    ctx.addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &ctx.addr.sin_addr) != 1) {
        close(ctx.fd); return 6;
    }

    FILE* left_pipe  = open_cam(left_cam,  eye_w, eye_h, fps);
    FILE* right_pipe = open_cam(right_cam, eye_w, eye_h, fps);
    if (!left_pipe || !right_pipe) {
        fprintf(stderr, "failed to start rpicam-vid (see /tmp/cam*.log)\n");
        if (left_pipe)  pclose(left_pipe);
        if (right_pipe) pclose(right_pipe);
        close(ctx.fd);
        imt_packetizer_destroy(&pkt);
        imt_map_destroy(&map);
        free(left_yuv); free(right_yuv); free(sbs_rgba);
        return 7;
    }

    fprintf(stderr,
            "mps-imt-rpicam-sbs → %s:%u  left=cam%u(xf%d) right=cam%u(xf%d)  eye=%ux%u sbs=%ux%u @ %u fps\n",
            host, port, left_cam, left_xform, right_cam, right_xform,
            eye_w, eye_h, eye_w * 2u, eye_h, fps);

    uint64_t frame_seq = 0;
    for (;;) {
        if (read_exact(left_pipe,  left_yuv,  yuv_size) != 0) {
            fprintf(stderr, "left cam read failed (see /tmp/cam%u.log)\n", left_cam);
            break;
        }
        if (read_exact(right_pipe, right_yuv, yuv_size) != 0) {
            fprintf(stderr, "right cam read failed (see /tmp/cam%u.log)\n", right_cam);
            break;
        }

        yuv420_to_rgba_sbs(left_yuv,  sbs_rgba, eye_w, eye_h, 0u,    left_xform);
        yuv420_to_rgba_sbs(right_yuv, sbs_rgba, eye_w, eye_h, eye_w, right_xform);

        imt_packetizer_send_frame(&pkt,
                                  sbs_rgba, (uint32_t)rgba_size,
                                  frame_seq, now_ns(),
                                  IMT_PACKET_FLAG_KEYFRAME,
                                  NULL, 0,
                                  emit_datagram, &ctx);
        frame_seq += 1u;
    }

    pclose(left_pipe);
    pclose(right_pipe);
    close(ctx.fd);
    imt_packetizer_destroy(&pkt);
    imt_map_destroy(&map);
    free(left_yuv); free(right_yuv); free(sbs_rgba);
    return 0;
}
