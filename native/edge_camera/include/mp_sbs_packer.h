#ifndef MP_SBS_PACKER_H
#define MP_SBS_PACKER_H

#include "mp_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

int mp_sbs_pack_nv12(const MpFrame* left, const MpFrame* right, MpFrame* out_sbs);

#ifdef __cplusplus
}
#endif

#endif

