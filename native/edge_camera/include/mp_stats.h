#ifndef MP_STATS_H
#define MP_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MpEdgeStats {
    uint64_t captured_frames;
    uint64_t masked_frames;
    uint64_t fully_masked_frames;
    uint64_t dropped_frames;
    uint64_t encoded_frames;
    uint64_t transmitted_frames;
    uint64_t last_capture_timestamp_ns;
    uint64_t last_send_timestamp_ns;
} MpEdgeStats;

void mp_stats_reset(MpEdgeStats* stats);
void mp_stats_on_capture(MpEdgeStats* stats, uint64_t timestamp_ns);
void mp_stats_on_mask(MpEdgeStats* stats);
void mp_stats_on_full_mask(MpEdgeStats* stats);
void mp_stats_on_drop(MpEdgeStats* stats);
void mp_stats_on_encode(MpEdgeStats* stats);
void mp_stats_on_transmit(MpEdgeStats* stats, uint64_t timestamp_ns);

#ifdef __cplusplus
}
#endif

#endif

