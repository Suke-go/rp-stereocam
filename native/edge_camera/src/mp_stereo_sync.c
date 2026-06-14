#include "mp_stereo_sync.h"

static uint64_t mp_abs_diff_u64(uint64_t a, uint64_t b)
{
    return a >= b ? a - b : b - a;
}

MpStereoSyncResult mp_stereo_sync_pair(const MpFrame* left,
                                       const MpFrame* right,
                                       uint64_t max_skew_ns,
                                       MpStereoPair* out_pair)
{
    if (!left || !right) {
        return MP_STEREO_SYNC_REJECT;
    }
    return mp_stereo_sync_pair_corrected(left,
                                         left->timestamp_ns,
                                         right,
                                         right->timestamp_ns,
                                         max_skew_ns,
                                         out_pair);
}

MpStereoSyncResult mp_stereo_sync_pair_corrected(const MpFrame* left,
                                                 uint64_t left_corrected_timestamp_ns,
                                                 const MpFrame* right,
                                                 uint64_t right_corrected_timestamp_ns,
                                                 uint64_t max_skew_ns,
                                                 MpStereoPair* out_pair)
{
    uint64_t skew;

    if (!left || !right || !left->data || !right->data || !out_pair) {
        return MP_STEREO_SYNC_REJECT;
    }
    if (left->format != right->format ||
        left->width != right->width ||
        left->height != right->height) {
        return MP_STEREO_SYNC_REJECT;
    }

    skew = mp_abs_diff_u64(left_corrected_timestamp_ns, right_corrected_timestamp_ns);
    if (skew > max_skew_ns) {
        return left_corrected_timestamp_ns < right_corrected_timestamp_ns ? MP_STEREO_SYNC_DROP_LEFT
                                                                          : MP_STEREO_SYNC_DROP_RIGHT;
    }

    out_pair->left = left;
    out_pair->right = right;
    out_pair->left_corrected_timestamp_ns = left_corrected_timestamp_ns;
    out_pair->right_corrected_timestamp_ns = right_corrected_timestamp_ns;
    out_pair->pair_timestamp_ns = left_corrected_timestamp_ns > right_corrected_timestamp_ns
                                      ? left_corrected_timestamp_ns
                                      : right_corrected_timestamp_ns;
    out_pair->skew_ns = skew;
    return MP_STEREO_SYNC_PAIR;
}
