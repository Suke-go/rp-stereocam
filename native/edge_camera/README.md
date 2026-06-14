# MetaPuppet Edge Camera Core

This is the C core for the Raspberry Pi / PC stereo-camera path before hardware encoding.

The code is intentionally hardware-adapter-free. It does not open Camera Module 3 yet. Instead, it defines the processing layer that a future `libcamera` adapter will feed with left/right NV12 frames and `SensorTimestamp` metadata.

## Current Pipeline

```text
left NV12 + timestamp
right NV12 + timestamp
  -> optional clock-domain correction
  -> strict stereo timestamp validation
  -> sync-quality monitor
  -> greenback foreground extraction
  -> side-by-side NV12 packing
  -> latest-frame ring
  -> encoder adapter, not implemented yet
```

For two Camera Module 3 devices on one Raspberry Pi 5, the intended primary timing source is libcamera `SensorTimestamp`, not PTP. The PTP-like code is only for multi-device cases where a remote timestamp must be mapped into a local clock domain.

## File Map

`mp_frame.h`
: Basic frame description. The hot path currently assumes NV12.

`mp_edge_config.*`
: Runtime configuration defaults and validation.

`mp_clock_sync.*`
: Lightweight PTP-style four-timestamp offset/delay/jitter estimation. This is auxiliary, not the main sync method for two cameras on one Pi.

`mp_stereo_sync.*`
: Compares corrected left/right timestamps and decides whether to accept a stereo pair or drop the older side.

`mp_sync_monitor.*`
: Tracks whether accepted stereo pairs remain stable over time. It checks skew and frame-period error and reports `WARMING`, `LOCKED`, `DEGRADED`, or `UNLOCKED`.

`mp_mask.*`
: NV12 masking utilities. Includes full-frame matte fill and greenback background removal while keeping a rear-view human silhouette.

`mp_foreground.*`
: Reusable foreground extraction workspace. It builds a full-resolution background mask from NV12 chroma, expands the background mask to suppress green spill, then applies the matte without per-frame allocation. The 3x3 mask dilation has an ARM NEON path with scalar fallback.

`mp_sbs_packer.*`
: Packs left/right NV12 frames into one side-by-side NV12 frame for the encoder.

`mp_frame_ring.*`
: Small latest-frame ring. It uses C11 atomics and a seqlock-style version check. It prefers dropping stale data over growing latency.

`mp_preprocess.*`
: Orchestrates the pre-encode path: timestamp correction, stereo validation, sync monitor, foreground extraction, and SBS packing.

`mp_security.*`
: Simple privacy gate retained from earlier work. For current lab use it is not the controlling design concern.

`mp_stats.*`
: Lightweight counters for capture, masking, drop, encode, and transmit events.

`main.c`
: Self-test executable. It builds synthetic left/right frames, runs the pre-encode path, writes the SBS frame into the latest-frame ring, and prints status.

## Camera Module 3 Control Direction

The next real implementation should be `camera_adapter_libcamera_cpp`, not more timing simulation.

That adapter should:

- open both Camera Module 3 devices in one process.
- configure the same camera mode on both sides.
- request NV12/YUV420 output.
- set fixed `FrameDurationLimits`.
- warm up AE/AWB/AF, then lock exposure, gain, white balance, and focus.
- read `SensorTimestamp` from request metadata for every frame.
- feed accepted frames into `mp_preprocess_stereo_to_sbs_nv12`.
- drop bad pairs before encode.

If fast motion must be geometrically strict, Camera Module 3's rolling shutter is the limitation. Then the engineering answer is to move to Global Shutter cameras or external trigger-capable hardware, not to hide the problem with timestamp code.

## Build

```sh
cmake -S native/edge_camera -B build/edge_camera
cmake --build build/edge_camera
./build/edge_camera/mp_edge_camera
```

On Raspberry Pi 5, compile in release mode:

```sh
cmake -S native/edge_camera -B build/edge_camera -DCMAKE_BUILD_TYPE=Release
cmake --build build/edge_camera -j4
```

Without CMake, the current core can be checked directly:

```sh
mkdir -p build/edge_camera_manual
cc -std=c11 -Wall -Wextra -Wpedantic \
  -I native/edge_camera/include \
  native/edge_camera/src/main.c \
  native/edge_camera/src/mp_clock_sync.c \
  native/edge_camera/src/mp_edge_config.c \
  native/edge_camera/src/mp_foreground.c \
  native/edge_camera/src/mp_frame_ring.c \
  native/edge_camera/src/mp_mask.c \
  native/edge_camera/src/mp_preprocess.c \
  native/edge_camera/src/mp_sbs_packer.c \
  native/edge_camera/src/mp_security.c \
  native/edge_camera/src/mp_stereo_sync.c \
  native/edge_camera/src/mp_sync_monitor.c \
  native/edge_camera/src/mp_stats.c \
  -o build/edge_camera_manual/mp_edge_camera
./build/edge_camera_manual/mp_edge_camera
```

## Pre-Encode Pipeline

The intermediate path before HMD encoding is:

```text
left NV12 frame
right NV12 frame
  -> timestamp correction, optional
  -> timestamp skew validation
  -> sync-quality monitoring
  -> optional greenback foreground extraction per eye
  -> SBS NV12 packing
  -> latest-frame ring
  -> hardware encoder adapter
```

The current implementation stops before the hardware encoder. The output `MpFrame` is already shaped as one SBS NV12 frame.

## Optimization Posture

The current frame ring copies frame data. This is acceptable for early validation and unit tests. On Pi hardware, the capture/encode adapter should replace this path with DMABUF-backed ownership and keep the same policy decisions.

Good first NEON targets:

- full-frame matte fill.
- greenback chroma classification.
- 3x3 mask dilation.
- mask upscale.
- mask dilation.
- NV12 plane pack/copy.
- SBS packing from left/right NV12.
