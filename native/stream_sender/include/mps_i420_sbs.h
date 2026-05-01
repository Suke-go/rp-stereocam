#ifndef MPS_I420_SBS_H
#define MPS_I420_SBS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t mps_i420_size(uint32_t width, uint32_t height);

int mps_i420_pack_sbs(const uint8_t* left_i420,
                      const uint8_t* right_i420,
                      uint8_t* out_sbs_i420,
                      uint32_t eye_width,
                      uint32_t eye_height);

int mps_i420_pack_sbs_mono(const uint8_t* eye_i420,
                           uint8_t* out_sbs_i420,
                           uint32_t eye_width,
                           uint32_t eye_height);

#ifdef __cplusplus
}
#endif

#endif
