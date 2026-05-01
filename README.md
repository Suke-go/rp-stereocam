# rp-stereocam

Raspberry Pi stereo camera capture, encoding, and Ethernet streaming stack for the MetaPuppet project.

## Scope

This repository owns the Pi-side code:

- `native/stream_sender`
  - `libcamera` stereo capture
  - SBS composition
  - low-latency H.264 encode path
  - UDP packetization and transmission
- `tools/pi`
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

## Repository layout

Use this repository for Pi-side camera and transport work.

Use `MetaPuppetVR` for:

- Unity scenes
- PCVR receive path
- shader/display integration
- headset-side presentation
