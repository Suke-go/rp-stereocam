#ifndef MP_MASK_H
#define MP_MASK_H

#include "mp_frame.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MpSolidMaskColor {
    uint8_t y;
    uint8_t u;
    uint8_t v;
} MpSolidMaskColor;

typedef struct MpGreenbackKeyConfig {
    uint8_t green_u;
    uint8_t green_v;
    uint8_t chroma_radius;
    uint8_t min_y;
    uint8_t edge_soften;
    MpSolidMaskColor replacement;
} MpGreenbackKeyConfig;

int mp_mask_apply_solid_nv12(MpFrame* nv12_frame,
                             const uint8_t* mask,
                             uint32_t mask_width,
                             uint32_t mask_height,
                             uint32_t mask_stride,
                             uint8_t threshold,
                             MpSolidMaskColor color);

int mp_mask_fill_solid_nv12(MpFrame* nv12_frame, MpSolidMaskColor color);

MpGreenbackKeyConfig mp_greenback_key_default(void);
int mp_greenback_remove_background_nv12(MpFrame* nv12_frame,
                                        const MpGreenbackKeyConfig* config);

#ifdef __cplusplus
}
#endif

#endif
