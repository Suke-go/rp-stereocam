#include "mp_stats.h"

#include <string.h>

void mp_stats_reset(MpEdgeStats* stats)
{
    if (!stats) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
}

void mp_stats_on_capture(MpEdgeStats* stats, uint64_t timestamp_ns)
{
    if (!stats) {
        return;
    }
    stats->captured_frames += 1u;
    stats->last_capture_timestamp_ns = timestamp_ns;
}

void mp_stats_on_mask(MpEdgeStats* stats)
{
    if (!stats) {
        return;
    }
    stats->masked_frames += 1u;
}

void mp_stats_on_full_mask(MpEdgeStats* stats)
{
    if (!stats) {
        return;
    }
    stats->fully_masked_frames += 1u;
}

void mp_stats_on_drop(MpEdgeStats* stats)
{
    if (!stats) {
        return;
    }
    stats->dropped_frames += 1u;
}

void mp_stats_on_encode(MpEdgeStats* stats)
{
    if (!stats) {
        return;
    }
    stats->encoded_frames += 1u;
}

void mp_stats_on_transmit(MpEdgeStats* stats, uint64_t timestamp_ns)
{
    if (!stats) {
        return;
    }
    stats->transmitted_frames += 1u;
    stats->last_send_timestamp_ns = timestamp_ns;
}

