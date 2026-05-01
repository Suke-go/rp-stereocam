#include "mps_i420_sbs.h"

#include <string.h>

size_t mps_i420_size(uint32_t width, uint32_t height)
{
    return (size_t)width * (size_t)height * 3u / 2u;
}

int mps_i420_pack_sbs(const uint8_t* left_i420,
                      const uint8_t* right_i420,
                      uint8_t* out_sbs_i420,
                      uint32_t eye_width,
                      uint32_t eye_height)
{
    const size_t eye_y_size = (size_t)eye_width * eye_height;
    const size_t eye_uv_size = eye_y_size / 4u;
    const uint8_t* left_y;
    const uint8_t* left_u;
    const uint8_t* left_v;
    const uint8_t* right_y;
    const uint8_t* right_u;
    const uint8_t* right_v;
    uint8_t* sbs_y;
    uint8_t* sbs_u;
    uint8_t* sbs_v;
    const uint32_t sbs_width = eye_width * 2u;
    const size_t sbs_y_size = (size_t)sbs_width * eye_height;
    const size_t sbs_uv_size = sbs_y_size / 4u;
    uint32_t row;

    if (!left_i420 || !right_i420 || !out_sbs_i420 || eye_width < 2u || eye_height < 2u ||
        (eye_width & 1u) != 0u || (eye_height & 1u) != 0u) {
        return -1;
    }

    left_y = left_i420;
    left_u = left_y + eye_y_size;
    left_v = left_u + eye_uv_size;
    right_y = right_i420;
    right_u = right_y + eye_y_size;
    right_v = right_u + eye_uv_size;
    sbs_y = out_sbs_i420;
    sbs_u = sbs_y + sbs_y_size;
    sbs_v = sbs_u + sbs_uv_size;

    for (row = 0; row < eye_height; ++row) {
        uint8_t* dst = sbs_y + (size_t)row * sbs_width;
        memcpy(dst, left_y + (size_t)row * eye_width, eye_width);
        memcpy(dst + eye_width, right_y + (size_t)row * eye_width, eye_width);
    }

    for (row = 0; row < eye_height / 2u; ++row) {
        uint8_t* dst_u = sbs_u + (size_t)row * eye_width;
        uint8_t* dst_v = sbs_v + (size_t)row * eye_width;
        const size_t src_offset = (size_t)row * (eye_width / 2u);
        memcpy(dst_u, left_u + src_offset, eye_width / 2u);
        memcpy(dst_u + eye_width / 2u, right_u + src_offset, eye_width / 2u);
        memcpy(dst_v, left_v + src_offset, eye_width / 2u);
        memcpy(dst_v + eye_width / 2u, right_v + src_offset, eye_width / 2u);
    }

    return 0;
}

int mps_i420_pack_sbs_mono(const uint8_t* eye_i420,
                           uint8_t* out_sbs_i420,
                           uint32_t eye_width,
                           uint32_t eye_height)
{
    return mps_i420_pack_sbs(eye_i420, eye_i420, out_sbs_i420, eye_width, eye_height);
}
