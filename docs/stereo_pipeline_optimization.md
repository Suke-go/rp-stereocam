# Stereo pipeline optimization notes

This review covers the dual-eye path from Raspberry Pi capture through x264
and IMT/UDP to the Windows D3D11/Unity receiver. Connectivity, firewall, VPN,
and host queueing-discipline changes are intentionally outside its scope.

## Implemented decisions

- x264 uses the `ultrafast,zerolatency` preset, constrained-baseline profile,
  no B-frames, no lookahead, one encoder thread per eye, AQ disabled, and a
  periodic GOP. Note that upstream `ultrafast` already disables CABAC; the
  profile change therefore constrains compatibility but is not counted as a
  new CPU saving. Explicit AQ disable does remove the sender's former override
  of the preset. See the upstream [x264 preset definitions](https://github.com/mirror/x264/blob/master/x264.c).
- Both eyes encode concurrently. Capture keeps only fresh frames, so overload
  drops stale capture work instead of adding queue latency.
- The adaptive controller measures the slower eye's local encode-and-send work
  against the frame budget and raises CRF gradually. It never sleeps after an
  already camera-paced frame. Resolution changes are excluded because the
  decoder media type and four Unity plane textures would have to be
  renegotiated atomically.
- IMT datagrams are sent in `sendmmsg()` batches. Linux documents this API as a
  way to send multiple messages with one system call: [sendmmsg(2)](https://man7.org/linux/man-pages/man2/sendmmsg.2.html).
- Direct libcamera capture is the default through
  `MPS_CAPTURE_BACKEND=libcamera`. It applies fixed frame-duration and
  Raspberry Pi server/client `SyncMode` controls, pairs on sensor timestamps,
  and hands pooled contiguous I420 buffers to the encoder workers. The
  `rpicam` backend remains available as a diagnostic fallback.
  The controls mirror the official
  [rpicam-apps implementation](https://github.com/raspberrypi/rpicam-apps/blob/main/core/rpicam_app.cpp).
- The PC decoder can use Media Foundation D3D11 NV12 output surfaces. At the
  Unity render event, a same-sequence stereo pair is copied GPU-to-GPU into two
  stable NV12 resources whose R8 luma and R8G8 chroma SRVs are wrapped by
  `Texture2D.CreateExternalTexture`. Failure at any prerequisite falls back to
  the existing CPU plane upload. Microsoft documents the NV12 plane view
  formats in [DXGI_FORMAT](https://learn.microsoft.com/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format),
  and Unity accepts an `ID3D11ShaderResourceView*` for
  [CreateExternalTexture](https://docs.unity3d.com/ScriptReference/Texture2D.CreateExternalTexture.html).
- IMT wire version 1 now defines a header-only `KEYFRAME_NACK` control packet.
  When a new frame supersedes an incomplete keyframe, the PC sends one NACK to
  the source address observed by `recvfrom()`. Each Pi eye socket has a receive
  thread that only validates and publishes the requested sequence; the normal
  eye worker performs the retry between encode jobs. Only the latest keyframe
  is retained, it can be retried once, and its retry uses normalized chunk
  weight 255 throughout (FEC group size 2). Stale, duplicate, oversized, and
  malformed requests are ignored. The PC assembler gates dependent
  interframes until the requested sequence or a newer natural keyframe arrives;
  this is required because plain latest-wins ordering would reject the older
  retry. A NACK accidentally delivered to an IMT assembler is explicitly
  ignored without changing active video state.

## Deliberately not implemented as a local patch

Sending a slice before the access unit finishes requires a wire-protocol
revision. Every current IMT slice datagram includes final `frame_size`,
`chunk_count`, and FEC group membership; the assembler publishes only after
those declared chunks are complete. x264 can expose NALs incrementally, but the
sender cannot know the final access-unit size and FEC layout at the first NAL.
A correct version needs an incremental-frame protocol (start/data/end or one
independently decodable frame unit per slice), receiver support, loss/reorder
tests, and a compatibility version gate. Reusing the current header with
placeholder sizes would corrupt assembly rather than reduce latency.

Kernel pacing is implemented as an explicit, logged
`MPS_KERNEL_PACING_MBPS` socket option but remains off by default. Linux `fq`
uses `sk_pacing_rate`; other qdiscs do not necessarily honor it. The
application does not mutate the host qdisc. See the Linux
[FQ scheduler configuration](https://github.com/torvalds/linux/blob/master/net/sched/Kconfig).

## Runtime acceptance checks

For a target Pi/relay/Quest run, record at least:

- per-eye capture pairs, capture drops, maximum timestamp skew, encode ms,
  send ms, output FPS, and Mbps;
- receiver packets dropped, incomplete/recovered frames, decode failures,
  overwritten frames, NACK sent/failure counts, pair mismatches,
  hardware-decoder and GPU-direct flags;
- receive-to-decode and receive-to-upload percentiles over several minutes;
- the same scene and camera motion for `rpicam` versus direct `libcamera`, and
  CPU upload versus GPU-direct, changing one variable at a time.

Do not subtract the Pi monotonic capture timestamp from the PC monotonic clock;
those clocks have unrelated epochs. End-to-end capture latency requires a
clock-synchronization scheme or an external measurement.

## Target-device smoke result

An isolated build was run for eight seconds on the target Pi with two IMX708
Wide cameras, direct libcamera capture, 1280x720 per eye, 60 fps, CRF 20, and
software sync. After synchronization, both eyes held about 60.0 fps; encode
time was about 7.4-7.6 ms per eye and batched send time about 0.04-0.06 ms.
The observed one-second maximum timestamp skew was typically about 1.6 ms.
Three frames were dropped during startup/synchronization. This is a smoke
result rather than a long-duration thermal or end-to-end Quest benchmark.

The reverse path has two automated smoke layers. IMT unit tests cover the new
wire type, incomplete-keyframe latch, and assembler control-packet isolation.
`MetaPuppetVR/tools/stereo_receiver_smoke.py --nack-smoke` injects an
unrecoverable keyframe on both loopback ports and validates exactly one NACK
per eye. `tools/stereo_keyframe_nack_smoke.py` is the real-UDP Pi response
probe; it verifies the repeated sequence uses FEC group size 2 and that a
duplicate NACK does not cause a second retransmission.
