# rp-stereocam

Raspberry Pi stereo camera capture, encoding, and Ethernet streaming stack for the MetaPuppet project.

## Scope

This repository owns the Pi-side code:

- `native/edge_camera`
  - foreground / mask primitives (chroma key, NV12 in-place mask apply,
    morph grow, frame ring)
  - clock sync + stereo sync helpers
  - stats / security / preprocess scaffolding
  - currently building blocks; not yet wired into `stream_sender` (see
    upstream `MetaPuppet` issue tracker for the integration plan)
- `native/stream_sender`
  - `libcamera` stereo capture
  - SBS composition (I420 and NV12)
  - low-latency H.264 encode path (v4l2h264enc / x264enc fallback)
  - UDP packetization and transmission
  - `mps_pack_bench` microbenchmark for I420 vs NV12 SBS pack
- `tools`
  - Pi bootstrap and build scripts
  - runtime checks
  - direct Ethernet helper scripts
  - streaming launch scripts

Unity-side rendering, native PCVR receive, and headset presentation stay in the separate `MetaPuppetVR` repository.

## Build

On Raspberry Pi:

```bash
./tools/pi/build_pi_streaming_stack.sh
```

## Typical run

Mono fallback:

```bash
./tools/pi/run_pcvr_camera_h264_stream.sh 192.168.137.1 5004 640 360 15 0 0 6000 8 software 1 120
```

Stereo:

```bash
./tools/pi/run_pcvr_camera_h264_stream.sh 192.168.137.1 5004 640 360 15 1 0 6000 8 software 1 120
```

Dual independent-eye x264 sender (left on `port`, right on `port + 1`):

```bash
cmake -S native/stream_sender -B build/pi_stream_sender -DMPS_ENABLE_X264=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/pi_stream_sender --target mps_stereo_x264_sender -j4
./build/pi_stream_sender/mps_stereo_x264_sender 192.168.137.1 5004 1280 720 60 0 1
```

The low-latency defaults use constrained-baseline H.264, AQ disabled, CRF 20,
a half-second GOP, six camera buffers, batched UDP sends, and no
application-level network pacing. A local processing-budget controller raises
CRF gradually (up to 30) if the slower eye cannot meet the frame budget.
Override them only when measurement shows a need:

```bash
MPS_X264_CRF=22 MPS_X264_KEYINT=30 MPS_CAMERA_BUFFERS=8 \
MPS_ADAPTIVE_TARGET_MS=17 MPS_ADAPTIVE_CRF_MAX=30 \
  ./build/pi_stream_sender/mps_stereo_x264_sender 192.168.137.1 5004 1280 720 60 0 1
```

The default `MPS_CAPTURE_BACKEND=libcamera` uses the direct in-process capture
path. It applies Raspberry Pi server/client `SyncMode` controls, pairs by
sensor timestamp, and transfers pooled I420 buffers to the two encoders
without a child process or pipe. Set `MPS_CAPTURE_BACKEND=rpicam` for the
diagnostic fallback. Use `MPS_CAMERA_MAX_SKEW_MS` (default 5) to set the direct
backend's pairing limit.

`MPS_ADAPTIVE=0` disables adaptive CRF. Dynamic resolution is deliberately not
performed because it would also require coordinated decoder and Unity texture
renegotiation; when encoding is late, latest-frame capture naturally drops
stale pairs instead of building latency.

`MPS_KERNEL_PACING_MBPS` is opt-in because Linux socket pacing only takes
effect with a pacing-aware queueing discipline such as `fq`. The sender logs
the effective socket rate; it does not change the host qdisc.
`MPS_CAMERA_SYNC=0` disables software camera synchronization for diagnostics
only.

## Repository layout

Use this repository for Pi-side camera and transport work.

Use `MetaPuppetVR` for:

- Unity scenes
- PCVR receive path
- shader/display integration
- headset-side presentation
