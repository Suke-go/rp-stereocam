#include "mp_foreground.h"

#include <stdlib.h>
#include <string.h>

#if defined(MP_EDGE_ENABLE_NEON) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define MP_FOREGROUND_HAS_NEON 1
#else
#define MP_FOREGROUND_HAS_NEON 0
#endif

static int mp_nv12_frame_valid(const MpFrame* frame)
{
    if (!frame || !frame->data || frame->format != MP_PIXFMT_NV12) {
        return 0;
    }
    if (frame->width < 2u || frame->height < 2u ||
        frame->stride_y < frame->width || frame->stride_uv < frame->width) {
        return 0;
    }
    if (frame->data_size < (size_t)frame->stride_y * frame->height +
                               (size_t)frame->stride_uv * (frame->height / 2u)) {
        return 0;
    }
    return 1;
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

MpForegroundConfig mp_foreground_config_default(void)
{
    MpForegroundConfig config;
    config.greenback = mp_greenback_key_default();
    config.background_grow_iterations = 1u;
    config.mask_threshold = 128u;
    return config;
}

int mp_foreground_workspace_init(MpForegroundWorkspace* workspace, uint32_t width, uint32_t height)
{
    size_t size;

    if (!workspace || width < 2u || height < 2u) {
        return -1;
    }

    memset(workspace, 0, sizeof(*workspace));
    size = (size_t)width * (size_t)height;
    workspace->mask = (uint8_t*)malloc(size);
    workspace->scratch = (uint8_t*)malloc(size);
    if (!workspace->mask || !workspace->scratch) {
        mp_foreground_workspace_destroy(workspace);
        return -2;
    }

    workspace->width = width;
    workspace->height = height;
    workspace->stride = width;
    workspace->buffer_size = size;
    return 0;
}

void mp_foreground_workspace_destroy(MpForegroundWorkspace* workspace)
{
    if (!workspace) {
        return;
    }
    free(workspace->mask);
    free(workspace->scratch);
    memset(workspace, 0, sizeof(*workspace));
}

static int mp_foreground_workspace_matches(const MpForegroundWorkspace* workspace,
                                           uint32_t width,
                                           uint32_t height)
{
    return workspace &&
           workspace->mask &&
           workspace->scratch &&
           workspace->width == width &&
           workspace->height == height &&
           workspace->stride >= width &&
           workspace->buffer_size >= (size_t)width * (size_t)height;
}

static void mp_build_greenback_background_mask(const MpFrame* frame,
                                               MpForegroundWorkspace* workspace,
                                               const MpGreenbackKeyConfig* config)
{
    uint32_t y;
    const uint8_t* y_plane = frame->data;
    const uint8_t* uv_plane = y_plane + (size_t)frame->stride_y * frame->height;

    memset(workspace->mask, 0, workspace->buffer_size);
    for (y = 0; y + 1u < frame->height; y += 2u) {
        uint32_t x;
        const uint8_t* y0 = y_plane + (size_t)y * frame->stride_y;
        const uint8_t* y1 = y_plane + (size_t)(y + 1u) * frame->stride_y;
        const uint8_t* uv = uv_plane + (size_t)(y / 2u) * frame->stride_uv;
        uint8_t* mask0 = workspace->mask + (size_t)y * workspace->stride;
        uint8_t* mask1 = workspace->mask + (size_t)(y + 1u) * workspace->stride;

        for (x = 0; x + 1u < frame->width; x += 2u) {
            if (mp_greenback_block_is_background(y0, y1, x, uv[x], uv[x + 1u], config)) {
                mask0[x] = 255u;
                mask0[x + 1u] = 255u;
                mask1[x] = 255u;
                mask1[x + 1u] = 255u;
            }
        }
    }
}

static void mp_dilate_mask3x3(MpForegroundWorkspace* workspace)
{
    uint32_t y;
    const uint32_t width = workspace->width;
    const uint32_t height = workspace->height;
    const uint32_t stride = workspace->stride;

    memcpy(workspace->scratch, workspace->mask, workspace->buffer_size);
    for (y = 1u; y + 1u < height; ++y) {
        uint32_t x;
        const uint8_t* prev = workspace->scratch + (size_t)(y - 1u) * stride;
        const uint8_t* curr = workspace->scratch + (size_t)y * stride;
        const uint8_t* next = workspace->scratch + (size_t)(y + 1u) * stride;
        uint8_t* out = workspace->mask + (size_t)y * stride;

#if MP_FOREGROUND_HAS_NEON
        for (x = 1u; x + 17u < width; x += 16u) {
            const uint8x16_t prev_l = vld1q_u8(prev + x - 1u);
            const uint8x16_t prev_c = vld1q_u8(prev + x);
            const uint8x16_t prev_r = vld1q_u8(prev + x + 1u);
            const uint8x16_t curr_l = vld1q_u8(curr + x - 1u);
            const uint8x16_t curr_c = vld1q_u8(curr + x);
            const uint8x16_t curr_r = vld1q_u8(curr + x + 1u);
            const uint8x16_t next_l = vld1q_u8(next + x - 1u);
            const uint8x16_t next_c = vld1q_u8(next + x);
            const uint8x16_t next_r = vld1q_u8(next + x + 1u);
            uint8x16_t merged = vorrq_u8(prev_l, prev_c);
            merged = vorrq_u8(merged, prev_r);
            merged = vorrq_u8(merged, curr_l);
            merged = vorrq_u8(merged, curr_c);
            merged = vorrq_u8(merged, curr_r);
            merged = vorrq_u8(merged, next_l);
            merged = vorrq_u8(merged, next_c);
            merged = vorrq_u8(merged, next_r);
            vst1q_u8(out + x, merged);
        }
#else
        x = 1u;
#endif
        for (; x + 1u < width; ++x) {
            if (prev[x - 1u] || prev[x] || prev[x + 1u] ||
                curr[x - 1u] || curr[x] || curr[x + 1u] ||
                next[x - 1u] || next[x] || next[x + 1u]) {
                out[x] = 255u;
            } else {
                out[x] = 0u;
            }
        }
    }
}

int mp_foreground_greenback_remove_nv12(MpFrame* nv12_frame,
                                        MpForegroundWorkspace* workspace,
                                        const MpForegroundConfig* config)
{
    MpForegroundConfig local_config;
    uint8_t i;

    if (!mp_nv12_frame_valid(nv12_frame)) {
        return -1;
    }
    if (!mp_foreground_workspace_matches(workspace, nv12_frame->width, nv12_frame->height)) {
        return -2;
    }

    local_config = config ? *config : mp_foreground_config_default();
    if (local_config.background_grow_iterations > 4u) {
        local_config.background_grow_iterations = 4u;
    }
    if (local_config.mask_threshold == 0u) {
        local_config.mask_threshold = 128u;
    }

    mp_build_greenback_background_mask(nv12_frame, workspace, &local_config.greenback);
    for (i = 0; i < local_config.background_grow_iterations; ++i) {
        mp_dilate_mask3x3(workspace);
    }

    return mp_mask_apply_solid_nv12(nv12_frame,
                                    workspace->mask,
                                    workspace->width,
                                    workspace->height,
                                    workspace->stride,
                                    local_config.mask_threshold,
                                    local_config.greenback.replacement);
}
