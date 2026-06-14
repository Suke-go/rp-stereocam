#ifndef MPS_I420_FOREGROUND_MASK_H
#define MPS_I420_FOREGROUND_MASK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MpsI420GreenbackMaskConfig {
    uint8_t key_u;
    uint8_t key_v;
    uint8_t chroma_tolerance;
} MpsI420GreenbackMaskConfig;

MpsI420GreenbackMaskConfig mps_i420_greenback_mask_config_default(void);
int mps_i420_greenback_foreground_mask_sbs_r8(const uint8_t* left_i420,
                                              const uint8_t* right_i420,
                                              uint32_t eye_width,
                                              uint32_t eye_height,
                                              uint32_t mask_eye_width,
                                              uint32_t mask_eye_height,
                                              uint8_t* out_sbs_r8,
                                              uint32_t out_stride,
                                              const MpsI420GreenbackMaskConfig* config);
int mps_i420_apply_sbs_r8_foreground_matte(uint8_t* sbs_i420,
                                           uint32_t sbs_width,
                                           uint32_t sbs_height,
                                           const uint8_t* mask_sbs_r8,
                                           uint32_t mask_width,
                                           uint32_t mask_height,
                                           uint32_t mask_stride,
                                           uint8_t mask_threshold,
                                           uint8_t matte_y,
                                           uint8_t matte_u,
                                           uint8_t matte_v);

#ifdef __cplusplus
}
#endif

#endif
