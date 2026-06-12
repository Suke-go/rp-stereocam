#include "imt.h"

#include <stdlib.h>
#include <string.h>

static size_t imt_assembler_chunk_size(const ImtAssembler* assembler, uint32_t chunk_index)
{
    /* R6.2: chunk sizes are recomputed from frame_size alone, so a chunk
     * recovered from parity has an unambiguous length. */
    const size_t offset = (size_t)chunk_index * assembler->max_payload;
    const size_t remaining = assembler->staging_size - offset;
    return remaining > assembler->max_payload ? assembler->max_payload : remaining;
}

static void imt_assembler_reset_active(ImtAssembler* assembler)
{
    uint32_t g;

    assembler->staging_size = 0;
    assembler->active_frame_seq = 0;
    assembler->active_capture_timestamp_ns = 0;
    assembler->active_chunk_count = 0;
    assembler->active_chunks_received = 0;
    assembler->active_valid = 0;
    assembler->active_keyframe = 0;
    assembler->active_codec_config = 0;
    if (assembler->received_chunks && assembler->max_chunks > 0u) {
        memset(assembler->received_chunks, 0, assembler->max_chunks);
    }
    if (assembler->groups) {
        for (g = 0; g < assembler->active_group_limit; ++g) {
            assembler->groups[g].known = 0;
            assembler->groups[g].parity_received = 0;
        }
    }
    assembler->active_group_limit = 0;
}

int imt_assembler_init(ImtAssembler* assembler, size_t max_frame_size,
                       size_t max_payload, uint32_t max_map_tiles)
{
    uint32_t g;

    if (!assembler || max_frame_size == 0u) {
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

    memset(assembler, 0, sizeof(*assembler));
    assembler->max_payload = max_payload;
    assembler->max_chunks = (uint32_t)((max_frame_size + max_payload - 1u) / max_payload);
    if (assembler->max_chunks > 0xFFFFu) {
        return -2;
    }
    /* Smallest FEC group is 2 data chunks (R5.2), so this bounds group ids. */
    assembler->max_groups = (assembler->max_chunks + 1u) / 2u;
    if (assembler->max_groups == 0u) {
        assembler->max_groups = 1u;
    }

    assembler->staging = (uint8_t*)malloc(max_frame_size);
    assembler->latest.data = (uint8_t*)malloc(max_frame_size);
    assembler->received_chunks = (uint8_t*)malloc(assembler->max_chunks);
    assembler->groups = (ImtAssemblerGroup*)malloc(
        (size_t)assembler->max_groups * sizeof(ImtAssemblerGroup));
    assembler->group_parity_block = (uint8_t*)malloc(
        (size_t)assembler->max_groups * max_payload);
    assembler->map_weights = (uint8_t*)malloc(max_map_tiles);
    if (!assembler->staging || !assembler->latest.data || !assembler->received_chunks ||
        !assembler->groups || !assembler->group_parity_block || !assembler->map_weights) {
        imt_assembler_destroy(assembler);
        return -3;
    }

    assembler->staging_capacity = max_frame_size;
    assembler->latest.capacity = max_frame_size;
    assembler->map_capacity = max_map_tiles;
    memset(assembler->groups, 0,
           (size_t)assembler->max_groups * sizeof(ImtAssemblerGroup));
    for (g = 0; g < assembler->max_groups; ++g) {
        assembler->groups[g].parity = assembler->group_parity_block +
                                      (size_t)g * max_payload;
    }
    memset(assembler->received_chunks, 0, assembler->max_chunks);
    /* R2.5: before any MAP arrives the fallback is the uniform map. */
    memset(assembler->map_weights, 128, max_map_tiles);
    return 0;
}

void imt_assembler_destroy(ImtAssembler* assembler)
{
    if (!assembler) {
        return;
    }
    free(assembler->staging);
    free(assembler->latest.data);
    free(assembler->received_chunks);
    free(assembler->groups);
    free(assembler->group_parity_block);
    free(assembler->map_weights);
    memset(assembler, 0, sizeof(*assembler));
}

static int imt_assembler_start_frame(ImtAssembler* assembler, const ImtWireHeader* header)
{
    const uint32_t expected_chunks =
        (uint32_t)(((size_t)header->frame_size + assembler->max_payload - 1u) /
                   assembler->max_payload);

    if (header->frame_size == 0u ||
        header->frame_size > assembler->staging_capacity ||
        header->chunk_count == 0u ||
        header->chunk_count != expected_chunks ||
        header->chunk_count > assembler->max_chunks) {
        return -1;
    }

    assembler->staging_size = header->frame_size;
    assembler->active_frame_seq = header->frame_seq;
    assembler->active_capture_timestamp_ns = header->capture_timestamp_ns;
    assembler->active_chunk_count = header->chunk_count;
    assembler->active_chunks_received = 0;
    assembler->active_valid = 1;
    assembler->active_keyframe = (header->flags & IMT_PACKET_FLAG_KEYFRAME) ? 1u : 0u;
    assembler->active_codec_config = (header->flags & IMT_PACKET_FLAG_CODEC_CONFIG) ? 1u : 0u;
    assembler->active_group_limit = assembler->max_groups;
    memset(assembler->received_chunks, 0, header->chunk_count);
    {
        uint32_t g;
        for (g = 0; g < assembler->max_groups; ++g) {
            assembler->groups[g].known = 0;
            assembler->groups[g].parity_received = 0;
        }
    }
    return 0;
}

static int imt_assembler_publish(ImtAssembler* assembler)
{
    uint8_t* previous;

    if (assembler->staging_size > assembler->latest.capacity) {
        return -1;
    }

    previous = assembler->latest.data;
    assembler->latest.data = assembler->staging;
    assembler->staging = previous;
    assembler->latest.size = assembler->staging_size;
    assembler->latest.frame_seq = assembler->active_frame_seq;
    assembler->latest.capture_timestamp_ns = assembler->active_capture_timestamp_ns;
    assembler->latest.keyframe = assembler->active_keyframe;
    assembler->latest.codec_config = assembler->active_codec_config;
    assembler->frames_completed += 1u;
    imt_assembler_reset_active(assembler);
    return 0;
}

/* R7.2: with exactly one missing data chunk and the parity present, the
 * missing chunk is rebuilt bit-exactly. Returns 0 when a chunk was
 * recovered, 1 when nothing could be done. */
static int imt_assembler_try_recover_group(ImtAssembler* assembler,
                                           ImtAssemblerGroup* group)
{
    uint32_t i;
    uint32_t missing_index = 0;
    uint32_t missing_count = 0;
    size_t missing_offset;
    size_t missing_size;

    if (!group->known || !group->parity_received) {
        return 1;
    }

    for (i = 0; i < group->size; ++i) {
        const uint32_t index = (uint32_t)group->base_index + i;
        if (!assembler->received_chunks[index]) {
            missing_index = index;
            missing_count += 1u;
        }
    }
    if (missing_count != 1u) {
        return 1;
    }

    missing_size = imt_assembler_chunk_size(assembler, missing_index);
    if (missing_size > group->parity_payload_size) {
        return 1; /* corrupt group description; leave the chunk missing */
    }
    missing_offset = (size_t)missing_index * assembler->max_payload;

    /* missing = parity XOR (all other chunks, zero-extended). The parity is
     * at least as long as every chunk in the group (R7.1), so the first
     * missing_size bytes are sufficient. */
    memcpy(assembler->staging + missing_offset, group->parity, missing_size);
    for (i = 0; i < group->size; ++i) {
        const uint32_t index = (uint32_t)group->base_index + i;
        size_t chunk_size;
        size_t j;
        const uint8_t* chunk;

        if (index == missing_index) {
            continue;
        }
        chunk_size = imt_assembler_chunk_size(assembler, index);
        if (chunk_size > missing_size) {
            chunk_size = missing_size;
        }
        chunk = assembler->staging + (size_t)index * assembler->max_payload;
        for (j = 0; j < chunk_size; ++j) {
            assembler->staging[missing_offset + j] ^= chunk[j];
        }
    }

    assembler->received_chunks[missing_index] = 1u;
    assembler->active_chunks_received += 1u;
    assembler->chunks_recovered += 1u;
    return 0;
}

static int imt_assembler_bind_group(ImtAssembler* assembler,
                                    const ImtWireHeader* header,
                                    uint32_t base_index,
                                    ImtAssemblerGroup** out_group)
{
    ImtAssemblerGroup* group;

    if (header->fec_group >= assembler->max_groups ||
        header->fec_group_size == 0u ||
        base_index + header->fec_group_size > assembler->active_chunk_count) {
        return -1;
    }

    group = &assembler->groups[header->fec_group];
    if (group->known) {
        if (group->base_index != base_index || group->size != header->fec_group_size) {
            return -1;
        }
    } else {
        group->known = 1;
        group->base_index = (uint16_t)base_index;
        group->size = header->fec_group_size;
    }
    *out_group = group;
    return 0;
}

static int imt_assembler_handle_map(ImtAssembler* assembler,
                                    const ImtWireHeader* header,
                                    const uint8_t* payload)
{
    ImtMapPayloadHeader map_header;
    uint32_t tile_count;

    if (header->payload_size < IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE) {
        return -6;
    }
    if (imt_wire_decode_map_payload_header(payload, header->payload_size, &map_header) != 0) {
        return -6;
    }
    tile_count = (uint32_t)map_header.cols * map_header.rows;
    if (tile_count == 0u || tile_count > assembler->map_capacity ||
        header->payload_size != IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE + tile_count) {
        return -6;
    }
    if (assembler->map_valid && header->frame_seq < assembler->map_frame_seq) {
        assembler->drops_stale += 1u;
        return 1; /* keep the newer map (R2.5: last-known-good) */
    }

    {
        uint32_t i;
        const uint8_t* weights = payload + IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE;
        for (i = 0; i < tile_count; ++i) {
            assembler->map_weights[i] = weights[i] == 0u ? 1u : weights[i];
        }
    }
    assembler->map_cols = map_header.cols;
    assembler->map_rows = map_header.rows;
    assembler->map_tile_size = map_header.tile_size;
    assembler->map_tile_count = tile_count;
    assembler->map_frame_seq = header->frame_seq;
    assembler->map_valid = 1;
    assembler->map_updates += 1u;
    return 1;
}

int imt_assembler_push_datagram(ImtAssembler* assembler,
                                const uint8_t* datagram, size_t size)
{
    ImtWireHeader header;
    const uint8_t* payload;
    int rc;

    if (!assembler || !assembler->staging || !datagram) {
        return -1;
    }

    rc = imt_wire_decode_header(datagram, size, &header);
    if (rc != 0) {
        /* R6.5: silently discard malformed/foreign headers, but account for them. */
        assembler->drops_bad_header += 1u;
        return 1;
    }
    payload = datagram + IMT_WIRE_HEADER_SIZE;

    if (header.type == IMT_PACKET_TYPE_MAP) {
        return imt_assembler_handle_map(assembler, &header, payload);
    }
    if (header.type != IMT_PACKET_TYPE_SLICE && header.type != IMT_PACKET_TYPE_PARITY) {
        return 1; /* FEEDBACK and unknown types are not for the assembler */
    }

    /* Latest-wins (R7.3): older frames are dropped outright. */
    if (assembler->latest.size != 0u && header.frame_seq <= assembler->latest.frame_seq) {
        assembler->drops_stale += 1u;
        return 1;
    }
    if (assembler->active_valid && header.frame_seq < assembler->active_frame_seq) {
        assembler->drops_stale += 1u;
        return 1;
    }
    if (!assembler->active_valid || header.frame_seq != assembler->active_frame_seq) {
        if (assembler->active_valid) {
            assembler->frames_incomplete += 1u; /* superseded while incomplete */
        }
        imt_assembler_reset_active(assembler);
        if (imt_assembler_start_frame(assembler, &header) != 0) {
            return -3;
        }
    }

    if (assembler->active_chunk_count != header.chunk_count ||
        assembler->staging_size != header.frame_size) {
        imt_assembler_reset_active(assembler);
        return -4;
    }

    if (header.type == IMT_PACKET_TYPE_PARITY) {
        ImtAssemblerGroup* group = NULL;

        /* For parity packets chunk_index carries the group's base index. */
        if (header.fec_index != IMT_FEC_INDEX_PARITY ||
            imt_assembler_bind_group(assembler, &header, header.chunk_index, &group) != 0) {
            imt_assembler_reset_active(assembler);
            return -5;
        }
        if (header.payload_size !=
                imt_assembler_chunk_size(assembler, group->base_index)) {
            imt_assembler_reset_active(assembler);
            return -5;
        }

        memcpy(group->parity, payload, header.payload_size);
        group->parity_payload_size = header.payload_size;
        group->parity_received = 1;
        (void)imt_assembler_try_recover_group(assembler, group);
    } else {
        ImtAssemblerGroup* group = NULL;
        size_t offset;

        if (header.chunk_index >= assembler->active_chunk_count ||
            header.fec_index == IMT_FEC_INDEX_PARITY ||
            header.fec_index >= header.fec_group_size ||
            header.fec_index > header.chunk_index ||
            header.payload_size !=
                imt_assembler_chunk_size(assembler, header.chunk_index)) {
            imt_assembler_reset_active(assembler);
            return -5;
        }
        if (imt_assembler_bind_group(assembler, &header,
                                     (uint32_t)header.chunk_index - header.fec_index,
                                     &group) != 0) {
            imt_assembler_reset_active(assembler);
            return -5;
        }

        offset = (size_t)header.chunk_index * assembler->max_payload;
        if (!assembler->received_chunks[header.chunk_index]) {
            memcpy(assembler->staging + offset, payload, header.payload_size);
            assembler->received_chunks[header.chunk_index] = 1u;
            assembler->active_chunks_received += 1u;
        }
        (void)imt_assembler_try_recover_group(assembler, group);
    }

    if (assembler->active_chunks_received == assembler->active_chunk_count) {
        return imt_assembler_publish(assembler);
    }
    return 1;
}

int imt_assembler_get_map(const ImtAssembler* assembler, ImtMapView* out_view)
{
    if (!assembler || !assembler->map_weights || !out_view) {
        return -1;
    }

    out_view->weights = assembler->map_weights;
    if (assembler->map_valid) {
        out_view->tile_count = assembler->map_tile_count;
        out_view->cols = assembler->map_cols;
        out_view->rows = assembler->map_rows;
        out_view->tile_size = assembler->map_tile_size;
        out_view->is_fallback = 0;
        return 0;
    }

    /* R2.5: uniform-128 fallback before any MAP datagram has arrived. */
    out_view->tile_count = assembler->map_capacity;
    out_view->cols = 0;
    out_view->rows = 0;
    out_view->tile_size = 0;
    out_view->is_fallback = 1;
    return 1;
}

int imt_assembler_copy_latest(ImtAssembler* assembler, uint8_t* dst,
                              size_t dst_capacity, ImtFrameView* out_frame)
{
    if (!assembler || !dst || !out_frame || assembler->latest.size == 0u) {
        return -1;
    }
    if (assembler->latest.size > dst_capacity) {
        return -2;
    }

    memcpy(dst, assembler->latest.data, assembler->latest.size);
    *out_frame = assembler->latest;
    out_frame->data = dst;
    out_frame->capacity = dst_capacity;
    return 0;
}
