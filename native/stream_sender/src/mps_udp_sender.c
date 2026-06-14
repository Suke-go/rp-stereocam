#include "mps_udp_sender.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET mps_socket_t;
#define mps_close_socket closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int mps_socket_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define mps_close_socket close
#endif

#include <string.h>

typedef struct MpsUdpSenderPrivate {
    struct sockaddr_in addr;
} MpsUdpSenderPrivate;

static MpsUdpSenderPrivate g_sender_private;

static void mps_udp_sender_apply_low_latency_options(mps_socket_t fd)
{
    int send_buffer_bytes = 256 * 1024;
    int tos_low_delay = 0x10;

    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&send_buffer_bytes, (int)sizeof(send_buffer_bytes));
    setsockopt(fd, IPPROTO_IP, IP_TOS, (const char*)&tos_low_delay, (int)sizeof(tos_low_delay));
}

int mps_udp_sender_open(MpsUdpSender* sender, const char* host, uint16_t port, uint32_t stream_id)
{
    mps_socket_t fd;

    if (!sender || !host || port == 0u) {
        return -1;
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return -2;
    }
#endif

    memset(sender, 0, sizeof(*sender));
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET) {
        return -3;
    }
    mps_udp_sender_apply_low_latency_options(fd);

    memset(&g_sender_private, 0, sizeof(g_sender_private));
    g_sender_private.addr.sin_family = AF_INET;
    g_sender_private.addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &g_sender_private.addr.sin_addr) != 1) {
        mps_close_socket(fd);
        return -4;
    }

    sender->socket_fd = (int)fd;
    mps_packet_writer_init(&sender->writer, stream_id);
    return 0;
}

void mps_udp_sender_close(MpsUdpSender* sender)
{
    if (!sender || sender->socket_fd == 0) {
        return;
    }
    mps_close_socket((mps_socket_t)sender->socket_fd);
    sender->socket_fd = 0;
#ifdef _WIN32
    WSACleanup();
#endif
}

int mps_udp_sender_send_frame(MpsUdpSender* sender,
                              const uint8_t* frame_data,
                              size_t frame_size,
                              uint64_t frame_sequence,
                              uint64_t capture_timestamp_ns,
                              uint32_t flags)
{
    uint32_t chunk_count;
    uint32_t chunk_index;

    if (!sender || sender->socket_fd == 0 || !frame_data || frame_size == 0u) {
        return -1;
    }

    chunk_count = (uint32_t)((frame_size + MPS_PACKET_MAX_PAYLOAD - 1u) / MPS_PACKET_MAX_PAYLOAD);
    if (chunk_count == 0u || chunk_count > MPS_PACKET_MAX_CHUNKS || frame_size > 0xffffffffu) {
        return -3;
    }
    for (chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        uint8_t datagram[MPS_PACKET_MAX_DATAGRAM];
        size_t out_size = 0;
        const size_t payload_offset = (size_t)chunk_index * MPS_PACKET_MAX_PAYLOAD;
        size_t payload_size = frame_size - payload_offset;
        int sent;
        int rc;

        if (payload_size > MPS_PACKET_MAX_PAYLOAD) {
            payload_size = MPS_PACKET_MAX_PAYLOAD;
        }

        rc = mps_packet_writer_make(&sender->writer,
                                    frame_sequence,
                                    capture_timestamp_ns,
                                    flags,
                                    chunk_index,
                                    chunk_count,
                                    frame_data,
                                    frame_size,
                                    payload_offset,
                                    payload_size,
                                    datagram,
                                    sizeof(datagram),
                                    &out_size);
        if (rc != 0) {
            return rc;
        }

        sent = (int)sendto((mps_socket_t)sender->socket_fd,
                           (const char*)datagram,
                           (int)out_size,
                           0,
                           (const struct sockaddr*)&g_sender_private.addr,
                           sizeof(g_sender_private.addr));
        if (sent == SOCKET_ERROR || (size_t)sent != out_size) {
            return -2;
        }

        mps_packet_writer_advance(&sender->writer);
        sender->packets_sent += 1u;
    }

    sender->frames_sent += 1u;
    return 0;
}

int mps_udp_sender_send_frame_xor_fec(MpsUdpSender* sender,
                                      const uint8_t* frame_data,
                                      size_t frame_size,
                                      uint64_t frame_sequence,
                                      uint64_t capture_timestamp_ns,
                                      uint32_t flags)
{
    uint32_t data_chunk_count;
    uint32_t packet_count;
    uint32_t chunk_index;
    uint8_t parity[MPS_PACKET_MAX_PAYLOAD];

    if (!sender || sender->socket_fd == 0 || !frame_data || frame_size == 0u) {
        return -1;
    }

    data_chunk_count = (uint32_t)((frame_size + MPS_PACKET_MAX_PAYLOAD - 1u) / MPS_PACKET_MAX_PAYLOAD);
    if (data_chunk_count == 0u || data_chunk_count + 1u > MPS_PACKET_MAX_CHUNKS || frame_size > 0xffffffffu) {
        return -3;
    }

    packet_count = data_chunk_count + 1u;
    memset(parity, 0, sizeof(parity));
    for (chunk_index = 0; chunk_index < data_chunk_count; ++chunk_index) {
        const size_t payload_offset = (size_t)chunk_index * MPS_PACKET_MAX_PAYLOAD;
        size_t payload_size = frame_size - payload_offset;
        size_t i;

        if (payload_size > MPS_PACKET_MAX_PAYLOAD) {
            payload_size = MPS_PACKET_MAX_PAYLOAD;
        }
        for (i = 0; i < payload_size; ++i) {
            parity[i] ^= frame_data[payload_offset + i];
        }
    }

    for (chunk_index = 0; chunk_index < data_chunk_count; ++chunk_index) {
        uint8_t datagram[MPS_PACKET_MAX_DATAGRAM];
        size_t out_size = 0;
        const size_t payload_offset = (size_t)chunk_index * MPS_PACKET_MAX_PAYLOAD;
        size_t payload_size = frame_size - payload_offset;
        int sent;
        int rc;

        if (payload_size > MPS_PACKET_MAX_PAYLOAD) {
            payload_size = MPS_PACKET_MAX_PAYLOAD;
        }

        rc = mps_packet_writer_make(&sender->writer,
                                    frame_sequence,
                                    capture_timestamp_ns,
                                    flags | MPS_PACKET_FLAG_FEC_PRESENT,
                                    chunk_index,
                                    packet_count,
                                    frame_data,
                                    frame_size,
                                    payload_offset,
                                    payload_size,
                                    datagram,
                                    sizeof(datagram),
                                    &out_size);
        if (rc != 0) {
            return rc;
        }

        sent = (int)sendto((mps_socket_t)sender->socket_fd,
                           (const char*)datagram,
                           (int)out_size,
                           0,
                           (const struct sockaddr*)&g_sender_private.addr,
                           sizeof(g_sender_private.addr));
        if (sent == SOCKET_ERROR || (size_t)sent != out_size) {
            return -2;
        }

        mps_packet_writer_advance(&sender->writer);
        sender->packets_sent += 1u;
    }

    {
        uint8_t datagram[MPS_PACKET_MAX_DATAGRAM];
        size_t out_size = 0;
        int sent;
        const int rc = mps_packet_writer_make_payload(&sender->writer,
                                                      frame_sequence,
                                                      capture_timestamp_ns,
                                                      flags | MPS_PACKET_FLAG_FEC_PRESENT | MPS_PACKET_FLAG_FEC_XOR_PARITY,
                                                      data_chunk_count,
                                                      packet_count,
                                                      parity,
                                                      sizeof(parity),
                                                      frame_size,
                                                      datagram,
                                                      sizeof(datagram),
                                                      &out_size);
        if (rc != 0) {
            return rc;
        }

        sent = (int)sendto((mps_socket_t)sender->socket_fd,
                           (const char*)datagram,
                           (int)out_size,
                           0,
                           (const struct sockaddr*)&g_sender_private.addr,
                           sizeof(g_sender_private.addr));
        if (sent == SOCKET_ERROR || (size_t)sent != out_size) {
            return -2;
        }
        mps_packet_writer_advance(&sender->writer);
        sender->packets_sent += 1u;
    }

    sender->frames_sent += 1u;
    return 0;
}
