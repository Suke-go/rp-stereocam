#include "mp_frame_ring.h"

#include <stdlib.h>
#include <string.h>

static void mp_frame_ring_zero(MpFrameRing* ring)
{
    if (!ring) {
        return;
    }
    ring->slots = NULL;
    ring->capacity = 0;
    ring->slot_size = 0;
    atomic_init(&ring->write_sequence, 0);
    atomic_init(&ring->read_sequence, 0);
    atomic_init(&ring->dropped_frames, 0);
}

int mp_frame_ring_init(MpFrameRing* ring, uint32_t capacity, size_t slot_size)
{
    uint32_t i;

    if (!ring || capacity == 0 || capacity > 8 || slot_size == 0) {
        return -1;
    }

    mp_frame_ring_zero(ring);
    ring->slots = (MpFrameRingSlot*)calloc(capacity, sizeof(MpFrameRingSlot));
    if (!ring->slots) {
        return -2;
    }

    ring->capacity = capacity;
    ring->slot_size = slot_size;

    for (i = 0; i < capacity; ++i) {
        atomic_init(&ring->slots[i].version, 0);
        ring->slots[i].storage = (uint8_t*)malloc(slot_size);
        ring->slots[i].storage_size = slot_size;
        if (!ring->slots[i].storage) {
            mp_frame_ring_destroy(ring);
            return -3;
        }
    }

    return 0;
}

void mp_frame_ring_destroy(MpFrameRing* ring)
{
    uint32_t i;

    if (!ring) {
        return;
    }

    if (ring->slots) {
        for (i = 0; i < ring->capacity; ++i) {
            free(ring->slots[i].storage);
            ring->slots[i].storage = NULL;
            ring->slots[i].storage_size = 0;
        }
        free(ring->slots);
    }

    mp_frame_ring_zero(ring);
}

int mp_frame_ring_write_latest(MpFrameRing* ring, const MpFrame* frame)
{
    MpFrameRingSlot* slot;
    uint64_t sequence;
    uint64_t read_sequence;
    uint64_t minimum_read_sequence;

    if (!ring || !frame || !frame->data || !ring->slots || ring->capacity == 0) {
        return -1;
    }
    if (frame->data_size > ring->slot_size) {
        return -2;
    }

    sequence = atomic_load_explicit(&ring->write_sequence, memory_order_relaxed) + 1u;
    slot = &ring->slots[sequence % ring->capacity];

    atomic_store_explicit(&slot->version, (sequence * 2u) - 1u, memory_order_release);
    memcpy(slot->storage, frame->data, frame->data_size);
    slot->frame = *frame;
    slot->frame.data = slot->storage;
    slot->frame.sequence = sequence;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&slot->version, sequence * 2u, memory_order_release);
    atomic_store_explicit(&ring->write_sequence, sequence, memory_order_release);

    read_sequence = atomic_load_explicit(&ring->read_sequence, memory_order_acquire);
    minimum_read_sequence = sequence > ring->capacity ? sequence - ring->capacity : 0u;
    if (read_sequence < minimum_read_sequence) {
        atomic_fetch_add_explicit(&ring->dropped_frames,
                                  minimum_read_sequence - read_sequence,
                                  memory_order_relaxed);
        atomic_store_explicit(&ring->read_sequence, minimum_read_sequence, memory_order_release);
    }

    return 0;
}

int mp_frame_ring_read_latest(MpFrameRing* ring, MpFrame* out_frame, uint8_t* out_storage, size_t out_storage_size)
{
    MpFrameRingSlot* slot;
    uint64_t sequence;
    uint64_t version_before;
    uint64_t version_after;
    MpFrame frame_copy;

    if (!ring || !out_frame || !out_storage || !ring->slots || ring->capacity == 0) {
        return -1;
    }

    sequence = atomic_load_explicit(&ring->write_sequence, memory_order_acquire);
    if (sequence == 0) {
        return 1;
    }

    slot = &ring->slots[sequence % ring->capacity];
    version_before = atomic_load_explicit(&slot->version, memory_order_acquire);
    if ((version_before & 1u) != 0u || version_before != sequence * 2u) {
        return 2;
    }

    frame_copy = slot->frame;
    if (frame_copy.data_size > out_storage_size) {
        return -2;
    }

    memcpy(out_storage, slot->storage, frame_copy.data_size);
    atomic_thread_fence(memory_order_acquire);
    version_after = atomic_load_explicit(&slot->version, memory_order_acquire);
    if (version_before != version_after) {
        return 2;
    }

    *out_frame = frame_copy;
    out_frame->data = out_storage;
    atomic_store_explicit(&ring->read_sequence, sequence, memory_order_release);
    return 0;
}

uint64_t mp_frame_ring_dropped_frames(const MpFrameRing* ring)
{
    if (!ring) {
        return 0;
    }
    return atomic_load_explicit(&ring->dropped_frames, memory_order_relaxed);
}
