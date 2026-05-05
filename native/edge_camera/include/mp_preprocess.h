#ifndef MP_PREPROCESS_H
#define MP_PREPROCESS_H

#include "mp_edge_config.h"
#include "mp_clock_sync.h"
#include "mp_foreground.h"
#include "mp_frame.h"
#include "mp_mask.h"
#include "mp_stats.h"
#include "mp_sync_monitor.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MpPreprocessConfig {
    uint64_t max_stereo_skew_ns;
    uint8_t enable_greenback;
    uint8_t use_clock_sync;
    const MpClockSync* left_clock;
    const MpClockSync* right_clock;
    MpSyncMonitor* sync_monitor;
    MpForegroundWorkspace* foreground_workspace;
    MpForegroundConfig foreground;
    MpGreenbackKeyConfig greenback;
} MpPreprocessConfig;

typedef struct MpPreprocessResult {
    uint64_t pair_timestamp_ns;
    uint64_t stereo_skew_ns;
    uint64_t left_corrected_timestamp_ns;
    uint64_t right_corrected_timestamp_ns;
    MpSyncLockState sync_state;
    uint8_t left_background_removed;
    uint8_t right_background_removed;
} MpPreprocessResult;

MpPreprocessConfig mp_preprocess_config_default(uint32_t fps);

int mp_preprocess_stereo_to_sbs_nv12(const MpPreprocessConfig* config,
                                     MpFrame* left_work,
                                     MpFrame* right_work,
                                     MpFrame* out_sbs,
                                     MpEdgeStats* stats,
                                     MpPreprocessResult* out_result);

#ifdef __cplusplus
}
#endif

#endif
