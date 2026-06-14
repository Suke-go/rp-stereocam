#include "mps_frame_envelope.h"

#include <string.h>

static void mps_frame_envelope_write_u16_le(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void mps_frame_envelope_write_u32_le(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
    p[2] = (uint8_t)((value >> 16) & 0xffu);
    p[3] = (uint8_t)((value >> 24) & 0xffu);
}

size_t mps_frame_envelope_required_size(size_t video_size, size_t mask_size)
{
    return (size_t)MPS_FRAME_ENVELOPE_HEADER_SIZE + video_size + mask_size;
}

int mps_frame_envelope_write(const uint8_t* video_data,
                             size_t video_size,
                             const uint8_t* mask_data,
                             size_t mask_size,
                             uint32_t mask_width,
                             uint32_t mask_height,
                             uint32_t mask_stride,
                             uint8_t* out_data,
                             size_t out_capacity,
                             size_t* out_size)
{
    const size_t required = mps_frame_envelope_required_size(video_size, mask_size);
    const uint32_t video_offset = MPS_FRAME_ENVELOPE_HEADER_SIZE;
    const uint32_t mask_offset = (uint32_t)(MPS_FRAME_ENVELOPE_HEADER_SIZE + video_size);
    const uint32_t flags = mask_size > 0u ? MPS_FRAME_ENVELOPE_FLAG_MASK_PRESENT : 0u;

    if (!video_data || video_size == 0u || !out_data || !out_size) {
        return -1;
    }
    if (video_size > 0xffffffffu || mask_size > 0xffffffffu || required > 0xffffffffu) {
        return -2;
    }
    if (mask_size > 0u) {
        if (!mask_data || mask_width == 0u || mask_height == 0u || mask_stride < mask_width ||
            mask_size < (size_t)mask_stride * mask_height) {
            return -3;
        }
    }
    if (out_capacity < required) {
        return -4;
    }

    memset(out_data, 0, MPS_FRAME_ENVELOPE_HEADER_SIZE);
    mps_frame_envelope_write_u32_le(out_data + 0, MPS_FRAME_ENVELOPE_MAGIC);
    mps_frame_envelope_write_u16_le(out_data + 4, MPS_FRAME_ENVELOPE_VERSION);
    mps_frame_envelope_write_u16_le(out_data + 6, MPS_FRAME_ENVELOPE_HEADER_SIZE);
    mps_frame_envelope_write_u32_le(out_data + 8, flags);
    mps_frame_envelope_write_u32_le(out_data + 12, video_offset);
    mps_frame_envelope_write_u32_le(out_data + 16, (uint32_t)video_size);
    mps_frame_envelope_write_u32_le(out_data + 20, mask_size > 0u ? mask_offset : 0u);
    mps_frame_envelope_write_u32_le(out_data + 24, (uint32_t)mask_size);
    mps_frame_envelope_write_u32_le(out_data + 28, mask_width);
    mps_frame_envelope_write_u32_le(out_data + 32, mask_height);
    mps_frame_envelope_write_u32_le(out_data + 36, mask_stride);
    mps_frame_envelope_write_u32_le(out_data + 40, mask_size > 0u ? MPS_FRAME_ENVELOPE_MASK_FORMAT_R8 : MPS_FRAME_ENVELOPE_MASK_FORMAT_NONE);
    mps_frame_envelope_write_u32_le(out_data + 44, 0u);

    memcpy(out_data + video_offset, video_data, video_size);
    if (mask_size > 0u) {
        memcpy(out_data + mask_offset, mask_data, mask_size);
    }
    *out_size = required;
    return 0;
}
