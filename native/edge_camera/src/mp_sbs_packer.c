#include "mp_sbs_packer.h"

#include <string.h>

static int mp_nv12_frame_valid(const MpFrame* frame)
{
    if (!frame || !frame->data || frame->format != MP_PIXFMT_NV12) {
        return 0;
    }
    if (frame->width == 0 || frame->height == 0 ||
        frame->stride_y < frame->width || frame->stride_uv < frame->width) {
        return 0;
    }
    if (frame->data_size < (size_t)frame->stride_y * frame->height +
                               (size_t)frame->stride_uv * (frame->height / 2u)) {
        return 0;
    }
    return 1;
}

int mp_sbs_pack_nv12(const MpFrame* left, const MpFrame* right, MpFrame* out_sbs)
{
    uint32_t y;
    uint8_t* out_y;
    uint8_t* out_uv;
    const uint8_t* left_y;
    const uint8_t* right_y;
    const uint8_t* left_uv;
    const uint8_t* right_uv;
    const uint32_t out_width = left ? left->width * 2u : 0u;

    if (!mp_nv12_frame_valid(left) || !mp_nv12_frame_valid(right) || !out_sbs || !out_sbs->data) {
        return -1;
    }
    if (left->width != right->width || left->height != right->height) {
        return -2;
    }
    if (out_sbs->format != MP_PIXFMT_NV12 ||
        out_sbs->width != out_width ||
        out_sbs->height != left->height ||
        out_sbs->stride_y < out_width ||
        out_sbs->stride_uv < out_width) {
        return -3;
    }
    if (out_sbs->data_size < (size_t)out_sbs->stride_y * out_sbs->height +
                                 (size_t)out_sbs->stride_uv * (out_sbs->height / 2u)) {
        return -4;
    }

    left_y = left->data;
    right_y = right->data;
    left_uv = left_y + (size_t)left->stride_y * left->height;
    right_uv = right_y + (size_t)right->stride_y * right->height;
    out_y = out_sbs->data;
    out_uv = out_y + (size_t)out_sbs->stride_y * out_sbs->height;

    for (y = 0; y < left->height; ++y) {
        uint8_t* dst = out_y + (size_t)y * out_sbs->stride_y;
        memcpy(dst, left_y + (size_t)y * left->stride_y, left->width);
        memcpy(dst + left->width, right_y + (size_t)y * right->stride_y, right->width);
    }

    for (y = 0; y < left->height / 2u; ++y) {
        uint8_t* dst = out_uv + (size_t)y * out_sbs->stride_uv;
        memcpy(dst, left_uv + (size_t)y * left->stride_uv, left->width);
        memcpy(dst + left->width, right_uv + (size_t)y * right->stride_uv, right->width);
    }

    out_sbs->timestamp_ns = left->timestamp_ns > right->timestamp_ns ? left->timestamp_ns
                                                                     : right->timestamp_ns;
    out_sbs->sequence = left->sequence > right->sequence ? left->sequence : right->sequence;
    return 0;
}

