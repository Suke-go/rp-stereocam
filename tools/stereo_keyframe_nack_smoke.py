"""Validate Pi stereo sender KEYFRAME_NACK retransmission over real UDP.

Run this on the destination PC, then point mps_stereo_x264_sender at this
machine and the selected port pair. The probe requests the first keyframe
once per eye, verifies that the repeated frame uses FEC group size 2, sends a
duplicate request, and rejects a second retransmission.
"""

from __future__ import annotations

import argparse
import dataclasses
import selectors
import socket
import struct
import time


IMT_HEADER = struct.Struct("<2sBBHHQQIHHHHBBH")
IMT_HEADER_SIZE = 40
IMT_PACKET_TYPE_SLICE = 0
IMT_PACKET_TYPE_PARITY = 2
IMT_PACKET_TYPE_KEYFRAME_NACK = 4
IMT_PACKET_FLAG_KEYFRAME = 1


def _nack(frame_sequence: int) -> bytes:
    return IMT_HEADER.pack(
        b"IM", 1, IMT_PACKET_TYPE_KEYFRAME_NACK, IMT_HEADER_SIZE, 0,
        frame_sequence, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    )


@dataclasses.dataclass
class EyeProbe:
    label: str
    sock: socket.socket
    requested_sequence: int | None = None
    peer: tuple[str, int] | None = None
    expected_retry_packets: int = 0
    retry_packets: set[tuple[int, int, int, int]] = dataclasses.field(default_factory=set)
    retry_duplicates: int = 0
    duplicate_nack_sent_at: float | None = None

    @property
    def done(self) -> bool:
        return (
            self.duplicate_nack_sent_at is not None
            and time.monotonic() - self.duplicate_nack_sent_at >= 0.25
        )

    def accept(self, datagram: bytes, peer: tuple[str, int]) -> None:
        if len(datagram) < IMT_HEADER_SIZE:
            return
        fields = IMT_HEADER.unpack_from(datagram)
        (magic, version, packet_type, header_size, flags, frame_sequence,
         _timestamp_ns, _frame_size, chunk_index, chunk_count, payload_size,
         fec_group, fec_group_size, fec_index, _reserved) = fields
        if (
            magic != b"IM"
            or version != 1
            or header_size != IMT_HEADER_SIZE
            or IMT_HEADER_SIZE + payload_size > len(datagram)
        ):
            return

        if self.requested_sequence is None and flags & IMT_PACKET_FLAG_KEYFRAME:
            self.requested_sequence = frame_sequence
            self.peer = peer
            # Every complete two-chunk group contributes two slices plus one
            # parity. An odd final one-chunk group is deliberately excluded:
            # its fec_group_size is clipped to 1 and does not by itself prove
            # that the requested all-255/group-2 retry policy was selected.
            self.expected_retry_packets = (chunk_count // 2) * 3
            self.sock.sendto(_nack(frame_sequence), peer)

        if (
            frame_sequence != self.requested_sequence
            or fec_group_size != 2
            or packet_type not in (IMT_PACKET_TYPE_SLICE, IMT_PACKET_TYPE_PARITY)
        ):
            return

        packet_key = (packet_type, chunk_index, fec_group, fec_index)
        if packet_key in self.retry_packets:
            self.retry_duplicates += 1
        else:
            self.retry_packets.add(packet_key)

        if (
            len(self.retry_packets) >= self.expected_retry_packets
            and self.duplicate_nack_sent_at is None
            and self.peer is not None
        ):
            self.sock.sendto(_nack(self.requested_sequence), self.peer)
            self.duplicate_nack_sent_at = time.monotonic()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=55004)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    selector = selectors.DefaultSelector()
    probes: list[EyeProbe] = []
    try:
        for eye, label in enumerate(("left", "right")):
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
            sock.bind((args.bind, args.port + eye))
            sock.setblocking(False)
            probe = EyeProbe(label, sock)
            probes.append(probe)
            selector.register(sock, selectors.EVENT_READ, probe)

        deadline = time.monotonic() + max(0.5, args.timeout)
        while time.monotonic() < deadline and not all(probe.done for probe in probes):
            for key, _ in selector.select(timeout=0.05):
                probe: EyeProbe = key.data
                datagram, peer = probe.sock.recvfrom(65536)
                probe.accept(datagram, peer)

        result = 0
        for probe in probes:
            print(
                f"{probe.label}: seq={probe.requested_sequence} "
                f"retry={len(probe.retry_packets)}/{probe.expected_retry_packets} "
                f"duplicatePackets={probe.retry_duplicates} "
                f"duplicateNackTested={probe.duplicate_nack_sent_at is not None}"
            )
            if not probe.done or len(probe.retry_packets) < probe.expected_retry_packets:
                result = 2
            if probe.retry_duplicates:
                result = 3
        return result
    finally:
        selector.close()
        for probe in probes:
            probe.sock.close()


if __name__ == "__main__":
    raise SystemExit(main())
