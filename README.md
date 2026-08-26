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

To deploy without overwriting an existing dirty Pi checkout, use the isolated
SCP helper from Windows. It builds the sender but does not start it:

```powershell
pwsh tools/deploy_stereo_dual.ps1
```

Then start the calibrated 60 fps stream on the Pi. Video goes to the direct
Ethernet PC address `192.168.50.1`, left eye on UDP 5004 and right on 5005:

```bash
cd /home/admin/MetaPuppet-dual
./tools/run_stereo_dual.sh 192.168.50.1 5004 1280 720 60 0 1
```

For exact calibration exposure at 30 fps:

```bash
MPS_USE_CALIBRATION_EXPOSURE=1 \
  ./tools/run_stereo_dual.sh 192.168.50.1 5004 1280 720 30 0 1
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

For stable colour and geometrically valid rectification, use the same manual
camera controls that were recorded in the calibration JSON. Both the direct
libcamera backend and the `rpicam-vid` fallback accept these variables. The
current `s1` calibration (left camera 0, right camera 1) is launched as:

```bash
MPS_CAMERA_AWB_GAINS=1.769247,2.343446 \
MPS_CAMERA_SHUTTER_US=26974 MPS_CAMERA_ANALOG_GAIN=1.499268 \
MPS_CAMERA_LEFT_LENS_POSITION=0.792 MPS_CAMERA_RIGHT_LENS_POSITION=0.787 \
  ./build/pi_stream_sender/mps_stereo_x264_sender \
  192.168.137.1 5004 1280 720 30 0 1
```

For a 60 fps colour/geometry check, keep the fixed AWB and two lens-position
variables but omit `MPS_CAMERA_SHUTTER_US` and `MPS_CAMERA_ANALOG_GAIN`; AE
then remains enabled and can satisfy the shorter frame interval.

Use the values from a newer `rectify_<session>.json` after recalibration.
Shutter and analogue gain must be supplied together; otherwise AE remains
enabled. A shutter of 26974 microseconds cannot sustain 60 fps, so this exact
calibration example uses 30 fps. `MPS_CAMERA_AWB_GAINS` disables AWB, while
the per-eye lens positions put autofocus into manual mode. If none of these
variables is set, the previous automatic camera behaviour is preserved.

`MPS_ADAPTIVE=0` disables adaptive CRF. Dynamic resolution is deliberately not
performed because it would also require coordinated decoder and Unity texture
renegotiation; when encoding is late, latest-frame capture naturally drops
stale pairs instead of building latency.

`MPS_KERNEL_PACING_MBPS` is opt-in because Linux socket pacing only takes
effect with a pacing-aware queueing discipline such as `fq`. The sender logs
the effective socket rate; it does not change the host qdisc.
`MPS_CAMERA_SYNC=0` disables software camera synchronization for diagnostics
only.

The dual-eye receiver sends one 40-byte `KEYFRAME_NACK` when an incomplete
keyframe is superseded. The sender retains only its latest keyframe and retries
that exact sequence once, using FEC group size 2. Retransmission is handled by
the existing eye worker, so the packetizer and `sendmmsg()` scratch state never
have concurrent owners. While recovery is pending, the PC drops dependent
interframes and resumes on the retry or the next natural keyframe; otherwise a
same-sequence retry would be rejected by latest-wins ordering. Set
`MPS_KEYFRAME_NACK=0` and disable Unity's `Enable Keyframe Retry` together for
an A/B comparison.
The PC diagnostics window exposes per-eye `nack` and `nackFail` counters.

To verify the Pi response independently of the decoder, run the probe on the
destination PC, then start the Pi sender against that PC and port pair:

```bash
python tools/stereo_keyframe_nack_smoke.py --port 55004
./build/pi_stream_sender/mps_stereo_x264_sender <pc-ip> 55004 1280 720 60 0 1
```

## Repository layout

Use this repository for Pi-side camera and transport work.

Use `MetaPuppetVR` for:

- Unity scenes
- PCVR receive path
- shader/display integration
- headset-side presentation
