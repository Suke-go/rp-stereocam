#include "mps_nv12_sbs.h"

#include <string.h>

size_t mps_nv12_size(uint32_t width, uint32_t height)
{
    return (size_t)width * (size_t)height * 3u / 2u;
}

int mps_nv12_pack_sbs(const uint8_t* left_nv12,
                      const uint8_t* right_nv12,
                      uint8_t* out_sbs_nv12,
                      uint32_t eye_width,
                      uint32_t eye_height)
{
    const size_t eye_y_size = (size_t)eye_width * eye_height;
    const uint32_t sbs_width = eye_width * 2u;
    const size_t sbs_y_size = (size_t)sbs_width * eye_height;
    uint32_t row;

    if (!left_nv12 || !right_nv12 || !out_sbs_nv12 || eye_width < 2u || eye_height < 2u ||
        (eye_width & 1u) != 0u || (eye_height & 1u) != 0u) {
        return -1;
    }

    {
        const uint8_t* left_y = left_nv12;
        const uint8_t* right_y = right_nv12;
        uint8_t* sbs_y = out_sbs_nv12;
        for (row = 0; row < eye_height; ++row) {
            uint8_t* dst = sbs_y + (size_t)row * sbs_width;
            memcpy(dst, left_y + (size_t)row * eye_width, eye_width);
            memcpy(dst + eye_width, right_y + (size_t)row * eye_width, eye_width);
        }
    }

    /* UV plane: chroma rows = eye_height / 2, per row = eye_width interleaved bytes. */
    {
        const uint8_t* left_uv = left_nv12 + eye_y_size;
        const uint8_t* right_uv = right_nv12 + eye_y_size;
        uint8_t* sbs_uv = out_sbs_nv12 + sbs_y_size;
        const uint32_t chroma_rows = eye_height / 2u;
        for (row = 0; row < chroma_rows; ++row) {
            uint8_t* dst = sbs_uv + (size_t)row * sbs_width;
            memcpy(dst, left_uv + (size_t)row * eye_width, eye_width);
            memcpy(dst + eye_width, right_uv + (size_t)row * eye_width, eye_width);
        }
    }

    return 0;
}

int mps_nv12_pack_sbs_mono(const uint8_t* eye_nv12,
                           uint8_t* out_sbs_nv12,
                           uint32_t eye_width,
                           uint32_t eye_height)
{
    return mps_nv12_pack_sbs(eye_nv12, eye_nv12, out_sbs_nv12, eye_width, eye_height);
}

int mps_yuv420_pack_sbs_nv12(const uint8_t* left_yuv420,
                             const uint8_t* right_yuv420,
                             uint8_t* out_sbs_nv12,
                             uint32_t eye_width,
                             uint32_t eye_height)
{
    const size_t eye_y_size = (size_t)eye_width * eye_height;
    const size_t eye_uv_size = eye_y_size / 4u;
    const uint32_t sbs_width = eye_width * 2u;
    const size_t sbs_y_size = (size_t)sbs_width * eye_height;
    const uint32_t chroma_rows = eye_height / 2u;
    const uint32_t chroma_cols = eye_width / 2u;
    uint32_t row;
    uint32_t col;

    if (!left_yuv420 || !right_yuv420 || !out_sbs_nv12 || eye_width < 2u || eye_height < 2u ||
        (eye_width & 1u) != 0u || (eye_height & 1u) != 0u) {
        return -1;
    }

    {
        const uint8_t* left_y = left_yuv420;
        const uint8_t* right_y = right_yuv420;
        uint8_t* sbs_y = out_sbs_nv12;
        for (row = 0; row < eye_height; ++row) {
            uint8_t* dst = sbs_y + (size_t)row * sbs_width;
            memcpy(dst, left_y + (size_t)row * eye_width, eye_width);
            memcpy(dst + eye_width, right_y + (size_t)row * eye_width, eye_width);
        }
    }

    {
        const uint8_t* left_u = left_yuv420 + eye_y_size;
        const uint8_t* left_v = left_u + eye_uv_size;
        const uint8_t* right_u = right_yuv420 + eye_y_size;
        const uint8_t* right_v = right_u + eye_uv_size;
        uint8_t* sbs_uv = out_sbs_nv12 + sbs_y_size;
        for (row = 0; row < chroma_rows; ++row) {
            uint8_t* dst_row = sbs_uv + (size_t)row * sbs_width;
            const uint8_t* lu = left_u + (size_t)row * chroma_cols;
            const uint8_t* lv = left_v + (size_t)row * chroma_cols;
            const uint8_t* ru = right_u + (size_t)row * chroma_cols;
            const uint8_t* rv = right_v + (size_t)row * chroma_cols;
            uint8_t* dst_left = dst_row;
            uint8_t* dst_right = dst_row + eye_width;
            for (col = 0; col < chroma_cols; ++col) {
                dst_left[col * 2u] = lu[col];
                dst_left[col * 2u + 1u] = lv[col];
                dst_right[col * 2u] = ru[col];
                dst_right[col * 2u + 1u] = rv[col];
            }
        }
    }

    return 0;
}
