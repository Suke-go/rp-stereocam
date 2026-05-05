/* Microbenchmark: I420-SBS pack vs NV12-SBS pack vs YUV420->NV12-SBS pack.
 * Run as: ./mps_pack_bench [eye_width] [eye_height] [iterations]
 * Default: 640x360, 1000 iterations.  No assertions on byte equivalence — this
 * exists only to verify the new path is at least as fast as the old one. */

#include "mps_i420_sbs.h"
#include "mps_nv12_sbs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
static double now_seconds(void)
{
    LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}
#else
static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

static void fill_pattern(uint8_t* buf, size_t size, uint8_t seed)
{
    size_t i;
    for (i = 0; i < size; ++i) {
        buf[i] = (uint8_t)((i * 13u + seed) & 0xFFu);
    }
}

int main(int argc, char** argv)
{
    uint32_t w = argc > 1 ? (uint32_t)atoi(argv[1]) : 640u;
    uint32_t h = argc > 2 ? (uint32_t)atoi(argv[2]) : 360u;
    int iters = argc > 3 ? atoi(argv[3]) : 1000;
    size_t eye_size = (size_t)w * h * 3u / 2u;
    size_t sbs_size = eye_size * 2u;

    if ((w & 1u) || (h & 1u)) {
        fprintf(stderr, "width and height must be even\n");
        return 1;
    }

    uint8_t* left = (uint8_t*)malloc(eye_size);
    uint8_t* right = (uint8_t*)malloc(eye_size);
    uint8_t* sbs = (uint8_t*)malloc(sbs_size);
    if (!left || !right || !sbs) {
        fprintf(stderr, "malloc failed\n");
        return 2;
    }

    fill_pattern(left, eye_size, 0u);
    fill_pattern(right, eye_size, 7u);

    printf("eye=%ux%u iterations=%d sbs_bytes=%zu\n", w, h, iters, sbs_size);

    /* Warmup */
    (void)mps_i420_pack_sbs(left, right, sbs, w, h);
    (void)mps_nv12_pack_sbs(left, right, sbs, w, h);
    (void)mps_yuv420_pack_sbs_nv12(left, right, sbs, w, h);

    double t0 = now_seconds();
    for (int i = 0; i < iters; ++i) {
        (void)mps_i420_pack_sbs(left, right, sbs, w, h);
    }
    double t1 = now_seconds();
    for (int i = 0; i < iters; ++i) {
        (void)mps_nv12_pack_sbs(left, right, sbs, w, h);
    }
    double t2 = now_seconds();
    for (int i = 0; i < iters; ++i) {
        (void)mps_yuv420_pack_sbs_nv12(left, right, sbs, w, h);
    }
    double t3 = now_seconds();

    double i420_ms = (t1 - t0) * 1000.0 / iters;
    double nv12_ms = (t2 - t1) * 1000.0 / iters;
    double yuv2nv12_ms = (t3 - t2) * 1000.0 / iters;

    printf("i420  pack: %.3f ms/frame\n", i420_ms);
    printf("nv12  pack: %.3f ms/frame\n", nv12_ms);
    printf("yuv420->nv12 pack: %.3f ms/frame (includes uv interleave)\n", yuv2nv12_ms);

    free(left);
    free(right);
    free(sbs);
    return 0;
}
