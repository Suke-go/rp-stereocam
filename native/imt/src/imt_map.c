#include "imt.h"

#include <stdlib.h>
#include <string.h>

#define IMT_MAP_DEFAULT_EMA_SHIFT 3u
#define IMT_MAP_BYTE_LO_BITS UINT64_C(0x0101010101010101)
#define IMT_MAP_BYTE_HI_BITS UINT64_C(0x8080808080808080)

static uint32_t imt_map_sum_8_weights(const uint8_t* weights)
{
    uint64_t packed;
    uint64_t sum;

    memcpy(&packed, weights, sizeof(packed));
    if (((packed - IMT_MAP_BYTE_LO_BITS) & ~packed & IMT_MAP_BYTE_HI_BITS) != 0u) {
        uint32_t i;
        uint32_t fallback_sum = 0u;
        for (i = 0; i < 8u; ++i) {
            const uint8_t weight = weights[i];
            fallback_sum += weight > 1u ? (uint32_t)weight : 1u;
        }
        return fallback_sum;
    }

    sum = (packed & UINT64_C(0x00FF00FF00FF00FF)) +
          ((packed >> 8) & UINT64_C(0x00FF00FF00FF00FF));
    sum = (sum & UINT64_C(0x0000FFFF0000FFFF)) +
          ((sum >> 16) & UINT64_C(0x0000FFFF0000FFFF));
    sum = (sum & UINT64_C(0x00000000FFFFFFFF)) + (sum >> 32);
    return (uint32_t)sum;
}

int imt_map_init(ImtMap* map, uint32_t frame_width, uint32_t frame_height,
                 uint16_t tile_size)
{
    uint32_t cols;
    uint32_t rows;

    if (!map || frame_width == 0u || frame_height == 0u || tile_size == 0u) {
        return -1;
    }

    cols = (frame_width + tile_size - 1u) / tile_size;
    rows = (frame_height + tile_size - 1u) / tile_size;
    if (cols > 0xFFFFu || rows > 0xFFFFu || (rows != 0u && cols > 0xFFFFFFFFu / rows)) {
        return -2;
    }

    memset(map, 0, sizeof(*map));
    map->weights = (uint8_t*)malloc((size_t)cols * rows);
    if (!map->weights) {
        return -3;
    }

    map->cols = (uint16_t)cols;
    map->rows = (uint16_t)rows;
    map->tile_size = tile_size;
    map->tile_count = cols * rows;
    map->ema_shift = IMT_MAP_DEFAULT_EMA_SHIFT;
    memset(map->weights, 128, map->tile_count);
    return 0;
}

void imt_map_destroy(ImtMap* map)
{
    if (!map) {
        return;
    }
    free(map->weights);
    memset(map, 0, sizeof(*map));
}

int imt_map_fill(ImtMap* map, uint8_t weight)
{
    if (!map || !map->weights) {
        return -1;
    }
    memset(map->weights, weight == 0u ? 1 : weight, map->tile_count);
    return 0;
}

int imt_map_update_ema(ImtMap* map, const uint8_t* samples, uint32_t sample_count)
{
    uint32_t i;
    uint32_t shift;
    uint32_t scale;

    if (!map || !map->weights || !samples) {
        return -1;
    }
    if (sample_count != map->tile_count) {
        return -2;
    }
    shift = map->ema_shift;
    if (shift == 0u || shift > 7u) {
        return -3;
    }

    scale = (1u << shift) - 1u;
    i = 0;
    while (i + 8u <= map->tile_count) {
        uint32_t j;
        for (j = 0; j < 8u; ++j) {
            const uint32_t sample_raw = samples[i + j];
            const uint32_t old_raw = map->weights[i + j];
            const uint32_t sample = sample_raw > 1u ? sample_raw : 1u;
            const uint32_t old_weight = old_raw > 1u ? old_raw : 1u;
            map->weights[i + j] = (uint8_t)((old_weight * scale + sample) >> shift);
        }
        i += 8u;
    }
    for (; i < map->tile_count; ++i) {
        const uint32_t sample_raw = samples[i];
        const uint32_t old_raw = map->weights[i];
        const uint32_t sample = sample_raw > 1u ? sample_raw : 1u;
        const uint32_t old_weight = old_raw > 1u ? old_raw : 1u;
        map->weights[i] = (uint8_t)((old_weight * scale + sample) >> shift);
    }
    return 0;
}

int imt_map_mean(const ImtMap* map)
{
    uint32_t i;
    uint64_t sum = 0;
    uint64_t mean;

    if (!map || !map->weights || map->tile_count == 0u) {
        return -1;
    }

    i = 0;
    while (i + 8u <= map->tile_count) {
        sum += imt_map_sum_8_weights(map->weights + i);
        i += 8u;
    }
    for (; i < map->tile_count; ++i) {
        const uint8_t weight = map->weights[i];
        sum += weight > 1u ? (uint64_t)weight : 1u;
    }
    mean = (sum + (uint64_t)map->tile_count / 2u) / map->tile_count;
    if (mean == 0u) {
        mean = 1u;
    }
    return (int)mean;
}

int imt_map_normalized_weight(uint32_t weight, uint32_t mean)
{
    uint32_t normalized;

    if (weight == 0u) {
        weight = 1u;
    }
    if (mean == 0u) {
        mean = 1u;
    }
    normalized = weight * 128u / mean;
    if (normalized < 1u) {
        normalized = 1u;
    }
    if (normalized > 255u) {
        normalized = 255u;
    }
    return (int)normalized;
}
