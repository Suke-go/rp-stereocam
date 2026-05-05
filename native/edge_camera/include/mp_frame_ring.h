#ifndef MP_FRAME_RING_H
#define MP_FRAME_RING_H

#include "mp_frame.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MpFrameRingSlot {
    atomic_uint_fast64_t version;
    uint8_t* storage;
    size_t storage_size;
    MpFrame frame;
} MpFrameRingSlot;

typedef struct MpFrameRing {
    MpFrameRingSlot* slots;
    uint32_t capacity;
    size_t slot_size;
    atomic_uint_fast64_t write_sequence;
    atomic_uint_fast64_t read_sequence;
    atomic_uint_fast64_t dropped_frames;
} MpFrameRing;

int mp_frame_ring_init(MpFrameRing* ring, uint32_t capacity, size_t slot_size);
void mp_frame_ring_destroy(MpFrameRing* ring);
int mp_frame_ring_write_latest(MpFrameRing* ring, const MpFrame* frame);
int mp_frame_ring_read_latest(MpFrameRing* ring, MpFrame* out_frame, uint8_t* out_storage, size_t out_storage_size);
uint64_t mp_frame_ring_dropped_frames(const MpFrameRing* ring);

#ifdef __cplusplus
}
#endif

#endif
