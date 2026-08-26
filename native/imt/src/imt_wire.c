#include "imt.h"

#include <string.h>

static void imt_write_u16_le(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void imt_write_u32_le(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
    p[2] = (uint8_t)((value >> 16) & 0xFFu);
    p[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void imt_write_u64_le(uint8_t* p, uint64_t value)
{
    imt_write_u32_le(p, (uint32_t)(value & 0xFFFFFFFFu));
    imt_write_u32_le(p + 4, (uint32_t)(value >> 32));
}

static uint16_t imt_read_u16_le(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t imt_read_u32_le(const uint8_t* p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t imt_read_u64_le(const uint8_t* p)
{
    return (uint64_t)imt_read_u32_le(p) | ((uint64_t)imt_read_u32_le(p + 4) << 32);
}

static int imt_wire_packet_type_is_known(uint8_t type)
{
    switch (type) {
        case IMT_PACKET_TYPE_SLICE:
        case IMT_PACKET_TYPE_MAP:
        case IMT_PACKET_TYPE_PARITY:
        case IMT_PACKET_TYPE_FEEDBACK:
        case IMT_PACKET_TYPE_KEYFRAME_NACK:
            return 1;
        default:
            return 0;
    }
}

/* R6.1 byte layout (all little-endian):
 * 0:2 magic, 2:1 version, 3:1 type, 4:2 header_size, 6:2 flags,
 * 8:8 frame_seq, 16:8 capture_timestamp_ns, 24:4 frame_size,
 * 28:2 chunk_index, 30:2 chunk_count, 32:2 payload_size,
 * 34:2 fec_group, 36:1 fec_group_size, 37:1 fec_index, 38:2 reserved. */
int imt_wire_encode_header(const ImtWireHeader* header, uint8_t* dst, size_t dst_size)
{
    if (!header || !dst || dst_size < IMT_WIRE_HEADER_SIZE) {
        return -1;
    }

    dst[0] = IMT_WIRE_MAGIC0;
    dst[1] = IMT_WIRE_MAGIC1;
    dst[2] = IMT_WIRE_VERSION;
    dst[3] = header->type;
    imt_write_u16_le(dst + 4, IMT_WIRE_HEADER_SIZE);
    imt_write_u16_le(dst + 6, header->flags);
    imt_write_u64_le(dst + 8, header->frame_seq);
    imt_write_u64_le(dst + 16, header->capture_timestamp_ns);
    imt_write_u32_le(dst + 24, header->frame_size);
    imt_write_u16_le(dst + 28, header->chunk_index);
    imt_write_u16_le(dst + 30, header->chunk_count);
    imt_write_u16_le(dst + 32, header->payload_size);
    imt_write_u16_le(dst + 34, header->fec_group);
    dst[36] = header->fec_group_size;
    dst[37] = header->fec_index;
    imt_write_u16_le(dst + 38, 0); /* reserved */
    return 0;
}

int imt_wire_decode_header(const uint8_t* data, size_t size, ImtWireHeader* out_header)
{
    ImtWireHeader h;

    if (!data || !out_header || size < IMT_WIRE_HEADER_SIZE) {
        return -1;
    }

    /* R6.5: foreign magic / version / header_size are rejected. */
    if (data[0] != IMT_WIRE_MAGIC0 ||
        data[1] != IMT_WIRE_MAGIC1 ||
        data[2] != IMT_WIRE_VERSION ||
        imt_read_u16_le(data + 4) != IMT_WIRE_HEADER_SIZE) {
        return -2;
    }

    memset(&h, 0, sizeof(h));
    h.type = data[3];
    if (!imt_wire_packet_type_is_known(h.type)) {
        return -4;
    }
    h.flags = imt_read_u16_le(data + 6);
    h.frame_seq = imt_read_u64_le(data + 8);
    h.capture_timestamp_ns = imt_read_u64_le(data + 16);
    h.frame_size = imt_read_u32_le(data + 24);
    h.chunk_index = imt_read_u16_le(data + 28);
    h.chunk_count = imt_read_u16_le(data + 30);
    h.payload_size = imt_read_u16_le(data + 32);
    h.fec_group = imt_read_u16_le(data + 34);
    h.fec_group_size = data[36];
    h.fec_index = data[37];

    if ((size_t)IMT_WIRE_HEADER_SIZE + h.payload_size > size) {
        return -3;
    }

    *out_header = h;
    return 0;
}

/* R6.4: latest_frame_seq(u64) latest_capture_timestamp_ns(u64)
 *       frame_age_ns(u64) frames_completed(u32) frames_incomplete(u32). */
int imt_wire_encode_feedback(const ImtFeedback* feedback, uint8_t* dst, size_t dst_size)
{
    if (!feedback || !dst || dst_size < IMT_WIRE_FEEDBACK_PAYLOAD_SIZE) {
        return -1;
    }
    imt_write_u64_le(dst + 0, feedback->latest_frame_seq);
    imt_write_u64_le(dst + 8, feedback->latest_capture_timestamp_ns);
    imt_write_u64_le(dst + 16, feedback->frame_age_ns);
    imt_write_u32_le(dst + 24, feedback->frames_completed);
    imt_write_u32_le(dst + 28, feedback->frames_incomplete);
    return 0;
}

int imt_wire_decode_feedback(const uint8_t* data, size_t size, ImtFeedback* out_feedback)
{
    if (!data || !out_feedback || size < IMT_WIRE_FEEDBACK_PAYLOAD_SIZE) {
        return -1;
    }
    out_feedback->latest_frame_seq = imt_read_u64_le(data + 0);
    out_feedback->latest_capture_timestamp_ns = imt_read_u64_le(data + 8);
    out_feedback->frame_age_ns = imt_read_u64_le(data + 16);
    out_feedback->frames_completed = imt_read_u32_le(data + 24);
    out_feedback->frames_incomplete = imt_read_u32_le(data + 28);
    return 0;
}

/* R6.3: cols(u16) rows(u16) tile_size(u16) reserved(u16). */
int imt_wire_encode_map_payload_header(const ImtMapPayloadHeader* header,
                                       uint8_t* dst, size_t dst_size)
{
    if (!header || !dst || dst_size < IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE) {
        return -1;
    }
    imt_write_u16_le(dst + 0, header->cols);
    imt_write_u16_le(dst + 2, header->rows);
    imt_write_u16_le(dst + 4, header->tile_size);
    imt_write_u16_le(dst + 6, 0); /* reserved */
    return 0;
}

int imt_wire_decode_map_payload_header(const uint8_t* data, size_t size,
                                       ImtMapPayloadHeader* out_header)
{
    if (!data || !out_header || size < IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE) {
        return -1;
    }
    out_header->cols = imt_read_u16_le(data + 0);
    out_header->rows = imt_read_u16_le(data + 2);
    out_header->tile_size = imt_read_u16_le(data + 4);
    return 0;
}
