#include "mp_preprocess.h"

#include "mp_sbs_packer.h"
#include "mp_stereo_sync.h"

MpPreprocessConfig mp_preprocess_config_default(uint32_t fps)
{
    MpPreprocessConfig config;
    config.max_stereo_skew_ns = fps >= 50u ? 3000000u : 6000000u;
    config.enable_greenback = 1;
    config.use_clock_sync = 0;
    config.left_clock = 0;
    config.right_clock = 0;
    config.sync_monitor = 0;
    config.foreground_workspace = 0;
    config.foreground = mp_foreground_config_default();
    config.greenback = mp_greenback_key_default();
    return config;
}

int mp_preprocess_stereo_to_sbs_nv12(const MpPreprocessConfig* config,
                                     MpFrame* left_work,
                                     MpFrame* right_work,
                                     MpFrame* out_sbs,
                                     MpEdgeStats* stats,
                                     MpPreprocessResult* out_result)
{
    MpPreprocessConfig local_config;
    MpStereoPair pair;
    MpStereoSyncResult sync_result;
    uint64_t left_ts;
    uint64_t right_ts;
    int rc;

    if (!left_work || !right_work || !out_sbs) {
        return -1;
    }

    local_config = config ? *config : mp_preprocess_config_default(60);
    if (local_config.max_stereo_skew_ns == 0u || local_config.max_stereo_skew_ns > 50000000u) {
        local_config.max_stereo_skew_ns = 6000000u;
    }

    left_ts = left_work->timestamp_ns;
    right_ts = right_work->timestamp_ns;
    if (local_config.use_clock_sync &&
        local_config.left_clock &&
        local_config.right_clock &&
        local_config.left_clock->locked &&
        local_config.right_clock->locked) {
        left_ts = mp_clock_sync_remote_to_local(local_config.left_clock, left_ts);
        right_ts = mp_clock_sync_remote_to_local(local_config.right_clock, right_ts);
    }

    sync_result = mp_stereo_sync_pair_corrected(left_work,
                                                left_ts,
                                                right_work,
                                                right_ts,
                                                local_config.max_stereo_skew_ns,
                                                &pair);
    if (sync_result != MP_STEREO_SYNC_PAIR) {
        if (stats) {
            mp_stats_on_drop(stats);
        }
        return sync_result == MP_STEREO_SYNC_DROP_LEFT ? 1 :
               sync_result == MP_STEREO_SYNC_DROP_RIGHT ? 2 : -2;
    }

    if (local_config.sync_monitor) {
        rc = mp_sync_monitor_observe_pair(local_config.sync_monitor,
                                          pair.left_corrected_timestamp_ns,
                                          pair.right_corrected_timestamp_ns,
                                          pair.pair_timestamp_ns);
        if (rc != 0) {
            if (stats) {
                mp_stats_on_drop(stats);
            }
            return 3;
        }
    }

    if (local_config.enable_greenback) {
        if (local_config.foreground_workspace) {
            rc = mp_foreground_greenback_remove_nv12(left_work,
                                                     local_config.foreground_workspace,
                                                     &local_config.foreground);
            if (rc != 0) {
                return -3;
            }
            rc = mp_foreground_greenback_remove_nv12(right_work,
                                                     local_config.foreground_workspace,
                                                     &local_config.foreground);
            if (rc != 0) {
                return -4;
            }
        } else {
            rc = mp_greenback_remove_background_nv12(left_work, &local_config.greenback);
            if (rc != 0) {
                return -3;
            }
            rc = mp_greenback_remove_background_nv12(right_work, &local_config.greenback);
            if (rc != 0) {
                return -4;
            }
        }
        if (stats) {
            mp_stats_on_mask(stats);
            mp_stats_on_mask(stats);
        }
    }

    rc = mp_sbs_pack_nv12(left_work, right_work, out_sbs);
    if (rc != 0) {
        return -5;
    }

    out_sbs->timestamp_ns = pair.pair_timestamp_ns;
    if (out_result) {
        out_result->pair_timestamp_ns = pair.pair_timestamp_ns;
        out_result->stereo_skew_ns = pair.skew_ns;
        out_result->left_corrected_timestamp_ns = pair.left_corrected_timestamp_ns;
        out_result->right_corrected_timestamp_ns = pair.right_corrected_timestamp_ns;
        out_result->sync_state = local_config.sync_monitor
                                     ? mp_sync_monitor_state(local_config.sync_monitor)
                                     : MP_SYNC_UNLOCKED;
        out_result->left_background_removed = local_config.enable_greenback ? 1u : 0u;
        out_result->right_background_removed = local_config.enable_greenback ? 1u : 0u;
    }

    return 0;
}
