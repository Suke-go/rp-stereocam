#ifndef MPS_LATENCY_GATE_H
#define MPS_LATENCY_GATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MPS_LATENCY_GATE_TRACKED_FRAMES 64u

typedef struct MpsLatencyFrameTag {
    uint64_t sequence;
    uint64_t capture_timestamp_ns;
    uint8_t valid;
} MpsLatencyFrameTag;

typedef struct MpsLatencyGate {
    uint32_t max_pending_frames;
    uint64_t max_frame_age_ns;
    uint64_t pushed_frames;
    uint64_t emitted_frames;
    uint64_t dropped_pending;
    uint64_t dropped_age;
    MpsLatencyFrameTag tags[MPS_LATENCY_GATE_TRACKED_FRAMES];
} MpsLatencyGate;

void mps_latency_gate_init(MpsLatencyGate* gate, uint32_t max_pending_frames, uint64_t max_frame_age_ns);
uint64_t mps_latency_gate_pending(const MpsLatencyGate* gate);
int mps_latency_gate_accept(MpsLatencyGate* gate, uint64_t capture_timestamp_ns, uint64_t now_ns);
void mps_latency_gate_mark_pushed(MpsLatencyGate* gate, uint64_t sequence, uint64_t capture_timestamp_ns);
uint64_t mps_latency_gate_mark_emitted(MpsLatencyGate* gate, uint64_t sequence, uint64_t fallback_timestamp_ns);

#ifdef __cplusplus
}
#endif

#endif
