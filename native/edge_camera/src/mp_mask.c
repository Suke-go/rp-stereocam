#include "mp_mask.h"

#include <string.h>

#if defined(MP_EDGE_ENABLE_NEON) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define MP_HAS_NEON 1
#else
#define MP_HAS_NEON 0
#endif

static void mp_set_u8(uint8_t* dst, uint8_t value, uint32_t count)
{
#if MP_HAS_NEON
    uint32_t i = 0;
    const uint8x16_t packed = vdupq_n_u8(value);
    for (; i + 16u <= count; i += 16u) {
        vst1q_u8(dst + i, packed);
    }
    for (; i < count; ++i) {
        dst[i] = value;
    }
#else
    memset(dst, value, count);
#endif
}

int mp_mask_fill_solid_nv12(MpFrame* nv12_frame, MpSolidMaskColor color)
{
    uint32_t y;
    uint8_t* y_plane;
    uint8_t* uv_plane;

    if (!nv12_frame || !nv12_frame->data || nv12_frame->format != MP_PIXFMT_NV12) {
        return -1;
    }
    if (nv12_frame->stride_y < nv12_frame->width || nv12_frame->stride_uv < nv12_frame->width) {
        return -2;
    }
    if (nv12_frame->data_size < (size_t)nv12_frame->stride_y * nv12_frame->height +
                                     (size_t)nv12_frame->stride_uv * (nv12_frame->height / 2u)) {
        return -3;
    }

    y_plane = nv12_frame->data;
    uv_plane = y_plane + (size_t)nv12_frame->stride_y * nv12_frame->height;

    for (y = 0; y < nv12_frame->height; ++y) {
        mp_set_u8(y_plane + (size_t)y * nv12_frame->stride_y, color.y, nv12_frame->width);
    }

    for (y = 0; y < nv12_frame->height / 2u; ++y) {
        uint32_t x;
        uint8_t* row = uv_plane + (size_t)y * nv12_frame->stride_uv;
        for (x = 0; x + 1u < nv12_frame->width; x += 2u) {
            row[x] = color.u;
            row[x + 1u] = color.v;
        }
    }

    return 0;
}

int mp_mask_apply_solid_nv12(MpFrame* nv12_frame,
                             const uint8_t* mask,
                             uint32_t mask_width,
                             uint32_t mask_height,
                             uint32_t mask_stride,
                             uint8_t threshold,
                             MpSolidMaskColor color)
{
    uint32_t y;
    uint8_t* y_plane;
    uint8_t* uv_plane;

    if (!nv12_frame || !nv12_frame->data || !mask || nv12_frame->format != MP_PIXFMT_NV12) {
        return -1;
    }
    if (mask_width == 0 || mask_height == 0 || mask_stride < mask_width) {
        return -2;
    }
    if (nv12_frame->stride_y < nv12_frame->width || nv12_frame->stride_uv < nv12_frame->width) {
        return -3;
    }
    if (nv12_frame->data_size < (size_t)nv12_frame->stride_y * nv12_frame->height +
                                     (size_t)nv12_frame->stride_uv * (nv12_frame->height / 2u)) {
        return -4;
    }

    y_plane = nv12_frame->data;
    uv_plane = y_plane + (size_t)nv12_frame->stride_y * nv12_frame->height;

    for (y = 0; y < nv12_frame->height; ++y) {
        uint32_t x;
        const uint32_t my = (uint32_t)(((uint64_t)y * mask_height) / nv12_frame->height);
        const uint8_t* mask_row = mask + (size_t)my * mask_stride;
        uint8_t* y_row = y_plane + (size_t)y * nv12_frame->stride_y;

        for (x = 0; x < nv12_frame->width; ++x) {
            const uint32_t mx = (uint32_t)(((uint64_t)x * mask_width) / nv12_frame->width);
            if (mask_row[mx] >= threshold) {
                y_row[x] = color.y;
            }
        }
    }

    for (y = 0; y < nv12_frame->height / 2u; ++y) {
        uint32_t x;
        uint8_t* uv_row = uv_plane + (size_t)y * nv12_frame->stride_uv;
        const uint32_t source_y0 = y * 2u;
        const uint32_t source_y1 = source_y0 + 1u < nv12_frame->height ? source_y0 + 1u : source_y0;
        const uint32_t my0 = (uint32_t)(((uint64_t)source_y0 * mask_height) / nv12_frame->height);
        const uint32_t my1 = (uint32_t)(((uint64_t)source_y1 * mask_height) / nv12_frame->height);
        const uint8_t* mask_row0 = mask + (size_t)my0 * mask_stride;
        const uint8_t* mask_row1 = mask + (size_t)my1 * mask_stride;

        for (x = 0; x + 1u < nv12_frame->width; x += 2u) {
            const uint32_t mx0 = (uint32_t)(((uint64_t)x * mask_width) / nv12_frame->width);
            const uint32_t mx1 = (uint32_t)(((uint64_t)(x + 1u) * mask_width) / nv12_frame->width);
            if (mask_row0[mx0] >= threshold || mask_row0[mx1] >= threshold ||
                mask_row1[mx0] >= threshold || mask_row1[mx1] >= threshold) {
                uv_row[x] = color.u;
                uv_row[x + 1u] = color.v;
            }
        }
    }

    return 0;
}

MpGreenbackKeyConfig mp_greenback_key_default(void)
{
    MpGreenbackKeyConfig config;
    config.green_u = 44;
    config.green_v = 21;
    config.chroma_radius = 34;
    config.min_y = 70;
    config.edge_soften = 1;
    config.replacement.y = 16;
    config.replacement.u = 128;
    config.replacement.v = 128;
    return config;
}

static int mp_chroma_distance2(uint8_t u, uint8_t v, uint8_t target_u, uint8_t target_v)
{
    const int du = (int)u - (int)target_u;
    const int dv = (int)v - (int)target_v;
    return du * du + dv * dv;
}

static int mp_greenback_block_is_background(const uint8_t* y0,
                                            const uint8_t* y1,
                                            uint32_t x,
                                            uint8_t u,
                                            uint8_t v,
                                            const MpGreenbackKeyConfig* config)
{
    const int radius2 = (int)config->chroma_radius * (int)config->chroma_radius;
    const uint32_t y_sum = (uint32_t)y0[x] + (uint32_t)y0[x + 1u] +
                           (uint32_t)y1[x] + (uint32_t)y1[x + 1u];
    const uint8_t y_avg = (uint8_t)(y_sum / 4u);

    if (y_avg < config->min_y) {
        return 0;
    }

    return mp_chroma_distance2(u, v, config->green_u, config->green_v) <= radius2;
}

int mp_greenback_remove_background_nv12(MpFrame* nv12_frame,
                                        const MpGreenbackKeyConfig* config)
{
    MpGreenbackKeyConfig local_config;
    uint8_t* y_plane;
    uint8_t* uv_plane;
    uint32_t y;

    if (!nv12_frame || !nv12_frame->data || nv12_frame->format != MP_PIXFMT_NV12) {
        return -1;
    }
    if (nv12_frame->width < 2u || nv12_frame->height < 2u) {
        return -2;
    }
    if (nv12_frame->stride_y < nv12_frame->width || nv12_frame->stride_uv < nv12_frame->width) {
        return -3;
    }
    if (nv12_frame->data_size < (size_t)nv12_frame->stride_y * nv12_frame->height +
                                     (size_t)nv12_frame->stride_uv * (nv12_frame->height / 2u)) {
        return -4;
    }

    local_config = config ? *config : mp_greenback_key_default();
    y_plane = nv12_frame->data;
    uv_plane = y_plane + (size_t)nv12_frame->stride_y * nv12_frame->height;

    for (y = 0; y + 1u < nv12_frame->height; y += 2u) {
        uint32_t x;
        uint8_t* y0 = y_plane + (size_t)y * nv12_frame->stride_y;
        uint8_t* y1 = y_plane + (size_t)(y + 1u) * nv12_frame->stride_y;
        uint8_t* uv = uv_plane + (size_t)(y / 2u) * nv12_frame->stride_uv;

        for (x = 0; x + 1u < nv12_frame->width; x += 2u) {
            const uint8_t u = uv[x];
            const uint8_t v = uv[x + 1u];
            if (mp_greenback_block_is_background(y0, y1, x, u, v, &local_config)) {
                y0[x] = local_config.replacement.y;
                y0[x + 1u] = local_config.replacement.y;
                y1[x] = local_config.replacement.y;
                y1[x + 1u] = local_config.replacement.y;
                uv[x] = local_config.replacement.u;
                uv[x + 1u] = local_config.replacement.v;
            }
        }
    }

    (void)local_config.edge_soften;
    return 0;
}
