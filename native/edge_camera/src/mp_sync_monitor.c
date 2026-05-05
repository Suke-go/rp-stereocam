#include "mp_sync_monitor.h"

#include <string.h>

static uint64_t mp_abs_diff_u64(uint64_t a, uint64_t b)
{
    return a >= b ? a - b : b - a;
}

static uint64_t mp_ewma_u64(uint64_t old_value, uint64_t new_value, uint32_t old_weight, uint32_t new_weight)
{
    return (old_value * old_weight + new_value * new_weight) / (old_weight + new_weight);
}

MpSyncMonitorConfig mp_sync_monitor_config_default(uint32_t fps)
{
    MpSyncMonitorConfig config;
    if (fps == 0u) {
        fps = 60u;
    }
    config.target_frame_period_ns = 1000000000ull / fps;
    config.max_pair_skew_ns = fps >= 50u ? 3000000ull : 6000000ull;
    config.max_frame_period_error_ns = config.target_frame_period_ns / 4u;
    config.warmup_pairs = 30u;
    config.degrade_after_bad_pairs = 3u;
    return config;
}

void mp_sync_monitor_init(MpSyncMonitor* monitor, const MpSyncMonitorConfig* config)
{
    MpSyncMonitorConfig local_config;

    if (!monitor) {
        return;
    }

    local_config = config ? *config : mp_sync_monitor_config_default(60u);
    memset(monitor, 0, sizeof(*monitor));
    monitor->config = local_config;
    monitor->state = MP_SYNC_WARMING;
}

int mp_sync_monitor_observe_pair(MpSyncMonitor* monitor,
                                 uint64_t left_timestamp_ns,
                                 uint64_t right_timestamp_ns,
                                 uint64_t pair_timestamp_ns)
{
    uint64_t skew_ns;
    uint64_t period_error_ns = 0u;
    int good;

    if (!monitor || pair_timestamp_ns == 0u) {
        return -1;
    }

    skew_ns = mp_abs_diff_u64(left_timestamp_ns, right_timestamp_ns);
    if (monitor->last_pair_timestamp_ns != 0u) {
        const uint64_t frame_period_ns = mp_abs_diff_u64(pair_timestamp_ns, monitor->last_pair_timestamp_ns);
        period_error_ns = mp_abs_diff_u64(frame_period_ns, monitor->config.target_frame_period_ns);
    }

    if (monitor->accepted_pairs + monitor->rejected_pairs == 0u) {
        monitor->skew_ewma_ns = skew_ns;
        monitor->frame_period_error_ewma_ns = period_error_ns;
    } else {
        monitor->skew_ewma_ns = mp_ewma_u64(monitor->skew_ewma_ns, skew_ns, 7u, 1u);
        monitor->frame_period_error_ewma_ns = mp_ewma_u64(monitor->frame_period_error_ewma_ns,
                                                          period_error_ns,
                                                          7u,
                                                          1u);
    }

    good = skew_ns <= monitor->config.max_pair_skew_ns &&
           (monitor->last_pair_timestamp_ns == 0u ||
            period_error_ns <= monitor->config.max_frame_period_error_ns);

    if (good) {
        monitor->accepted_pairs += 1u;
        monitor->consecutive_good_pairs += 1u;
        monitor->consecutive_bad_pairs = 0u;
        if (monitor->consecutive_good_pairs >= monitor->config.warmup_pairs) {
            monitor->state = MP_SYNC_LOCKED;
        } else if (monitor->state == MP_SYNC_UNLOCKED) {
            monitor->state = MP_SYNC_WARMING;
        }
        monitor->last_pair_timestamp_ns = pair_timestamp_ns;
        return 0;
    }

    monitor->rejected_pairs += 1u;
    monitor->consecutive_bad_pairs += 1u;
    monitor->consecutive_good_pairs = 0u;
    if (monitor->consecutive_bad_pairs >= monitor->config.degrade_after_bad_pairs) {
        monitor->state = monitor->accepted_pairs > 0u ? MP_SYNC_DEGRADED : MP_SYNC_UNLOCKED;
    }
    return 1;
}

MpSyncLockState mp_sync_monitor_state(const MpSyncMonitor* monitor)
{
    return monitor ? monitor->state : MP_SYNC_UNLOCKED;
}

