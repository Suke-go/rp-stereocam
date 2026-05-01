#ifndef MPS_PACKET_WRITER_H
#define MPS_PACKET_WRITER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MPS_PACKET_MAGIC 0x3151504du
#define MPS_PACKET_VERSION 1u
#define MPS_PACKET_HEADER_SIZE 56u
#define MPS_PACKET_MAX_DATAGRAM 1500u
#define MPS_PACKET_MAX_PAYLOAD (MPS_PACKET_MAX_DATAGRAM - MPS_PACKET_HEADER_SIZE)
#define MPS_PACKET_MAX_CHUNKS 4096u

typedef enum MpsPacketFlags {
    MPS_PACKET_FLAG_KEYFRAME = 1u << 0,
    MPS_PACKET_FLAG_CODEC_CONFIG = 1u << 1,
    MPS_PACKET_FLAG_END_OF_FRAME = 1u << 2,
    MPS_PACKET_FLAG_RAW_RGBA = 1u << 16
} MpsPacketFlags;

typedef struct MpsPacketWriter {
    uint8_t datagram[MPS_PACKET_MAX_DATAGRAM];
    uint64_t packet_sequence;
    uint32_t stream_id;
} MpsPacketWriter;

void mps_packet_writer_init(MpsPacketWriter* writer, uint32_t stream_id);
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
                           size_t* out_size);
void mps_packet_writer_advance(MpsPacketWriter* writer);

#ifdef __cplusplus
}
#endif

#endif
