#include "imt.h"

#include <stdlib.h>
#include <string.h>

/* Group size used when no per-chunk weights are supplied (R7.4 default). */
#define IMT_PACKETIZE_UNIFORM_GROUP_SIZE 8u

int imt_packetizer_init(ImtPacketizer* packetizer, size_t max_payload,
                        uint32_t max_map_tiles)
{
    size_t map_payload;

    if (!packetizer) {
        return -1;
    }
    if (max_payload == 0u) {
        max_payload = IMT_DEFAULT_MAX_PAYLOAD;
    }
    if (max_map_tiles == 0u) {
        max_map_tiles = IMT_DEFAULT_MAP_TILES;
    }
    if (max_payload > 0xFFFFu) {
        return -2;
    }
    map_payload = (size_t)IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE + max_map_tiles;
    if (map_payload > 0xFFFFu) {
        return -2;
    }

    memset(packetizer, 0, sizeof(*packetizer));
    packetizer->datagram_capacity = IMT_WIRE_HEADER_SIZE +
        (map_payload > max_payload ? map_payload : max_payload);
    packetizer->datagram = (uint8_t*)malloc(packetizer->datagram_capacity);
    packetizer->parity = (uint8_t*)malloc(max_payload);
    if (!packetizer->datagram || !packetizer->parity) {
        imt_packetizer_destroy(packetizer);
        return -3;
    }
    packetizer->max_payload = max_payload;
    packetizer->max_map_tiles = max_map_tiles;
    return 0;
}

void imt_packetizer_destroy(ImtPacketizer* packetizer)
{
    if (!packetizer) {
        return;
    }
    free(packetizer->datagram);
    free(packetizer->parity);
    memset(packetizer, 0, sizeof(*packetizer));
}

static size_t imt_packetizer_chunk_size(uint32_t frame_size, size_t max_payload,
                                        uint32_t chunk_index)
{
    /* R6.2: every chunk except the last is full, so each chunk size is a
     * pure function of frame_size, max_payload and the chunk index. */
    const size_t offset = (size_t)chunk_index * max_payload;
    const size_t remaining = (size_t)frame_size - offset;
    return remaining > max_payload ? max_payload : remaining;
}

int imt_packetizer_send_frame(ImtPacketizer* packetizer,
                              const uint8_t* frame,
                              uint32_t frame_size,
                              uint64_t frame_seq,
                              uint64_t capture_timestamp_ns,
                              uint16_t flags,
                              const uint8_t* chunk_weights_norm,
                              uint32_t chunk_weight_count,
                              int (*emit)(const uint8_t* datagram, size_t len, void* user),
                              void* user)
{
    uint32_t chunk_count;
    uint32_t chunk;
    uint32_t group;
    ImtWireHeader header;

    if (!packetizer || !packetizer->datagram || !frame || !emit) {
        return -1;
    }
    if (frame_size == 0u) {
        return -2;
    }
    chunk_count = (uint32_t)((frame_size + packetizer->max_payload - 1u) /
                             packetizer->max_payload);
    if (chunk_count > 0xFFFFu) {
        return -3;
    }
    if (chunk_weights_norm && chunk_weight_count < chunk_count) {
        return -4;
    }

    memset(&header, 0, sizeof(header));
    header.flags = flags;
    header.frame_seq = frame_seq;
    header.capture_timestamp_ns = capture_timestamp_ns;
    header.frame_size = frame_size;
    header.chunk_count = (uint16_t)chunk_count;

    chunk = 0;
    group = 0;
    while (chunk < chunk_count) {
        uint32_t group_size;
        uint32_t i;
        size_t parity_size;
        int rc;

        if (chunk_weights_norm) {
            group_size = (uint32_t)imt_fec_group_size(chunk_weights_norm[chunk]);
        } else {
            group_size = IMT_PACKETIZE_UNIFORM_GROUP_SIZE;
        }
        if (group_size > chunk_count - chunk) {
            group_size = chunk_count - chunk;
        }
        if (group > 0xFFFFu) {
            return -5;
        }

        /* R7.1: parity length is the largest payload in the group; chunks
         * are full except the last, so the group's first chunk is largest. */
        parity_size = imt_packetizer_chunk_size(frame_size, packetizer->max_payload, chunk);
        memset(packetizer->parity, 0, parity_size);

        for (i = 0; i < group_size; ++i) {
            const uint32_t index = chunk + i;
            const size_t offset = (size_t)index * packetizer->max_payload;
            const size_t payload_size =
                imt_packetizer_chunk_size(frame_size, packetizer->max_payload, index);

            header.type = IMT_PACKET_TYPE_SLICE;
            header.chunk_index = (uint16_t)index;
            header.payload_size = (uint16_t)payload_size;
            header.fec_group = (uint16_t)group;
            header.fec_group_size = (uint8_t)group_size;
            header.fec_index = (uint8_t)i;
            rc = imt_wire_encode_header(&header, packetizer->datagram,
                                        packetizer->datagram_capacity);
            if (rc != 0) {
                return rc;
            }
            memcpy(packetizer->datagram + IMT_WIRE_HEADER_SIZE, frame + offset, payload_size);
            rc = emit(packetizer->datagram, IMT_WIRE_HEADER_SIZE + payload_size, user);
            if (rc != 0) {
                return rc;
            }
            rc = imt_fec_xor_accumulate(packetizer->parity, parity_size,
                                        frame + offset, payload_size);
            if (rc != 0) {
                return rc;
            }
        }

        header.type = IMT_PACKET_TYPE_PARITY;
        header.chunk_index = (uint16_t)chunk; /* group base for the assembler */
        header.payload_size = (uint16_t)parity_size;
        header.fec_group = (uint16_t)group;
        header.fec_group_size = (uint8_t)group_size;
        header.fec_index = IMT_FEC_INDEX_PARITY;
        rc = imt_wire_encode_header(&header, packetizer->datagram,
                                    packetizer->datagram_capacity);
        if (rc != 0) {
            return rc;
        }
        memcpy(packetizer->datagram + IMT_WIRE_HEADER_SIZE, packetizer->parity, parity_size);
        rc = emit(packetizer->datagram, IMT_WIRE_HEADER_SIZE + parity_size, user);
        if (rc != 0) {
            return rc;
        }

        chunk += group_size;
        group += 1u;
    }
    return 0;
}

int imt_packetizer_send_map(ImtPacketizer* packetizer,
                            const ImtMap* map,
                            uint64_t frame_seq,
                            uint64_t capture_timestamp_ns,
                            int (*emit)(const uint8_t* datagram, size_t len, void* user),
                            void* user)
{
    ImtWireHeader header;
    ImtMapPayloadHeader map_header;
    size_t payload_size;
    int rc;

    if (!packetizer || !packetizer->datagram || !map || !map->weights || !emit) {
        return -1;
    }
    if (map->tile_count == 0u || map->tile_count > packetizer->max_map_tiles) {
        return -2;
    }

    payload_size = (size_t)IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE + map->tile_count;

    memset(&header, 0, sizeof(header));
    header.type = IMT_PACKET_TYPE_MAP;
    header.frame_seq = frame_seq;
    header.capture_timestamp_ns = capture_timestamp_ns;
    header.frame_size = (uint32_t)payload_size;
    header.chunk_index = 0;
    header.chunk_count = 1;
    header.payload_size = (uint16_t)payload_size;
    rc = imt_wire_encode_header(&header, packetizer->datagram,
                                packetizer->datagram_capacity);
    if (rc != 0) {
        return rc;
    }

    map_header.cols = map->cols;
    map_header.rows = map->rows;
    map_header.tile_size = map->tile_size;
    rc = imt_wire_encode_map_payload_header(&map_header,
                                            packetizer->datagram + IMT_WIRE_HEADER_SIZE,
                                            packetizer->datagram_capacity - IMT_WIRE_HEADER_SIZE);
    if (rc != 0) {
        return rc;
    }
    memcpy(packetizer->datagram + IMT_WIRE_HEADER_SIZE + IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE,
           map->weights, map->tile_count);

    rc = emit(packetizer->datagram, IMT_WIRE_HEADER_SIZE + payload_size, user);
    if (rc != 0) {
        return rc;
    }
    return 0;
}
