#include "mps_packet_writer.h"

#include <string.h>

static void mps_write_u16_le(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void mps_write_u32_le(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
    p[2] = (uint8_t)((value >> 16) & 0xffu);
    p[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void mps_write_u64_le(uint8_t* p, uint64_t value)
{
    mps_write_u32_le(p, (uint32_t)(value & 0xffffffffull));
    mps_write_u32_le(p + 4, (uint32_t)((value >> 32) & 0xffffffffull));
}

void mps_packet_writer_init(MpsPacketWriter* writer, uint32_t stream_id)
{
    if (!writer) {
        return;
    }
    memset(writer, 0, sizeof(*writer));
    writer->stream_id = stream_id;
}

int mps_packet_writer_make(const MpsPacketWriter* writer,
                           uint64_t frame_sequence,
                           uint64_t capture_timestamp_ns,
                           uint32_t flags,
                           uint32_t chunk_index,
                           uint32_t chunk_count,
                           const uint8_t* frame_data,
                           size_t frame_size,
                           size_t payload_offset,
                           size_t payload_size,
                           uint8_t* out_datagram,
                           size_t out_capacity,
                           size_t* out_size)
{
    if (!writer || !frame_data || !out_datagram || !out_size) {
        return -1;
    }
    if (chunk_count == 0u ||
        chunk_count > MPS_PACKET_MAX_CHUNKS ||
        chunk_index >= chunk_count ||
        payload_size > MPS_PACKET_MAX_PAYLOAD ||
        frame_size > 0xffffffffu) {
        return -2;
    }
    if (payload_offset + payload_size > frame_size || out_capacity < MPS_PACKET_HEADER_SIZE + payload_size) {
        return -3;
    }

    memset(out_datagram, 0, MPS_PACKET_HEADER_SIZE);
    mps_write_u32_le(out_datagram + 0, MPS_PACKET_MAGIC);
    mps_write_u16_le(out_datagram + 4, MPS_PACKET_VERSION);
    mps_write_u16_le(out_datagram + 6, MPS_PACKET_HEADER_SIZE);
    mps_write_u32_le(out_datagram + 8, flags | (chunk_index + 1u == chunk_count ? MPS_PACKET_FLAG_END_OF_FRAME : 0u));
    mps_write_u32_le(out_datagram + 12, writer->stream_id);
    mps_write_u64_le(out_datagram + 16, writer->packet_sequence);
    mps_write_u64_le(out_datagram + 24, frame_sequence);
    mps_write_u64_le(out_datagram + 32, capture_timestamp_ns);
    mps_write_u32_le(out_datagram + 40, chunk_index);
    mps_write_u32_le(out_datagram + 44, chunk_count);
    mps_write_u32_le(out_datagram + 48, (uint32_t)payload_size);
    mps_write_u32_le(out_datagram + 52, (uint32_t)frame_size);
    memcpy(out_datagram + MPS_PACKET_HEADER_SIZE, frame_data + payload_offset, payload_size);
    *out_size = MPS_PACKET_HEADER_SIZE + payload_size;
    return 0;
}

void mps_packet_writer_advance(MpsPacketWriter* writer)
{
    if (!writer) {
        return;
    }
    writer->packet_sequence += 1u;
}
