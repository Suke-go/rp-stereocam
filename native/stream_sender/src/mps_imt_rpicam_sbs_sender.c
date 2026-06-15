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
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_SEND_INTERVAL 10u
#define FEEDBACK_PORT 5005u
#define FEEDBACK_RECV_TIMEOUT_US 100000
#define IMT_TARGET_LATENCY_NS 50000000ll

typedef struct {
    int fd;
    struct sockaddr_in addr;
} SendCtx;

typedef struct {
    int fd;
    ImtPace* pace;
    pthread_mutex_t lock;
    volatile int stop;
} FeedbackState;

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

static void* feedback_thread(void* arg)
{
    FeedbackState* state = (FeedbackState*)arg;
    uint8_t buf[IMT_WIRE_HEADER_SIZE + IMT_WIRE_FEEDBACK_PAYLOAD_SIZE];

    while (!state->stop) {
        const ssize_t n = recv(state->fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == (ssize_t)sizeof(buf)) {
            ImtWireHeader header;
            ImtFeedback feedback;
            if (imt_wire_decode_header(buf, (size_t)n, &header) == 0 &&
                header.type == IMT_PACKET_TYPE_FEEDBACK &&
                imt_wire_decode_feedback(buf + IMT_WIRE_HEADER_SIZE,
                                         IMT_WIRE_FEEDBACK_PAYLOAD_SIZE,
                                         &feedback) == 0) {
                pthread_mutex_lock(&state->lock);
                (void)imt_pace_update(state->pace, (int64_t)feedback.frame_age_ns);
                pthread_mutex_unlock(&state->lock);
            }
        }
    }
    return NULL;
}

static int feedback_state_start(FeedbackState* state, ImtPace* pace, pthread_t* thread)
{
    struct sockaddr_in addr;
    struct timeval timeout;

    memset(state, 0, sizeof(*state));
    state->fd = -1;
    state->pace = pace;
    if (pthread_mutex_init(&state->lock, NULL) != 0) {
        return -1;
    }

    state->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (state->fd < 0) {
        pthread_mutex_destroy(&state->lock);
        return -2;
    }

    timeout.tv_sec = 0;
    timeout.tv_usec = FEEDBACK_RECV_TIMEOUT_US;
    (void)setsockopt(state->fd, SOL_SOCKET, SO_RCVTIMEO,
                     &timeout, sizeof(timeout));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(FEEDBACK_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(state->fd, (const struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(state->fd);
        state->fd = -1;
        pthread_mutex_destroy(&state->lock);
        return -3;
    }

    if (pthread_create(thread, NULL, feedback_thread, state) != 0) {
        close(state->fd);
        state->fd = -1;
        pthread_mutex_destroy(&state->lock);
        return -4;
    }
    return 0;
}

static void feedback_state_stop(FeedbackState* state, pthread_t thread)
{
    if (!state || state->fd < 0) {
        return;
    }
    state->stop = 1;
    (void)pthread_join(thread, NULL);
    close(state->fd);
    state->fd = -1;
    pthread_mutex_destroy(&state->lock);
}

static int32_t feedback_state_rate_q16(FeedbackState* state)
{
    int32_t rate;
    if (!state || state->fd < 0 || !state->pace) {
        return IMT_PACE_Q16_ONE;
    }

    pthread_mutex_lock(&state->lock);
    rate = imt_pace_rate_q16(state->pace);
    pthread_mutex_unlock(&state->lock);
    return rate;
}

static void apply_feedback_pacing(FeedbackState* state, uint32_t fps)
{
    const int32_t rate = feedback_state_rate_q16(state);
    if (fps == 0u || rate <= 0 || rate >= IMT_PACE_Q16_ONE) {
        return;
    }
    {
        const uint64_t frame_us = 1000000ull / fps;
        const uint64_t extra_us =
            (frame_us * (uint64_t)(IMT_PACE_Q16_ONE - rate)) / IMT_PACE_Q16_ONE;
        if (extra_us > 0u) {
            struct timespec delay;
            delay.tv_sec = (time_t)(extra_us / 1000000ull);
            delay.tv_nsec = (long)((extra_us % 1000000ull) * 1000ull);
            while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
            }
        }
    }
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

static int fill_chunk_weights(const ImtMap* map,
                              uint8_t* chunk_weights,
                              uint32_t chunk_count,
                              uint32_t sbs_w,
                              uint32_t frame_size)
{
    int mean_int;
    uint32_t mean;
    uint32_t ci;

    if (!map || !map->weights || !chunk_weights || map->tile_size == 0u ||
        map->cols == 0u || map->tile_count == 0u || sbs_w == 0u) {
        return -1;
    }

    mean_int = imt_map_mean(map);
    mean = mean_int > 0 ? (uint32_t)mean_int : 1u;
    for (ci = 0u; ci < chunk_count; ++ci) {
        const uint32_t offset = ci * (uint32_t)IMT_DEFAULT_MAX_PAYLOAD;
        uint32_t payload_size = frame_size - offset;
        uint32_t mid_byte;
        uint32_t mid_pixel;
        uint32_t row;
        uint32_t col;
        uint32_t tile_idx;

        if (payload_size > (uint32_t)IMT_DEFAULT_MAX_PAYLOAD) {
            payload_size = (uint32_t)IMT_DEFAULT_MAX_PAYLOAD;
        }
        mid_byte = offset + payload_size / 2u;
        if (mid_byte >= frame_size) {
            mid_byte = frame_size - 1u;
        }
        mid_pixel = mid_byte / 4u;
        row = mid_pixel / sbs_w;
        col = mid_pixel % sbs_w;
        tile_idx = (row / map->tile_size) * map->cols + (col / map->tile_size);
        if (tile_idx >= map->tile_count) {
            tile_idx = map->tile_count - 1u;
        }
        chunk_weights[ci] = (uint8_t)imt_map_normalized_weight(map->weights[tile_idx], mean);
    }
    return 0;
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
    const size_t chunk_count_size =
        (rgba_size + IMT_DEFAULT_MAX_PAYLOAD - 1u) / IMT_DEFAULT_MAX_PAYLOAD;

    uint8_t* left_yuv  = (uint8_t*)malloc(yuv_size);
    uint8_t* right_yuv = (uint8_t*)malloc(yuv_size);
    uint8_t* sbs_rgba  = (uint8_t*)malloc(rgba_size);
    uint8_t* chunk_weights = (uint8_t*)malloc(chunk_count_size);
    if (!left_yuv || !right_yuv || !sbs_rgba || !chunk_weights ||
        rgba_size > UINT32_MAX || chunk_count_size > UINT32_MAX) {
        fprintf(stderr, "malloc failed\n");
        free(left_yuv);
        free(right_yuv);
        free(sbs_rgba);
        free(chunk_weights);
        return 2;
    }

    ImtMap map;
    if (imt_map_init(&map, eye_w * 2u, eye_h, 32u) != 0) {
        fprintf(stderr, "imt_map_init failed\n");
        free(left_yuv);
        free(right_yuv);
        free(sbs_rgba);
        free(chunk_weights);
        return 3;
    }
    imt_map_fill(&map, 128u);

    ImtPacketizer pkt;
    if (imt_packetizer_init(&pkt, 0, 0) != 0) {
        fprintf(stderr, "imt_packetizer_init failed\n");
        imt_map_destroy(&map);
        free(left_yuv);
        free(right_yuv);
        free(sbs_rgba);
        free(chunk_weights);
        return 4;
    }

    SendCtx ctx;
    ctx.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx.fd < 0) {
        imt_packetizer_destroy(&pkt);
        imt_map_destroy(&map);
        free(left_yuv);
        free(right_yuv);
        free(sbs_rgba);
        free(chunk_weights);
        return 5;
    }
    memset(&ctx.addr, 0, sizeof(ctx.addr));
    ctx.addr.sin_family = AF_INET;
    ctx.addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &ctx.addr.sin_addr) != 1) {
        close(ctx.fd);
        imt_packetizer_destroy(&pkt);
        imt_map_destroy(&map);
        free(left_yuv);
        free(right_yuv);
        free(sbs_rgba);
        free(chunk_weights);
        return 6;
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
        free(left_yuv);
        free(right_yuv);
        free(sbs_rgba);
        free(chunk_weights);
        return 7;
    }

    fprintf(stderr,
            "mps-imt-rpicam-sbs → %s:%u  left=cam%u(xf%d) right=cam%u(xf%d)  eye=%ux%u sbs=%ux%u @ %u fps\n",
            host, port, left_cam, left_xform, right_cam, right_xform,
            eye_w, eye_h, eye_w * 2u, eye_h, fps);

    ImtPace pace;
    FeedbackState feedback;
    pthread_t feedback_tid;
    int feedback_started = 0;
    if (imt_pace_init(&pace, IMT_TARGET_LATENCY_NS) == 0) {
        const int feedback_rc = feedback_state_start(&feedback, &pace, &feedback_tid);
        if (feedback_rc == 0) {
            feedback_started = 1;
            fprintf(stderr, "feedback receiver listening on UDP %u\n", FEEDBACK_PORT);
        } else {
            fprintf(stderr, "feedback receiver disabled: %d\n", feedback_rc);
        }
    }

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

        {
            const uint64_t capture_timestamp_ns = now_ns();
            const uint32_t chunk_count = (uint32_t)chunk_count_size;
            if (fill_chunk_weights(&map, chunk_weights, chunk_count,
                                   eye_w * 2u, (uint32_t)rgba_size) != 0) {
                fprintf(stderr, "fill_chunk_weights failed\n");
                break;
            }
            (void)imt_packetizer_send_frame(&pkt,
                                            sbs_rgba, (uint32_t)rgba_size,
                                            frame_seq, capture_timestamp_ns,
                                            IMT_PACKET_FLAG_KEYFRAME,
                                            chunk_weights, chunk_count,
                                            emit_datagram, &ctx);
            if (frame_seq % MAP_SEND_INTERVAL == 0u) {
                (void)imt_packetizer_send_map(&pkt, &map, frame_seq,
                                              capture_timestamp_ns,
                                              emit_datagram, &ctx);
            }
        }
        if (feedback_started) {
            apply_feedback_pacing(&feedback, fps);
        }
        frame_seq += 1u;
    }

    if (feedback_started) {
        feedback_state_stop(&feedback, feedback_tid);
    }
    pclose(left_pipe);
    pclose(right_pipe);
    close(ctx.fd);
    imt_packetizer_destroy(&pkt);
    imt_map_destroy(&map);
    free(left_yuv);
    free(right_yuv);
    free(sbs_rgba);
    free(chunk_weights);
    return 0;
}
