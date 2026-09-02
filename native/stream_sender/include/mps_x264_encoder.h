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

typedef struct MpsX264Options {
    /* Maximum distance between IDR frames. 1 preserves the legacy
     * all-intra stream; larger values enable inter prediction. */
    uint32_t keyint;
    /* x264 worker threads inside one encoder. The stereo sender normally
     * keeps this at one and runs the two eye encoders concurrently. */
    uint32_t threads;
    /* Maximum H.264 slice size in bytes. Zero lets x264 choose natural NAL
     * boundaries; the IMT transport fragments the completed access unit. */
    uint32_t slice_max_size;
    int repeat_headers;
} MpsX264Options;

void mps_x264_default_options(MpsX264Options* options);
MpsX264Ctx* mps_x264_init(uint32_t width, uint32_t height, uint32_t fps, float crf);
MpsX264Ctx* mps_x264_init_ex(uint32_t width, uint32_t height, uint32_t fps, float crf,
                             const MpsX264Options* options);
void mps_x264_destroy(MpsX264Ctx* ctx);
uint32_t mps_x264_mb_width(const MpsX264Ctx* ctx);
/* Reconfigure CRF between frames. Call only while no thread is inside
 * mps_x264_encode for this context. */
int mps_x264_set_crf(MpsX264Ctx* ctx, float crf);
/* Request an IDR for the next access unit. Call on the same worker thread as
 * mps_x264_encode; the encoder context is intentionally not internally
 * synchronized. */
int mps_x264_force_idr_next(MpsX264Ctx* ctx);

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
