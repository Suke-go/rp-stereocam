#include "mp_clock_sync.h"

#include <limits.h>
#include <string.h>

static uint64_t mp_abs_i64_to_u64(int64_t value)
{
    return value >= 0 ? (uint64_t)value : (uint64_t)(-value);
}

static int64_t mp_clamp_i64(int64_t value, int64_t min_value, int64_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static uint64_t mp_ewma_u64(uint64_t old_value, uint64_t new_value, uint32_t old_weight, uint32_t new_weight)
{
    return (old_value * old_weight + new_value * new_weight) / (old_weight + new_weight);
}

void mp_clock_sync_reset(MpClockSync* sync)
{
    if (!sync) {
        return;
    }
    memset(sync, 0, sizeof(*sync));
}

int mp_clock_sync_update_ptp(MpClockSync* sync, const MpPtpSample* sample)
{
    int64_t offset_a;
    int64_t offset_b;
    int64_t offset;
    int64_t previous_offset;
    int64_t drift_ppb;
    uint64_t round_trip_ns;
    uint64_t remote_processing_ns;
    uint64_t delay_ns;
    uint64_t local_elapsed_ns;
    uint64_t offset_delta_ns;

    if (!sync || !sample) {
        return -1;
    }
    if (sample->local_rx_ns < sample->local_tx_ns ||
        sample->remote_tx_ns < sample->remote_rx_ns) {
        return -2;
    }

    round_trip_ns = sample->local_rx_ns - sample->local_tx_ns;
    remote_processing_ns = sample->remote_tx_ns - sample->remote_rx_ns;
    if (round_trip_ns < remote_processing_ns) {
        return -3;
    }

    offset_a = (int64_t)sample->remote_rx_ns - (int64_t)sample->local_tx_ns;
    offset_b = (int64_t)sample->remote_tx_ns - (int64_t)sample->local_rx_ns;
    offset = (offset_a + offset_b) / 2;
    delay_ns = (round_trip_ns - remote_processing_ns) / 2u;

    previous_offset = sync->remote_to_local_offset_ns;
    if (sync->sample_count == 0u) {
        sync->remote_to_local_offset_ns = -offset;
        sync->one_way_delay_ns = delay_ns;
        sync->jitter_ns = 0;
        sync->drift_ppb = 0;
        sync->locked = 1;
    } else {
        const int64_t target_remote_to_local = -offset;
        const int64_t offset_error = target_remote_to_local - previous_offset;
        sync->remote_to_local_offset_ns = previous_offset + offset_error / 8;
        sync->one_way_delay_ns = mp_ewma_u64(sync->one_way_delay_ns, delay_ns, 7, 1);
        sync->jitter_ns = mp_ewma_u64(sync->jitter_ns, mp_abs_i64_to_u64(offset_error), 7, 1);

        local_elapsed_ns = sample->local_rx_ns - sync->last_local_sample_ns;
        if (local_elapsed_ns > 0u) {
            offset_delta_ns = mp_abs_i64_to_u64(sync->remote_to_local_offset_ns - previous_offset);
            drift_ppb = (int64_t)((offset_delta_ns * 1000000000ull) / local_elapsed_ns);
            if (sync->remote_to_local_offset_ns < previous_offset) {
                drift_ppb = -drift_ppb;
            }
            sync->drift_ppb = mp_clamp_i64((sync->drift_ppb * 7 + drift_ppb) / 8,
                                           -1000000,
                                           1000000);
        }
    }

    sync->last_local_sample_ns = sample->local_rx_ns;
    sync->sample_count += 1u;
    return 0;
}

uint64_t mp_clock_sync_remote_to_local(const MpClockSync* sync, uint64_t remote_timestamp_ns)
{
    int64_t corrected;

    if (!sync || !sync->locked) {
        return remote_timestamp_ns;
    }

    corrected = (int64_t)remote_timestamp_ns + sync->remote_to_local_offset_ns;
    return corrected < 0 ? 0u : (uint64_t)corrected;
}

uint64_t mp_clock_sync_local_to_remote(const MpClockSync* sync, uint64_t local_timestamp_ns)
{
    int64_t corrected;

    if (!sync || !sync->locked) {
        return local_timestamp_ns;
    }

    corrected = (int64_t)local_timestamp_ns - sync->remote_to_local_offset_ns;
    return corrected < 0 ? 0u : (uint64_t)corrected;
}

int mp_clock_sync_is_usable(const MpClockSync* sync, uint64_t max_delay_ns, uint64_t max_jitter_ns)
{
    if (!sync || !sync->locked || sync->sample_count < 2u) {
        return 0;
    }
    if (max_delay_ns > 0u && sync->one_way_delay_ns > max_delay_ns) {
        return 0;
    }
    if (max_jitter_ns > 0u && sync->jitter_ns > max_jitter_ns) {
        return 0;
    }
    return 1;
}

