#ifndef MPS_UDP_SENDER_H
#define MPS_UDP_SENDER_H

#include "mps_packet_writer.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MpsUdpSender {
    int socket_fd;
    MpsPacketWriter writer;
    uint64_t frames_sent;
    uint64_t packets_sent;
} MpsUdpSender;

int mps_udp_sender_open(MpsUdpSender* sender, const char* host, uint16_t port, uint32_t stream_id);
void mps_udp_sender_close(MpsUdpSender* sender);
int mps_udp_sender_send_frame(MpsUdpSender* sender,
                              const uint8_t* frame_data,
                              size_t frame_size,
                              uint64_t frame_sequence,
                              uint64_t capture_timestamp_ns,
                              uint32_t flags);
int mps_udp_sender_send_frame_xor_fec(MpsUdpSender* sender,
                                      const uint8_t* frame_data,
                                      size_t frame_size,
                                      uint64_t frame_sequence,
                                      uint64_t capture_timestamp_ns,
                                      uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif
