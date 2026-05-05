#ifndef MP_FRAME_H
#define MP_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MpPixelFormat {
    MP_PIXFMT_NV12 = 0,
    MP_PIXFMT_MASK8 = 1
} MpPixelFormat;

typedef struct MpFrame {
    uint8_t* data;
    size_t data_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride_y;
    uint32_t stride_uv;
    uint64_t timestamp_ns;
    uint64_t sequence;
    MpPixelFormat format;
} MpFrame;

static inline size_t mp_nv12_size(uint32_t width, uint32_t height)
{
    return (size_t)width * (size_t)height * 3u / 2u;
}

static inline size_t mp_mask8_size(uint32_t width, uint32_t height)
{
    return (size_t)width * (size_t)height;
}

#ifdef __cplusplus
}
#endif

#endif

