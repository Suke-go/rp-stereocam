#include "mps_i420_foreground_mask.h"

#include <stddef.h>

MpsI420GreenbackMaskConfig mps_i420_greenback_mask_config_default(void)
{
    MpsI420GreenbackMaskConfig config;
    config.key_u = 54u;
    config.key_v = 34u;
    config.chroma_tolerance = 34u;
    return config;
}

static int mps_i420_is_foreground(const uint8_t* u_plane,
                                  const uint8_t* v_plane,
                                  uint32_t eye_width,
                                  uint32_t x,
                                  uint32_t y,
                                  const MpsI420GreenbackMaskConfig* config)
{
    const uint32_t chroma_width = eye_width / 2u;
    const size_t chroma_index = (size_t)(y / 2u) * chroma_width + (x / 2u);
    const int du = (int)u_plane[chroma_index] - (int)config->key_u;
    const int dv = (int)v_plane[chroma_index] - (int)config->key_v;
    const int tolerance = (int)config->chroma_tolerance;
    return du * du + dv * dv > tolerance * tolerance;
}

static void mps_i420_write_eye_mask(const uint8_t* i420,
                                    uint32_t eye_width,
                                    uint32_t eye_height,
                                    uint32_t mask_eye_width,
                                    uint32_t mask_eye_height,
                                    uint8_t* out_sbs_r8,
                                    uint32_t out_stride,
                                    uint32_t out_x_offset,
                                    const MpsI420GreenbackMaskConfig* config)
{
    const size_t y_plane_size = (size_t)eye_width * eye_height;
    const uint8_t* u_plane = i420 + y_plane_size;
    const uint8_t* v_plane = u_plane + y_plane_size / 4u;
    uint32_t my;

    for (my = 0; my < mask_eye_height; ++my) {
        const uint32_t y0 = (uint32_t)(((uint64_t)my * eye_height) / mask_eye_height);
        uint32_t y1 = (uint32_t)(((uint64_t)(my + 1u) * eye_height) / mask_eye_height);
        uint32_t mx;

        if (y1 <= y0) {
            y1 = y0 + 1u;
        }
        if (y1 > eye_height) {
            y1 = eye_height;
        }

        for (mx = 0; mx < mask_eye_width; ++mx) {
            const uint32_t x0 = (uint32_t)(((uint64_t)mx * eye_width) / mask_eye_width);
            uint32_t x1 = (uint32_t)(((uint64_t)(mx + 1u) * eye_width) / mask_eye_width);
            uint32_t foreground_count = 0u;
            uint32_t sample_count = 0u;
            uint32_t sy;

            if (x1 <= x0) {
                x1 = x0 + 1u;
            }
            if (x1 > eye_width) {
                x1 = eye_width;
            }

            for (sy = y0; sy < y1; ++sy) {
                uint32_t sx;
                for (sx = x0; sx < x1; ++sx) {
                    foreground_count += (uint32_t)mps_i420_is_foreground(u_plane, v_plane, eye_width, sx, sy, config);
                    sample_count += 1u;
                }
            }
            out_sbs_r8[(size_t)my * out_stride + out_x_offset + mx] =
                sample_count ? (uint8_t)((foreground_count * 255u + sample_count / 2u) / sample_count) : 0u;
        }
    }
}

int mps_i420_greenback_foreground_mask_sbs_r8(const uint8_t* left_i420,
                                              const uint8_t* right_i420,
                                              uint32_t eye_width,
                                              uint32_t eye_height,
                                              uint32_t mask_eye_width,
                                              uint32_t mask_eye_height,
                                              uint8_t* out_sbs_r8,
                                              uint32_t out_stride,
                                              const MpsI420GreenbackMaskConfig* config)
{
    MpsI420GreenbackMaskConfig default_config;

    if (!left_i420 || !right_i420 || !out_sbs_r8 ||
        eye_width < 2u || eye_height < 2u ||
        (eye_width & 1u) != 0u || (eye_height & 1u) != 0u ||
        mask_eye_width == 0u || mask_eye_height == 0u ||
        out_stride < mask_eye_width * 2u) {
        return -1;
    }

    default_config = mps_i420_greenback_mask_config_default();
    if (!config) {
        config = &default_config;
    }

    mps_i420_write_eye_mask(left_i420,
                            eye_width,
                            eye_height,
                            mask_eye_width,
                            mask_eye_height,
                            out_sbs_r8,
                            out_stride,
                            0u,
                            config);
    mps_i420_write_eye_mask(right_i420,
                            eye_width,
                            eye_height,
                            mask_eye_width,
                            mask_eye_height,
                            out_sbs_r8,
                            out_stride,
                            mask_eye_width,
                            config);
    return 0;
}

int mps_i420_apply_sbs_r8_foreground_matte(uint8_t* sbs_i420,
                                           uint32_t sbs_width,
                                           uint32_t sbs_height,
                                           const uint8_t* mask_sbs_r8,
                                           uint32_t mask_width,
                                           uint32_t mask_height,
                                           uint32_t mask_stride,
                                           uint8_t mask_threshold,
                                           uint8_t matte_y,
                                           uint8_t matte_u,
                                           uint8_t matte_v)
{
    uint8_t* y_plane;
    uint8_t* u_plane;
    uint8_t* v_plane;
    uint32_t y;

    if (!sbs_i420 || !mask_sbs_r8 ||
        sbs_width < 2u || sbs_height < 2u ||
        (sbs_width & 1u) != 0u || (sbs_height & 1u) != 0u ||
        mask_width == 0u || mask_height == 0u || mask_stride < mask_width) {
        return -1;
    }

    y_plane = sbs_i420;
    u_plane = y_plane + (size_t)sbs_width * sbs_height;
    v_plane = u_plane + (size_t)(sbs_width / 2u) * (sbs_height / 2u);

    for (y = 0; y < sbs_height; ++y) {
        const uint32_t my = (uint32_t)(((uint64_t)y * mask_height) / sbs_height);
        uint32_t x;
        for (x = 0; x < sbs_width; ++x) {
            const uint32_t mx = (uint32_t)(((uint64_t)x * mask_width) / sbs_width);
            if (mask_sbs_r8[(size_t)my * mask_stride + mx] < mask_threshold) {
                y_plane[(size_t)y * sbs_width + x] = matte_y;
            }
        }
    }

    for (y = 0; y < sbs_height / 2u; ++y) {
        const uint32_t source_y = y * 2u;
        const uint32_t my = (uint32_t)(((uint64_t)source_y * mask_height) / sbs_height);
        uint32_t x;
        for (x = 0; x < sbs_width / 2u; ++x) {
            const uint32_t source_x = x * 2u;
            const uint32_t mx = (uint32_t)(((uint64_t)source_x * mask_width) / sbs_width);
            if (mask_sbs_r8[(size_t)my * mask_stride + mx] < mask_threshold) {
                u_plane[(size_t)y * (sbs_width / 2u) + x] = matte_u;
                v_plane[(size_t)y * (sbs_width / 2u) + x] = matte_v;
            }
        }
    }

    return 0;
}
