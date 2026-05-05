#ifndef MP_SYNC_MONITOR_H
#define MP_SYNC_MONITOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MpSyncLockState {
    MP_SYNC_UNLOCKED = 0,
    MP_SYNC_WARMING = 1,
    MP_SYNC_LOCKED = 2,
    MP_SYNC_DEGRADED = 3
} MpSyncLockState;

typedef struct MpSyncMonitorConfig {
    uint64_t target_frame_period_ns;
    uint64_t max_pair_skew_ns;
    uint64_t max_frame_period_error_ns;
    uint32_t warmup_pairs;
    uint32_t degrade_after_bad_pairs;
} MpSyncMonitorConfig;

typedef struct MpSyncMonitor {
    MpSyncMonitorConfig config;
    MpSyncLockState state;
    uint64_t accepted_pairs;
    uint64_t rejected_pairs;
    uint64_t last_pair_timestamp_ns;
    uint64_t skew_ewma_ns;
    uint64_t frame_period_error_ewma_ns;
    uint32_t consecutive_good_pairs;
    uint32_t consecutive_bad_pairs;
} MpSyncMonitor;

MpSyncMonitorConfig mp_sync_monitor_config_default(uint32_t fps);
void mp_sync_monitor_init(MpSyncMonitor* monitor, const MpSyncMonitorConfig* config);
int mp_sync_monitor_observe_pair(MpSyncMonitor* monitor,
                                 uint64_t left_timestamp_ns,
                                 uint64_t right_timestamp_ns,
                                 uint64_t pair_timestamp_ns);
MpSyncLockState mp_sync_monitor_state(const MpSyncMonitor* monitor);

#ifdef __cplusplus
}
#endif

#endif

