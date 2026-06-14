#include "imt.h"

#include <string.h>

/*
 * R5.3: stable counting sort, 256 buckets, descending weight, O(n).
 * No comparison sort, no heap allocation, no floating point.
 */
int imt_sched_order(const uint8_t* weights, uint32_t count, uint16_t* out_order)
{
    uint32_t counts[256];
    uint32_t positions[256];
    uint32_t i;
    int w;

    if (!weights || !out_order) {
        return -1;
    }
    if (count == 0u || count > 65536u) {
        return -2;
    }

    memset(counts, 0, sizeof(counts));
    i = 0;
    while (i + 8u <= count) {
        counts[weights[i + 0u]] += 1u;
        counts[weights[i + 1u]] += 1u;
        counts[weights[i + 2u]] += 1u;
        counts[weights[i + 3u]] += 1u;
        counts[weights[i + 4u]] += 1u;
        counts[weights[i + 5u]] += 1u;
        counts[weights[i + 6u]] += 1u;
        counts[weights[i + 7u]] += 1u;
        i += 8u;
    }
    for (; i < count; ++i) {
        counts[weights[i]] += 1u;
    }

    /* Descending: weight 255 occupies the front of the order array. */
    positions[255] = 0;
    for (w = 254; w >= 0; --w) {
        positions[w] = positions[w + 1] + counts[w + 1];
    }

    /* A forward scan keeps equal weights in original index order (stable). */
    i = 0;
    while (i + 8u <= count) {
        const uint8_t w0 = weights[i + 0u];
        const uint8_t w1 = weights[i + 1u];
        const uint8_t w2 = weights[i + 2u];
        const uint8_t w3 = weights[i + 3u];
        const uint8_t w4 = weights[i + 4u];
        const uint8_t w5 = weights[i + 5u];
        const uint8_t w6 = weights[i + 6u];
        const uint8_t w7 = weights[i + 7u];
        out_order[positions[w0]] = (uint16_t)(i + 0u);
        positions[w0] += 1u;
        out_order[positions[w1]] = (uint16_t)(i + 1u);
        positions[w1] += 1u;
        out_order[positions[w2]] = (uint16_t)(i + 2u);
        positions[w2] += 1u;
        out_order[positions[w3]] = (uint16_t)(i + 3u);
        positions[w3] += 1u;
        out_order[positions[w4]] = (uint16_t)(i + 4u);
        positions[w4] += 1u;
        out_order[positions[w5]] = (uint16_t)(i + 5u);
        positions[w5] += 1u;
        out_order[positions[w6]] = (uint16_t)(i + 6u);
        positions[w6] += 1u;
        out_order[positions[w7]] = (uint16_t)(i + 7u);
        positions[w7] += 1u;
        i += 8u;
    }
    for (; i < count; ++i) {
        out_order[positions[weights[i]]] = (uint16_t)i;
        positions[weights[i]] += 1u;
    }
    return 0;
}
