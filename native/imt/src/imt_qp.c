#include "imt.h"

#include <math.h>

#define IMT_QP_MIN (-6)
#define IMT_QP_MAX 12

/*
 * R5.1: dqp(w~) = round(-6 * log2(w~ / 128)) clamped to [-6, +12].
 * Built once at init; floating point is allowed here only (R2.3).
 */
int imt_qp_lut_init(ImtQpLut* lut)
{
    int i;

    if (!lut) {
        return -1;
    }

    for (i = 1; i < 256; ++i) {
        const double value = -6.0 * log2((double)i / 128.0);
        int dqp = (int)floor(value + 0.5);
        if (dqp < IMT_QP_MIN) {
            dqp = IMT_QP_MIN;
        }
        if (dqp > IMT_QP_MAX) {
            dqp = IMT_QP_MAX;
        }
        lut->dqp[i] = (int8_t)dqp;
    }
    lut->dqp[0] = lut->dqp[1]; /* weight 0 is treated as 1 (R4.2) */
    return 0;
}

int imt_qp_offset(const ImtQpLut* lut, uint8_t weight, uint32_t mean)
{
    if (!lut) {
        return 0;
    }
    return lut->dqp[imt_map_normalized_weight(weight, mean)];
}

int imt_qp_offsets_for_map(const ImtQpLut* lut, const ImtMap* map,
                           int8_t* dst, size_t dst_count)
{
    int8_t frame_lut[256];
    uint32_t i;
    int mean;

    if (!lut || !map || !map->weights || !dst) {
        return -1;
    }
    if (dst_count < map->tile_count) {
        return -2;
    }

    mean = imt_map_mean(map);
    if (mean < 0) {
        return -3;
    }

    for (i = 0; i < 256u; ++i) {
        frame_lut[i] = lut->dqp[imt_map_normalized_weight(i, (uint32_t)mean)];
    }
    i = 0;
    while (i + 8u <= map->tile_count) {
        dst[i + 0u] = frame_lut[map->weights[i + 0u]];
        dst[i + 1u] = frame_lut[map->weights[i + 1u]];
        dst[i + 2u] = frame_lut[map->weights[i + 2u]];
        dst[i + 3u] = frame_lut[map->weights[i + 3u]];
        dst[i + 4u] = frame_lut[map->weights[i + 4u]];
        dst[i + 5u] = frame_lut[map->weights[i + 5u]];
        dst[i + 6u] = frame_lut[map->weights[i + 6u]];
        dst[i + 7u] = frame_lut[map->weights[i + 7u]];
        i += 8u;
    }
    for (; i < map->tile_count; ++i) {
        dst[i] = frame_lut[map->weights[i]];
    }
    return 0;
}
