#ifndef MPS_FRAME_ENVELOPE_H
#define MPS_FRAME_ENVELOPE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MPS_FRAME_ENVELOPE_MAGIC 0x3145464du
#define MPS_FRAME_ENVELOPE_VERSION 1u
#define MPS_FRAME_ENVELOPE_HEADER_SIZE 48u

typedef enum MpsFrameEnvelopeFlags {
    MPS_FRAME_ENVELOPE_FLAG_MASK_PRESENT = 1u << 0
} MpsFrameEnvelopeFlags;

typedef enum MpsFrameEnvelopeMaskFormat {
    MPS_FRAME_ENVELOPE_MASK_FORMAT_NONE = 0u,
    MPS_FRAME_ENVELOPE_MASK_FORMAT_R8 = 1u
} MpsFrameEnvelopeMaskFormat;

typedef struct MpsFrameEnvelopeDesc {
    uint32_t flags;
    uint32_t video_offset;
    uint32_t video_size;
    uint32_t mask_offset;
    uint32_t mask_size;
    uint32_t mask_width;
    uint32_t mask_height;
    uint32_t mask_stride;
    uint32_t mask_format;
} MpsFrameEnvelopeDesc;

size_t mps_frame_envelope_required_size(size_t video_size, size_t mask_size);
int mps_frame_envelope_write(const uint8_t* video_data,
                             size_t video_size,
                             const uint8_t* mask_data,
                             size_t mask_size,
                             uint32_t mask_width,
                             uint32_t mask_height,
                             uint32_t mask_stride,
                             uint8_t* out_data,
                             size_t out_capacity,
                             size_t* out_size);

#ifdef __cplusplus
}
#endif

#endif
