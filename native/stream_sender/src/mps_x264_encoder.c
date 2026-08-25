#include "mps_x264_encoder.h"

#include <stdlib.h>
#include <string.h>

#include <x264.h>

struct MpsX264Ctx {
    x264_t* enc;
    uint32_t width;
    uint32_t height;
    uint32_t mb_width;
    uint32_t mb_height;
    uint64_t pts;
    float* quant_offsets;
    uint8_t* nal_buf;
    MpsX264SliceInfo* slices;
    int nal_capacity;
    int slice_capacity;
};

static int mps_x264_reserve(MpsX264Ctx* ctx, int size)
{
    uint8_t* next;
    int capacity;
    if (size <= ctx->nal_capacity) {
        return 0;
    }
    capacity = ctx->nal_capacity > 0 ? ctx->nal_capacity : 4096;
    while (capacity < size) {
        if (capacity > 0x3fffffff) {
            return -1;
        }
        capacity *= 2;
    }
    next = (uint8_t*)realloc(ctx->nal_buf, (size_t)capacity);
    if (!next) {
        return -1;
    }
    ctx->nal_buf = next;
    ctx->nal_capacity = capacity;
    return 0;
}

static int mps_x264_reserve_slices(MpsX264Ctx* ctx, int count)
{
    MpsX264SliceInfo* next;
    int capacity;
    if (count <= ctx->slice_capacity) {
        return 0;
    }
    capacity = ctx->slice_capacity > 0 ? ctx->slice_capacity : 16;
    while (capacity < count) {
        if (capacity > 0x3fffffff) {
            return -1;
        }
        capacity *= 2;
    }
    next = (MpsX264SliceInfo*)realloc(ctx->slices,
                                      (size_t)capacity * sizeof(ctx->slices[0]));
    if (!next) {
        return -1;
    }
    ctx->slices = next;
    ctx->slice_capacity = capacity;
    return 0;
}

static void mps_x264_fill_quant_offsets(MpsX264Ctx* ctx,
                                        const ImtMap* map,
                                        const ImtQpLut* lut)
{
    const uint32_t mb_count = ctx->mb_width * ctx->mb_height;
    uint32_t mb_y;
    int mean = 0;

    if (!ctx->quant_offsets || mb_count == 0u) {
        return;
    }
    memset(ctx->quant_offsets, 0, (size_t)mb_count * sizeof(ctx->quant_offsets[0]));
    if (!map || !map->weights || !lut || map->tile_size == 0u ||
        map->cols == 0u || map->rows == 0u || map->tile_count == 0u) {
        return;
    }

    mean = imt_map_mean(map);
    if (mean <= 0) {
        mean = 1;
    }

    for (mb_y = 0; mb_y < ctx->mb_height; ++mb_y) {
        uint32_t mb_x;
        uint32_t y = mb_y * 16u + 8u;
        if (y >= ctx->height) {
            y = ctx->height - 1u;
        }
        for (mb_x = 0; mb_x < ctx->mb_width; ++mb_x) {
            uint32_t x = mb_x * 16u + 8u;
            uint32_t tile_col;
            uint32_t tile_row;
            uint32_t tile_idx;
            if (x >= ctx->width) {
                x = ctx->width - 1u;
            }
            tile_col = x / map->tile_size;
            tile_row = y / map->tile_size;
            if (tile_col >= map->cols) {
                tile_col = map->cols - 1u;
            }
            if (tile_row >= map->rows) {
                tile_row = map->rows - 1u;
            }
            tile_idx = tile_row * map->cols + tile_col;
            if (tile_idx >= map->tile_count) {
                tile_idx = map->tile_count - 1u;
            }
            ctx->quant_offsets[mb_y * ctx->mb_width + mb_x] =
                (float)imt_qp_offset(lut, map->weights[tile_idx], (uint32_t)mean);
        }
    }
}

MpsX264Ctx* mps_x264_init(uint32_t width, uint32_t height, uint32_t fps, float crf)
{
    x264_param_t param;
    MpsX264Ctx* ctx;
    uint32_t mb_count;

    if (width == 0u || height == 0u || fps == 0u) {
        return NULL;
    }

    ctx = (MpsX264Ctx*)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->width = width;
    ctx->height = height;
    ctx->mb_width = (width + 15u) / 16u;
    ctx->mb_height = (height + 15u) / 16u;
    mb_count = ctx->mb_width * ctx->mb_height;
    ctx->quant_offsets = (float*)calloc(mb_count, sizeof(ctx->quant_offsets[0]));
    if (!ctx->quant_offsets) {
        mps_x264_destroy(ctx);
        return NULL;
    }

    if (x264_param_default_preset(&param, "ultrafast", "zerolatency") < 0) {
        mps_x264_destroy(ctx);
        return NULL;
    }
    param.i_width = (int)width;
    param.i_height = (int)height;
    param.i_fps_num = (int)fps;
    param.i_fps_den = 1;
    param.i_csp = X264_CSP_I420;
    param.b_annexb = 1;
    param.b_repeat_headers = 1;
    param.i_keyint_max = 1;
    param.i_keyint_min = 1;
    param.i_slice_max_size = (int)IMT_DEFAULT_MAX_PAYLOAD;
    param.b_sliced_threads = 0;
    param.rc.i_rc_method = X264_RC_CRF;
    param.rc.f_rf_constant = crf > 0.0f ? crf : 23.0f;
    param.rc.i_aq_mode = X264_AQ_VARIANCE;
    param.i_threads = 1;
    if (x264_param_apply_profile(&param, "main") < 0) {
        mps_x264_destroy(ctx);
        return NULL;
    }

    ctx->enc = x264_encoder_open(&param);
    if (!ctx->enc) {
        mps_x264_destroy(ctx);
        return NULL;
    }
    return ctx;
}

void mps_x264_destroy(MpsX264Ctx* ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->enc) {
        x264_encoder_close(ctx->enc);
    }
    free(ctx->quant_offsets);
    free(ctx->nal_buf);
    free(ctx->slices);
    free(ctx);
}

uint32_t mps_x264_mb_width(const MpsX264Ctx* ctx)
{
    return ctx ? ctx->mb_width : 0u;
}

int mps_x264_encode(MpsX264Ctx* ctx,
                    const uint8_t* sbs_i420,
                    const ImtMap* map,
                    const ImtQpLut* lut,
                    uint8_t** out_nal,
                    int* out_is_keyframe,
                    const MpsX264SliceInfo** out_slices,
                    uint32_t* out_slice_count)
{
    x264_picture_t pic_in;
    x264_picture_t pic_out;
    x264_nal_t* nals = NULL;
    int nal_count = 0;
    int payload_size;
    int offset = 0;
    int i;

    if (!ctx || !ctx->enc || !sbs_i420 || !out_nal || !out_is_keyframe ||
        !out_slices || !out_slice_count) {
        return -1;
    }
    *out_slices = NULL;
    *out_slice_count = 0u;

    mps_x264_fill_quant_offsets(ctx, map, lut);
    x264_picture_init(&pic_in);
    x264_picture_init(&pic_out);
    pic_in.i_type = X264_TYPE_IDR;
    pic_in.i_pts = (int64_t)ctx->pts++;
    pic_in.img.i_csp = X264_CSP_I420;
    pic_in.img.i_plane = 3;
    pic_in.img.plane[0] = (uint8_t*)sbs_i420;
    pic_in.img.plane[1] = (uint8_t*)sbs_i420 + (size_t)ctx->width * ctx->height;
    pic_in.img.plane[2] = pic_in.img.plane[1] + (size_t)ctx->width * ctx->height / 4u;
    pic_in.img.i_stride[0] = (int)ctx->width;
    pic_in.img.i_stride[1] = (int)(ctx->width / 2u);
    pic_in.img.i_stride[2] = (int)(ctx->width / 2u);
    pic_in.prop.quant_offsets = ctx->quant_offsets;

    payload_size = x264_encoder_encode(ctx->enc, &nals, &nal_count, &pic_in, &pic_out);
    if (payload_size < 0) {
        return -2;
    }
    if (payload_size == 0 || nal_count == 0) {
        return 0;
    }
    if (mps_x264_reserve(ctx, payload_size) != 0) {
        return -3;
    }
    if (mps_x264_reserve_slices(ctx, nal_count) != 0) {
        return -3;
    }
    for (i = 0; i < nal_count; ++i) {
        if (offset + nals[i].i_payload > ctx->nal_capacity) {
            return -3;
        }
        ctx->slices[i].byte_offset = (uint32_t)offset;
        ctx->slices[i].byte_size = (uint32_t)nals[i].i_payload;
        ctx->slices[i].first_mb = nals[i].i_first_mb;
        ctx->slices[i].last_mb = nals[i].i_last_mb;
        memcpy(ctx->nal_buf + offset, nals[i].p_payload, (size_t)nals[i].i_payload);
        offset += nals[i].i_payload;
    }

    *out_nal = ctx->nal_buf;
    *out_is_keyframe = pic_out.b_keyframe ? 1 : 0;
    *out_slices = ctx->slices;
    *out_slice_count = (uint32_t)nal_count;
    return offset;
}
