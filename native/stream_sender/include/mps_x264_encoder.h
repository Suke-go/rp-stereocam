#pragma once

#include "imt.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MpsX264Ctx MpsX264Ctx;

typedef struct MpsX264SliceInfo {
    uint32_t byte_offset;
    uint32_t byte_size;
    int32_t first_mb;
    int32_t last_mb;
} MpsX264SliceInfo;

MpsX264Ctx* mps_x264_init(uint32_t width, uint32_t height, uint32_t fps, float crf);
void mps_x264_destroy(MpsX264Ctx* ctx);
uint32_t mps_x264_mb_width(const MpsX264Ctx* ctx);

int mps_x264_encode(MpsX264Ctx* ctx,
                    const uint8_t* sbs_i420,
                    const ImtMap* map,
                    const ImtQpLut* lut,
                    uint8_t** out_nal,
                    int* out_is_keyframe,
                    const MpsX264SliceInfo** out_slices,
                    uint32_t* out_slice_count);

#ifdef __cplusplus
}
#endif
