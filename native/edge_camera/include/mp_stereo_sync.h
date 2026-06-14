#ifndef MP_STEREO_SYNC_H
#define MP_STEREO_SYNC_H

#include "mp_frame.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MpStereoPair {
    const MpFrame* left;
    const MpFrame* right;
    uint64_t pair_timestamp_ns;
    uint64_t left_corrected_timestamp_ns;
    uint64_t right_corrected_timestamp_ns;
    uint64_t skew_ns;
} MpStereoPair;

typedef enum MpStereoSyncResult {
    MP_STEREO_SYNC_PAIR = 0,
    MP_STEREO_SYNC_DROP_LEFT = 1,
    MP_STEREO_SYNC_DROP_RIGHT = 2,
    MP_STEREO_SYNC_REJECT = 3
} MpStereoSyncResult;

MpStereoSyncResult mp_stereo_sync_pair(const MpFrame* left,
                                       const MpFrame* right,
                                       uint64_t max_skew_ns,
                                       MpStereoPair* out_pair);

MpStereoSyncResult mp_stereo_sync_pair_corrected(const MpFrame* left,
                                                 uint64_t left_corrected_timestamp_ns,
                                                 const MpFrame* right,
                                                 uint64_t right_corrected_timestamp_ns,
                                                 uint64_t max_skew_ns,
                                                 MpStereoPair* out_pair);

#ifdef __cplusplus
}
#endif

#endif
