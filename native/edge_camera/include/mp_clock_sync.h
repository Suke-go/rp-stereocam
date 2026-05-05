#ifndef MP_CLOCK_SYNC_H
#define MP_CLOCK_SYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MpPtpSample {
    uint64_t local_tx_ns;
    uint64_t remote_rx_ns;
    uint64_t remote_tx_ns;
    uint64_t local_rx_ns;
} MpPtpSample;

typedef struct MpClockSync {
    int64_t remote_to_local_offset_ns;
    int64_t drift_ppb;
    uint64_t one_way_delay_ns;
    uint64_t jitter_ns;
    uint64_t last_local_sample_ns;
    uint64_t sample_count;
    uint8_t locked;
} MpClockSync;

void mp_clock_sync_reset(MpClockSync* sync);
int mp_clock_sync_update_ptp(MpClockSync* sync, const MpPtpSample* sample);
uint64_t mp_clock_sync_remote_to_local(const MpClockSync* sync, uint64_t remote_timestamp_ns);
uint64_t mp_clock_sync_local_to_remote(const MpClockSync* sync, uint64_t local_timestamp_ns);
int mp_clock_sync_is_usable(const MpClockSync* sync, uint64_t max_delay_ns, uint64_t max_jitter_ns);

#ifdef __cplusplus
}
#endif

#endif

