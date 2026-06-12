#include "imt.h"

/*
 * R5.2: smaller groups give thicker protection.
 * w~ >= 192 -> 2, w~ >= 128 -> 4, w~ >= 64 -> 8, otherwise 16.
 */
int imt_fec_group_size(uint32_t normalized_weight)
{
    if (normalized_weight == 0u) {
        normalized_weight = 1u; /* R4.2 */
    }
    if (normalized_weight >= 192u) {
        return 2;
    }
    if (normalized_weight >= 128u) {
        return 4;
    }
    if (normalized_weight >= 64u) {
        return 8;
    }
    return 16;
}

/*
 * R7.1: parity is the XOR of the group's data payloads with short payloads
 * zero-extended. XORing only the first data_size bytes and leaving the tail
 * untouched is exactly the zero-extension semantics.
 */
int imt_fec_xor_accumulate(uint8_t* parity, size_t parity_size,
                           const uint8_t* data, size_t data_size)
{
    size_t i;

    if (!parity || !data) {
        return -1;
    }
    if (data_size > parity_size) {
        return -2;
    }

    for (i = 0; i < data_size; ++i) {
        parity[i] ^= data[i];
    }
    return 0;
}
