#ifndef MPS_NV12_SBS_H
#define MPS_NV12_SBS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NV12 layout: a Y plane of (width * height) bytes followed by an interleaved
 * UV plane of (width * height / 2) bytes (one chroma sample per 2x2 luma block,
 * U and V interleaved as UVUVUV...).  This is the format that v4l2 hw encoders
 * such as v4l2h264enc on Pi prefer, and skipping the I420->NV12 (or YUV420->I420)
 * repack saves a memcpy + chroma de-interleave per frame. */

size_t mps_nv12_size(uint32_t width, uint32_t height);

int mps_nv12_pack_sbs(const uint8_t* left_nv12,
                      const uint8_t* right_nv12,
                      uint8_t* out_sbs_nv12,
                      uint32_t eye_width,
                      uint32_t eye_height);

int mps_nv12_pack_sbs_mono(const uint8_t* eye_nv12,
                           uint8_t* out_sbs_nv12,
                           uint32_t eye_width,
                           uint32_t eye_height);

/* Pack an SBS NV12 directly from a libcamera-style YUV420 (3 planes: Y, U, V)
 * source for each eye, avoiding an intermediate I420->NV12 pass. */
int mps_yuv420_pack_sbs_nv12(const uint8_t* left_yuv420,
                             const uint8_t* right_yuv420,
                             uint8_t* out_sbs_nv12,
                             uint32_t eye_width,
                             uint32_t eye_height);

#ifdef __cplusplus
}
#endif

#endif
