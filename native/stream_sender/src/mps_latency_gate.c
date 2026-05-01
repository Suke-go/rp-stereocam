#include "mps_latency_gate.h"

#include <string.h>

#define MPS_LATENCY_GATE_CLOCK_SANITY_NS 10000000000ull

void mps_latency_gate_init(MpsLatencyGate* gate, uint32_t max_pending_frames, uint64_t max_frame_age_ns)
{
    if (!gate) {
        return;
    }

    memset(gate, 0, sizeof(*gate));
    gate->max_pending_frames = max_pending_frames;
    gate->max_frame_age_ns = max_frame_age_ns;
}

uint64_t mps_latency_gate_pending(const MpsLatencyGate* gate)
{
    if (!gate || gate->pushed_frames < gate->emitted_frames) {
        return 0u;
    }
    return gate->pushed_frames - gate->emitted_frames;
}

int mps_latency_gate_accept(MpsLatencyGate* gate, uint64_t capture_timestamp_ns, uint64_t now_ns)
{
    if (!gate) {
        return 0;
    }

    if (gate->max_pending_frames > 0u &&
        mps_latency_gate_pending(gate) >= (uint64_t)gate->max_pending_frames) {
        gate->dropped_pending += 1u;
        return 0;
    }

    if (capture_timestamp_ns != 0u && gate->max_frame_age_ns != 0u && now_ns > capture_timestamp_ns) {
        const uint64_t age_ns = now_ns - capture_timestamp_ns;
        if (age_ns > gate->max_frame_age_ns && age_ns < MPS_LATENCY_GATE_CLOCK_SANITY_NS) {
            gate->dropped_age += 1u;
            return 0;
        }
    }

    return 1;
}

void mps_latency_gate_mark_pushed(MpsLatencyGate* gate, uint64_t sequence, uint64_t capture_timestamp_ns)
{
    MpsLatencyFrameTag* tag;

    if (!gate) {
        return;
    }

    gate->pushed_frames += 1u;
    tag = &gate->tags[sequence % MPS_LATENCY_GATE_TRACKED_FRAMES];
    tag->sequence = sequence;
    tag->capture_timestamp_ns = capture_timestamp_ns;
    tag->valid = 1u;
}

uint64_t mps_latency_gate_mark_emitted(MpsLatencyGate* gate, uint64_t sequence, uint64_t fallback_timestamp_ns)
{
    MpsLatencyFrameTag* tag;
    uint64_t timestamp_ns = fallback_timestamp_ns;

    if (!gate) {
        return timestamp_ns;
    }

    if (gate->emitted_frames < gate->pushed_frames) {
        gate->emitted_frames += 1u;
    }

    tag = &gate->tags[sequence % MPS_LATENCY_GATE_TRACKED_FRAMES];
    if (tag->valid && tag->sequence == sequence && tag->capture_timestamp_ns != 0u) {
        timestamp_ns = tag->capture_timestamp_ns;
        tag->valid = 0u;
    }

    return timestamp_ns;
}
