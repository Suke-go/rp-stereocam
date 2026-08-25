#ifndef MPS_LIBCAMERA_STEREO_CAPTURE_H
#define MPS_LIBCAMERA_STEREO_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MpsLibcameraStereoCapture MpsLibcameraStereoCapture;

typedef struct MpsLibcameraStereoPair {
    const uint8_t* left_i420;
    const uint8_t* right_i420;
    size_t frame_size;
    uint64_t frame_sequence;
    uint64_t capture_timestamp_ns;
    uint64_t timestamp_skew_ns;
} MpsLibcameraStereoPair;

/* Direct dual-camera capture. Completed libcamera requests are copied once
 * from their stride-padded mmap planes into pooled contiguous I420 buffers;
 * ownership of those buffers is then transferred to the caller until
 * release_pair. No child process, stdout pipe, or per-frame allocation is
 * used in steady state. */
MpsLibcameraStereoCapture* mps_libcamera_stereo_create(
    uint32_t left_camera,
    uint32_t right_camera,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t buffer_count,
    uint64_t max_skew_ns,
    int software_sync);
int mps_libcamera_stereo_start(MpsLibcameraStereoCapture* capture);
/* Returns 1 with a pair, 0 on timeout, negative on failure. Only one pair may
 * be outstanding at a time. */
int mps_libcamera_stereo_acquire_pair(MpsLibcameraStereoCapture* capture,
                                      uint32_t timeout_ms,
                                      MpsLibcameraStereoPair* out_pair);
void mps_libcamera_stereo_release_pair(MpsLibcameraStereoCapture* capture);
uint64_t mps_libcamera_stereo_dropped_frames(const MpsLibcameraStereoCapture* capture);
void mps_libcamera_stereo_destroy(MpsLibcameraStereoCapture* capture);

#ifdef __cplusplus
}
#endif

#endif
