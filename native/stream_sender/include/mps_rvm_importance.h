#pragma once
/*
 * mps_rvm_importance.h
 * RVM (MobileNetV3) ONNX Runtime adapter — updates an ImtMap from a YUV420
 * eye frame so that person-region tiles receive high importance weights.
 *
 * Built only when ONNXRUNTIME_ROOT is set at CMake time (MPS_ENABLE_RVM=ON).
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "imt.h"
#include <stdint.h>

typedef struct MpsRvmCtx MpsRvmCtx; /* opaque */

/*
 * model_path : absolute path to rvm_mobilenetv3_fp32.onnx
 * width      : single-eye width  (e.g. 640)
 * height     : single-eye height (e.g. 360)
 * Returns NULL on failure (reason printed to stderr).
 */
MpsRvmCtx* mps_rvm_init(const char* model_path,
                         uint32_t width, uint32_t height);
void mps_rvm_destroy(MpsRvmCtx* ctx);

/*
 * Runs one RVM inference step on a single-eye YUV420 frame and updates `map`
 * via imt_map_update_ema.  Only the tile columns that belong to this eye are
 * set from the alpha output; all other columns are set to 128 (neutral) for
 * the EMA call so the cross-eye tiles decay toward the mean over time.
 *
 * yuv420       : I420 buffer — Y plane (eye_w×eye_h) then U and V quarter-planes
 * yuv_size     : must be >= width * height * 3 / 2
 * map          : ImtMap for the full SBS frame (eye_w*2 × eye_h, tile_size=32)
 * eye_x_offset : 0 for the left eye, eye_w for the right eye
 *
 * Returns 0 on success, negative error code otherwise.
 */
int mps_rvm_update_map(MpsRvmCtx* ctx,
                       const uint8_t* yuv420, uint32_t yuv_size,
                       ImtMap* map,
                       uint32_t eye_x_offset);

/* Tile weights assigned by alpha threshold (alpha > 0.5 → FG). */
#define MPS_RVM_WEIGHT_FG 200u
#define MPS_RVM_WEIGHT_BG  60u

#ifdef __cplusplus
}
#endif
