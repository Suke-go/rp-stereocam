/*
 * imt_tests - single self-contained test executable for the imt library.
 * Plain C, no framework (R9.1). Returns nonzero when any check fails.
 */

#include "imt.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

/* ------------------------------------------------------------------ */
/* Tiny check harness                                                 */
/* ------------------------------------------------------------------ */

static int g_checks_passed;
static int g_checks_failed;

static void imt_test_check(int ok, const char* expr, int line)
{
    if (ok) {
        g_checks_passed += 1;
    } else {
        g_checks_failed += 1;
        printf("FAIL line %d: %s\n", line, expr);
    }
}

#define CHECK(cond) imt_test_check((cond) ? 1 : 0, #cond, __LINE__)

/* Fixed-seed LCG (Numerical Recipes constants); rand() is not used. */
static uint32_t imt_test_lcg(uint32_t* state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void imt_test_fill_random(uint8_t* dst, size_t size, uint32_t seed)
{
    size_t i;
    uint32_t state = seed;
    for (i = 0; i < size; ++i) {
        dst[i] = (uint8_t)(imt_test_lcg(&state) >> 24);
    }
}

static uint64_t imt_test_now_ns(void)
{
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (uint64_t)((double)counter.QuadPart * 1e9 / (double)frequency.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* ------------------------------------------------------------------ */
/* Datagram capture used as the fake network                          */
/* ------------------------------------------------------------------ */

#define TEST_MAX_DATAGRAMS 128
#define TEST_MAX_DATAGRAM_SIZE 8192
#define TEST_FRAME_SIZE 16000u
#define TEST_MAX_FRAME 65536u

typedef struct TestCapture {
    uint8_t data[TEST_MAX_DATAGRAMS][TEST_MAX_DATAGRAM_SIZE];
    size_t sizes[TEST_MAX_DATAGRAMS];
    ImtWireHeader headers[TEST_MAX_DATAGRAMS];
    int count;
} TestCapture;

static TestCapture g_capture;
static uint8_t g_frame[TEST_FRAME_SIZE];
static uint8_t g_copy[TEST_MAX_FRAME];

static int imt_test_emit(const uint8_t* datagram, size_t len, void* user)
{
    TestCapture* capture = (TestCapture*)user;
    if (capture->count >= TEST_MAX_DATAGRAMS || len > TEST_MAX_DATAGRAM_SIZE) {
        return -1;
    }
    memcpy(capture->data[capture->count], datagram, len);
    capture->sizes[capture->count] = len;
    if (imt_wire_decode_header(datagram, len, &capture->headers[capture->count]) != 0) {
        return -2;
    }
    capture->count += 1;
    return 0;
}

static int imt_test_capture_frame(ImtPacketizer* packetizer, uint64_t frame_seq,
                                  const uint8_t* weights, uint32_t weight_count)
{
    g_capture.count = 0;
    imt_test_fill_random(g_frame, TEST_FRAME_SIZE, (uint32_t)frame_seq * 7919u + 17u);
    return imt_packetizer_send_frame(packetizer, g_frame, TEST_FRAME_SIZE, frame_seq,
                                     frame_seq * 1000u, 0, weights, weight_count,
                                     imt_test_emit, &g_capture);
}

/* Delivers every captured datagram whose index is not flagged in drop[]. */
static void imt_test_deliver(ImtAssembler* assembler, const uint8_t* drop)
{
    int i;
    for (i = 0; i < g_capture.count; ++i) {
        if (drop && drop[i]) {
            continue;
        }
        CHECK(imt_assembler_push_datagram(assembler, g_capture.data[i],
                                          g_capture.sizes[i]) >= 0);
    }
}

/* ------------------------------------------------------------------ */
/* imt_map                                                            */
/* ------------------------------------------------------------------ */

static void test_map(void)
{
    ImtMap map;
    uint8_t samples[7200];
    uint32_t i;
    int rc;

    CHECK(imt_map_init(&map, 2560, 720, 16) == 0);
    CHECK(map.cols == 160 && map.rows == 45 && map.tile_count == 7200);
    CHECK(map.tile_size == 16 && map.ema_shift == 3);
    for (i = 0; i < map.tile_count; ++i) {
        CHECK(map.weights[i] == 128);
        if (map.weights[i] != 128) {
            break;
        }
    }

    /* fill: weight 0 is stored as 1 (R4.2) */
    CHECK(imt_map_fill(&map, 0) == 0);
    CHECK(map.weights[0] == 1 && map.weights[map.tile_count - 1u] == 1);

    /* EMA from above converges exactly to the constant input (R4.3) */
    CHECK(imt_map_fill(&map, 255) == 0);
    memset(samples, 100, sizeof(samples));
    for (i = 0; i < 64; ++i) {
        rc = imt_map_update_ema(&map, samples, map.tile_count);
        CHECK(rc == 0);
        if (rc != 0) {
            break;
        }
    }
    CHECK(map.weights[0] == 100 && map.weights[map.tile_count - 1u] == 100);

    /* EMA from below stalls within (2^s - 1) below the input (floor EMA) */
    CHECK(imt_map_fill(&map, 1) == 0);
    memset(samples, 200, sizeof(samples));
    for (i = 0; i < 64; ++i) {
        (void)imt_map_update_ema(&map, samples, map.tile_count);
    }
    CHECK(map.weights[0] >= 193 && map.weights[0] <= 200);
    CHECK(map.weights[map.tile_count - 1u] >= 193);

    /* mean */
    for (i = 0; i < map.tile_count; ++i) {
        map.weights[i] = (uint8_t)(i < map.tile_count / 2u ? 100 : 201);
    }
    CHECK(imt_map_mean(&map) == 151); /* rounded 150.5 */

    /* normalization w~ = clamp(1, w*128/mean, 255) (R5.1) */
    CHECK(imt_map_normalized_weight(128, 128) == 128);
    CHECK(imt_map_normalized_weight(64, 128) == 64);
    CHECK(imt_map_normalized_weight(255, 64) == 255); /* clamped high */
    CHECK(imt_map_normalized_weight(1, 255) == 1);    /* clamped low */
    CHECK(imt_map_normalized_weight(0, 128) == 1);    /* 0 treated as 1 */
    CHECK(imt_map_normalized_weight(100, 0) == 255);  /* mean 0 treated as 1 */

    /* argument validation */
    CHECK(imt_map_update_ema(&map, samples, map.tile_count - 1u) == -2);
    CHECK(imt_map_init(NULL, 2560, 720, 16) < 0);
    CHECK(imt_map_init(&map, 0, 720, 16) < 0);

    imt_map_destroy(&map);
    CHECK(map.weights == NULL);
}

/* ------------------------------------------------------------------ */
/* imt_qp                                                             */
/* ------------------------------------------------------------------ */

static void test_qp(void)
{
    ImtQpLut lut;
    int i;

    CHECK(imt_qp_lut_init(&lut) == 0);
    CHECK(imt_qp_lut_init(NULL) < 0);

    /* R5.1 verification points */
    CHECK(lut.dqp[128] == 0);
    CHECK(lut.dqp[255] == -6);
    CHECK(lut.dqp[64] == 6);
    CHECK(lut.dqp[32] == 12);
    CHECK(lut.dqp[1] == 12); /* clamped */
    CHECK(lut.dqp[0] == lut.dqp[1]);

    /* monotone non-increasing over the whole LUT */
    for (i = 2; i < 256; ++i) {
        if (lut.dqp[i] > lut.dqp[i - 1]) {
            CHECK(lut.dqp[i] <= lut.dqp[i - 1]);
            break;
        }
    }
    CHECK(i == 256);

    /* range is always within [-6, +12] */
    for (i = 0; i < 256; ++i) {
        if (lut.dqp[i] < -6 || lut.dqp[i] > 12) {
            break;
        }
    }
    CHECK(i == 256);

    /* helper goes through normalization (mean 128 keeps w~ == w) */
    CHECK(imt_qp_offset(&lut, 128, 128) == 0);
    CHECK(imt_qp_offset(&lut, 255, 128) == -6);
    CHECK(imt_qp_offset(&lut, 64, 128) == 6);
    CHECK(imt_qp_offset(&lut, 32, 128) == 12);
    CHECK(imt_qp_offset(&lut, 1, 128) == 12);
}

/* ------------------------------------------------------------------ */
/* imt_sched                                                          */
/* ------------------------------------------------------------------ */

static void test_sched(void)
{
    static const uint8_t weights[5] = { 10, 200, 200, 5, 255 };
    uint16_t order[5];
    uint8_t random_weights[1000];
    uint16_t random_order[1000];
    uint8_t seen[1000];
    uint32_t i;

    CHECK(imt_sched_order(weights, 5, order) == 0);
    CHECK(order[0] == 4); /* 255 */
    CHECK(order[1] == 1); /* 200, first occurrence */
    CHECK(order[2] == 2); /* 200, second occurrence (stability) */
    CHECK(order[3] == 0); /* 10 */
    CHECK(order[4] == 3); /* 5 */

    imt_test_fill_random(random_weights, sizeof(random_weights), 42u);
    CHECK(imt_sched_order(random_weights, 1000, random_order) == 0);
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < 1000; ++i) {
        seen[random_order[i]] += 1u;
    }
    for (i = 0; i < 1000; ++i) {
        if (seen[i] != 1u) {
            break;
        }
    }
    CHECK(i == 1000); /* permutation */
    for (i = 1; i < 1000; ++i) {
        const uint8_t prev = random_weights[random_order[i - 1]];
        const uint8_t curr = random_weights[random_order[i]];
        if (curr > prev) {
            break; /* not descending */
        }
        if (curr == prev && random_order[i] < random_order[i - 1]) {
            break; /* not stable */
        }
    }
    CHECK(i == 1000);

    CHECK(imt_sched_order(NULL, 5, order) < 0);
    CHECK(imt_sched_order(weights, 0, order) < 0);
}

/* ------------------------------------------------------------------ */
/* imt_fec primitives                                                 */
/* ------------------------------------------------------------------ */

static void test_fec(void)
{
    uint8_t parity[8];
    static const uint8_t data[3] = { 0xAA, 0x55, 0xFF };

    /* R5.2 thresholds */
    CHECK(imt_fec_group_size(255) == 2);
    CHECK(imt_fec_group_size(192) == 2);
    CHECK(imt_fec_group_size(191) == 4);
    CHECK(imt_fec_group_size(128) == 4);
    CHECK(imt_fec_group_size(127) == 8);
    CHECK(imt_fec_group_size(64) == 8);
    CHECK(imt_fec_group_size(63) == 16);
    CHECK(imt_fec_group_size(1) == 16);
    CHECK(imt_fec_group_size(0) == 16); /* 0 treated as 1 */

    /* XOR with zero-extension semantics (R7.1) */
    memset(parity, 0, sizeof(parity));
    CHECK(imt_fec_xor_accumulate(parity, sizeof(parity), data, sizeof(data)) == 0);
    CHECK(parity[0] == 0xAA && parity[2] == 0xFF && parity[3] == 0x00);
    CHECK(imt_fec_xor_accumulate(parity, sizeof(parity), data, sizeof(data)) == 0);
    CHECK(parity[0] == 0x00 && parity[1] == 0x00 && parity[2] == 0x00);
    CHECK(imt_fec_xor_accumulate(parity, 2, data, sizeof(data)) == -2);
    CHECK(imt_fec_xor_accumulate(NULL, 8, data, 3) < 0);
}

/* ------------------------------------------------------------------ */
/* imt_wire                                                           */
/* ------------------------------------------------------------------ */

static void test_wire(void)
{
    ImtWireHeader header;
    ImtWireHeader decoded;
    ImtFeedback feedback;
    ImtFeedback feedback_decoded;
    ImtMapPayloadHeader map_header;
    ImtMapPayloadHeader map_decoded;
    uint8_t buffer[IMT_WIRE_HEADER_SIZE + 64];

    memset(&header, 0, sizeof(header));
    header.type = IMT_PACKET_TYPE_PARITY;
    header.flags = IMT_PACKET_FLAG_KEYFRAME | IMT_PACKET_FLAG_CODEC_CONFIG;
    header.frame_seq = 0x1122334455667788ull;
    header.capture_timestamp_ns = 0x99AABBCCDDEEFF00ull;
    header.frame_size = 0xDEADBEEFu;
    header.chunk_index = 0x1234;
    header.chunk_count = 0x5678;
    header.payload_size = 64;
    header.fec_group = 0xCAFE;
    header.fec_group_size = 0x7F;
    header.fec_index = 0x0F;

    memset(buffer, 0xCC, sizeof(buffer));
    CHECK(imt_wire_encode_header(&header, buffer, sizeof(buffer)) == 0);

    /* spot-check the exact R6.1 byte layout */
    CHECK(buffer[0] == 0x49 && buffer[1] == 0x4D);      /* "IM" */
    CHECK(buffer[2] == 1);                              /* version */
    CHECK(buffer[3] == IMT_PACKET_TYPE_PARITY);         /* type */
    CHECK(buffer[4] == 40 && buffer[5] == 0);           /* header_size LE */
    CHECK(buffer[6] == 0x03 && buffer[7] == 0x00);      /* flags LE */
    CHECK(buffer[8] == 0x88 && buffer[15] == 0x11);     /* frame_seq LE */
    CHECK(buffer[16] == 0x00 && buffer[23] == 0x99);    /* timestamp LE */
    CHECK(buffer[24] == 0xEF && buffer[25] == 0xBE &&
          buffer[26] == 0xAD && buffer[27] == 0xDE);    /* frame_size LE */
    CHECK(buffer[28] == 0x34 && buffer[29] == 0x12);    /* chunk_index */
    CHECK(buffer[30] == 0x78 && buffer[31] == 0x56);    /* chunk_count */
    CHECK(buffer[32] == 64 && buffer[33] == 0);         /* payload_size */
    CHECK(buffer[34] == 0xFE && buffer[35] == 0xCA);    /* fec_group */
    CHECK(buffer[36] == 0x7F && buffer[37] == 0x0F);    /* fec fields */
    CHECK(buffer[38] == 0 && buffer[39] == 0);          /* reserved */

    /* round trip of every field */
    CHECK(imt_wire_decode_header(buffer, sizeof(buffer), &decoded) == 0);
    CHECK(decoded.type == header.type);
    CHECK(decoded.flags == header.flags);
    CHECK(decoded.frame_seq == header.frame_seq);
    CHECK(decoded.capture_timestamp_ns == header.capture_timestamp_ns);
    CHECK(decoded.frame_size == header.frame_size);
    CHECK(decoded.chunk_index == header.chunk_index);
    CHECK(decoded.chunk_count == header.chunk_count);
    CHECK(decoded.payload_size == header.payload_size);
    CHECK(decoded.fec_group == header.fec_group);
    CHECK(decoded.fec_group_size == header.fec_group_size);
    CHECK(decoded.fec_index == header.fec_index);

    /* rejection (R6.5) */
    buffer[0] = 0x4Du; /* wrong magic */
    CHECK(imt_wire_decode_header(buffer, sizeof(buffer), &decoded) == -2);
    buffer[0] = 0x49u;
    buffer[2] = 2; /* wrong version */
    CHECK(imt_wire_decode_header(buffer, sizeof(buffer), &decoded) == -2);
    buffer[2] = 1;
    buffer[4] = 39; /* wrong header_size */
    CHECK(imt_wire_decode_header(buffer, sizeof(buffer), &decoded) == -2);
    buffer[4] = 40;
    CHECK(imt_wire_decode_header(buffer, sizeof(buffer), &decoded) == 0);
    CHECK(imt_wire_decode_header(buffer, IMT_WIRE_HEADER_SIZE - 1u, &decoded) == -1);
    CHECK(imt_wire_decode_header(buffer, IMT_WIRE_HEADER_SIZE + 10u, &decoded) == -3);

    /* FEEDBACK payload (R6.4) */
    memset(&feedback, 0, sizeof(feedback));
    memset(&feedback_decoded, 0, sizeof(feedback_decoded));
    feedback.latest_frame_seq = 0x0102030405060708ull;
    feedback.latest_capture_timestamp_ns = 0x1112131415161718ull;
    feedback.frame_age_ns = 0x2122232425262728ull;
    feedback.frames_completed = 1000;
    feedback.frames_incomplete = 7;
    CHECK(imt_wire_encode_feedback(&feedback, buffer, sizeof(buffer)) == 0);
    CHECK(IMT_WIRE_FEEDBACK_PAYLOAD_SIZE == 32);
    CHECK(buffer[0] == 0x08 && buffer[7] == 0x01);
    CHECK(buffer[8] == 0x18 && buffer[15] == 0x11);
    CHECK(buffer[16] == 0x28 && buffer[23] == 0x21);
    CHECK(buffer[24] == 0xE8 && buffer[25] == 0x03 &&
          buffer[26] == 0x00 && buffer[27] == 0x00);
    CHECK(buffer[28] == 0x07 && buffer[29] == 0x00 &&
          buffer[30] == 0x00 && buffer[31] == 0x00);
    CHECK(imt_wire_decode_feedback(buffer, IMT_WIRE_FEEDBACK_PAYLOAD_SIZE,
                                   &feedback_decoded) == 0);
    CHECK(feedback_decoded.latest_frame_seq == feedback.latest_frame_seq);
    CHECK(feedback_decoded.latest_capture_timestamp_ns ==
          feedback.latest_capture_timestamp_ns);
    CHECK(feedback_decoded.frame_age_ns == feedback.frame_age_ns);
    CHECK(feedback_decoded.frames_completed == feedback.frames_completed);
    CHECK(feedback_decoded.frames_incomplete == feedback.frames_incomplete);
    CHECK(imt_wire_decode_feedback(buffer, IMT_WIRE_FEEDBACK_PAYLOAD_SIZE - 1u,
                                   &feedback_decoded) < 0);

    /* MAP payload header (R6.3) */
    map_header.cols = 160;
    map_header.rows = 45;
    map_header.tile_size = 16;
    CHECK(imt_wire_encode_map_payload_header(&map_header, buffer, sizeof(buffer)) == 0);
    CHECK(buffer[6] == 0 && buffer[7] == 0); /* reserved */
    CHECK(imt_wire_decode_map_payload_header(buffer, IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE,
                                             &map_decoded) == 0);
    CHECK(map_decoded.cols == 160 && map_decoded.rows == 45 && map_decoded.tile_size == 16);
}

/* ------------------------------------------------------------------ */
/* imt_packetize geometry (R6.2, R5.2)                                */
/* ------------------------------------------------------------------ */

static void test_packetize_geometry(void)
{
    ImtPacketizer packetizer;
    uint8_t weights[14];
    int i;
    int slices;
    int parities;

    CHECK(imt_packetizer_init(&packetizer, 0, 0) == 0);
    CHECK(packetizer.max_payload == IMT_DEFAULT_MAX_PAYLOAD);

    /* NULL weights -> uniform group size 8; 16000 B -> 14 chunks */
    CHECK(imt_test_capture_frame(&packetizer, 1, NULL, 0) == 0);
    slices = 0;
    parities = 0;
    for (i = 0; i < g_capture.count; ++i) {
        const ImtWireHeader* h = &g_capture.headers[i];
        CHECK(h->frame_size == TEST_FRAME_SIZE);
        CHECK(h->chunk_count == 14);
        if (h->type == IMT_PACKET_TYPE_SLICE) {
            /* R6.2: all chunks full except the last */
            if (h->chunk_index == 13) {
                CHECK(h->payload_size == 400);
            } else {
                CHECK(h->payload_size == IMT_DEFAULT_MAX_PAYLOAD);
            }
            CHECK(h->fec_index == h->chunk_index % 8u);
            CHECK(h->fec_group == h->chunk_index / 8u);
            CHECK(h->fec_group_size == (h->chunk_index < 8u ? 8u : 6u));
            slices += 1;
        } else {
            CHECK(h->type == IMT_PACKET_TYPE_PARITY);
            CHECK(h->fec_index == IMT_FEC_INDEX_PARITY);
            CHECK(h->payload_size == IMT_DEFAULT_MAX_PAYLOAD);
            CHECK(h->chunk_index == (h->fec_group == 0u ? 0u : 8u));
            parities += 1;
        }
    }
    CHECK(slices == 14 && parities == 2);
    CHECK(g_capture.count == 16);

    /* weighted groups: w~ 200 -> size 2, 130 -> 4, 70 -> 8 (R5.2) */
    weights[0] = 200; weights[1] = 200;
    for (i = 2; i < 6; ++i) {
        weights[i] = 130;
    }
    for (i = 6; i < 14; ++i) {
        weights[i] = 70;
    }
    CHECK(imt_test_capture_frame(&packetizer, 2, weights, 14) == 0);
    parities = 0;
    for (i = 0; i < g_capture.count; ++i) {
        const ImtWireHeader* h = &g_capture.headers[i];
        if (h->type == IMT_PACKET_TYPE_PARITY) {
            parities += 1;
            continue;
        }
        if (h->chunk_index < 2u) {
            CHECK(h->fec_group == 0 && h->fec_group_size == 2);
        } else if (h->chunk_index < 6u) {
            CHECK(h->fec_group == 1 && h->fec_group_size == 4);
        } else {
            CHECK(h->fec_group == 2 && h->fec_group_size == 8);
        }
    }
    CHECK(parities == 3);
    CHECK(g_capture.count == 17);

    /* argument validation */
    CHECK(imt_packetizer_send_frame(&packetizer, g_frame, 0, 1, 0, 0, NULL, 0,
                                    imt_test_emit, &g_capture) < 0);
    CHECK(imt_packetizer_send_frame(&packetizer, g_frame, TEST_FRAME_SIZE, 1, 0, 0,
                                    weights, 5, imt_test_emit, &g_capture) < 0);

    imt_packetizer_destroy(&packetizer);
}

/* ------------------------------------------------------------------ */
/* fec / assembler loss-injection scenarios (R7, R9.1)                */
/* ------------------------------------------------------------------ */

static void test_assembler_no_loss(void)
{
    ImtPacketizer packetizer;
    ImtAssembler assembler;
    ImtFrameView view;

    CHECK(imt_packetizer_init(&packetizer, 0, 0) == 0);
    CHECK(imt_assembler_init(&assembler, TEST_MAX_FRAME, 0, 0) == 0);

    CHECK(imt_test_capture_frame(&packetizer, 1, NULL, 0) == 0);
    imt_test_deliver(&assembler, NULL);

    CHECK(assembler.latest.size == TEST_FRAME_SIZE);
    CHECK(assembler.latest.frame_seq == 1);
    CHECK(assembler.latest.capture_timestamp_ns == 1000);
    CHECK(memcmp(assembler.latest.data, g_frame, TEST_FRAME_SIZE) == 0);
    CHECK(assembler.frames_completed == 1);
    CHECK(assembler.chunks_recovered == 0);

    CHECK(imt_assembler_copy_latest(&assembler, g_copy, sizeof(g_copy), &view) == 0);
    CHECK(view.size == TEST_FRAME_SIZE && view.frame_seq == 1);
    CHECK(memcmp(g_copy, g_frame, TEST_FRAME_SIZE) == 0);

    imt_assembler_destroy(&assembler);
    imt_packetizer_destroy(&packetizer);
}

static void test_assembler_one_loss_per_group(void)
{
    ImtPacketizer packetizer;
    ImtAssembler assembler;
    uint8_t drop[TEST_MAX_DATAGRAMS];
    int target_by_group[16];
    uint32_t lcg = 12345u;
    int i;
    int dropped = 0;

    CHECK(imt_packetizer_init(&packetizer, 0, 0) == 0);
    CHECK(imt_assembler_init(&assembler, TEST_MAX_FRAME, 0, 0) == 0);

    CHECK(imt_test_capture_frame(&packetizer, 1, NULL, 0) == 0);

    /* drop one pseudo-random data chunk in every FEC group */
    memset(drop, 0, sizeof(drop));
    for (i = 0; i < 16; ++i) {
        target_by_group[i] = -1;
    }
    for (i = 0; i < g_capture.count; ++i) {
        const ImtWireHeader* h = &g_capture.headers[i];
        if (h->type == IMT_PACKET_TYPE_SLICE) {
            if (h->fec_group >= 16u) {
                CHECK(h->fec_group < 16u);
                continue;
            }
            if (target_by_group[h->fec_group] < 0) {
                target_by_group[h->fec_group] =
                    (int)(imt_test_lcg(&lcg) % h->fec_group_size);
            }
            if (h->fec_index == (uint8_t)target_by_group[h->fec_group]) {
                drop[i] = 1;
                dropped += 1;
            }
        }
    }
    CHECK(dropped == 2); /* one per group (two groups with uniform size 8) */

    imt_test_deliver(&assembler, drop);
    CHECK(assembler.latest.size == TEST_FRAME_SIZE);
    CHECK(assembler.latest.frame_seq == 1);
    CHECK(memcmp(assembler.latest.data, g_frame, TEST_FRAME_SIZE) == 0); /* R7.2 */
    CHECK(assembler.chunks_recovered == 2);

    imt_assembler_destroy(&assembler);
    imt_packetizer_destroy(&packetizer);
}

static void test_assembler_unrecoverable_then_supersede(void)
{
    ImtPacketizer packetizer;
    ImtAssembler assembler;
    uint8_t drop[TEST_MAX_DATAGRAMS];
    int i;

    CHECK(imt_packetizer_init(&packetizer, 0, 0) == 0);
    CHECK(imt_assembler_init(&assembler, TEST_MAX_FRAME, 0, 0) == 0);

    /* frame 1: drop two data chunks of group 0 -> unrecoverable (R7.3) */
    CHECK(imt_test_capture_frame(&packetizer, 1, NULL, 0) == 0);
    memset(drop, 0, sizeof(drop));
    for (i = 0; i < g_capture.count; ++i) {
        const ImtWireHeader* h = &g_capture.headers[i];
        if (h->type == IMT_PACKET_TYPE_SLICE && h->fec_group == 0u &&
            (h->fec_index == 1u || h->fec_index == 3u)) {
            drop[i] = 1;
        }
    }
    imt_test_deliver(&assembler, drop);
    CHECK(assembler.latest.size == 0);       /* nothing published */
    CHECK(assembler.frames_completed == 0);

    /* frame 2 arrives complete and supersedes the stuck frame 1 */
    CHECK(imt_test_capture_frame(&packetizer, 2, NULL, 0) == 0);
    imt_test_deliver(&assembler, NULL);
    CHECK(assembler.latest.size == TEST_FRAME_SIZE);
    CHECK(assembler.latest.frame_seq == 2);
    CHECK(memcmp(assembler.latest.data, g_frame, TEST_FRAME_SIZE) == 0);
    CHECK(assembler.frames_incomplete == 1); /* frame 1 was discarded */

    /* a late datagram from frame 1 is now stale and ignored */
    CHECK(imt_test_capture_frame(&packetizer, 1, NULL, 0) == 0);
    CHECK(imt_assembler_push_datagram(&assembler, g_capture.data[0],
                                      g_capture.sizes[0]) == 1);
    CHECK(assembler.drops_stale >= 1);

    imt_assembler_destroy(&assembler);
    imt_packetizer_destroy(&packetizer);
}

static void test_assembler_parity_only_loss(void)
{
    ImtPacketizer packetizer;
    ImtAssembler assembler;
    uint8_t drop[TEST_MAX_DATAGRAMS];
    int i;

    CHECK(imt_packetizer_init(&packetizer, 0, 0) == 0);
    CHECK(imt_assembler_init(&assembler, TEST_MAX_FRAME, 0, 0) == 0);

    CHECK(imt_test_capture_frame(&packetizer, 1, NULL, 0) == 0);
    memset(drop, 0, sizeof(drop));
    for (i = 0; i < g_capture.count; ++i) {
        if (g_capture.headers[i].type == IMT_PACKET_TYPE_PARITY) {
            drop[i] = 1;
        }
    }
    imt_test_deliver(&assembler, drop);
    CHECK(assembler.latest.size == TEST_FRAME_SIZE);
    CHECK(memcmp(assembler.latest.data, g_frame, TEST_FRAME_SIZE) == 0);
    CHECK(assembler.chunks_recovered == 0);

    imt_assembler_destroy(&assembler);
    imt_packetizer_destroy(&packetizer);
}

static void test_assembler_parity_first_delivery(void)
{
    ImtPacketizer packetizer;
    ImtAssembler assembler;
    uint8_t weights[14];
    uint8_t dropped_one_per_group[TEST_MAX_DATAGRAMS];
    int i;

    CHECK(imt_packetizer_init(&packetizer, 0, 0) == 0);
    CHECK(imt_assembler_init(&assembler, TEST_MAX_FRAME, 0, 0) == 0);

    /* mixed group sizes, parities delivered before any slice */
    weights[0] = 200; weights[1] = 200;
    for (i = 2; i < 6; ++i) {
        weights[i] = 130;
    }
    for (i = 6; i < 14; ++i) {
        weights[i] = 70;
    }
    CHECK(imt_test_capture_frame(&packetizer, 1, weights, 14) == 0);

    memset(dropped_one_per_group, 0, sizeof(dropped_one_per_group));
    for (i = 0; i < g_capture.count; ++i) {
        const ImtWireHeader* h = &g_capture.headers[i];
        if (h->type == IMT_PACKET_TYPE_SLICE && h->fec_index == 0u) {
            dropped_one_per_group[i] = 1; /* lose the first chunk of each group */
        }
    }

    for (i = 0; i < g_capture.count; ++i) { /* parities first */
        if (g_capture.headers[i].type == IMT_PACKET_TYPE_PARITY) {
            CHECK(imt_assembler_push_datagram(&assembler, g_capture.data[i],
                                              g_capture.sizes[i]) >= 0);
        }
    }
    for (i = 0; i < g_capture.count; ++i) {
        if (g_capture.headers[i].type == IMT_PACKET_TYPE_SLICE &&
            !dropped_one_per_group[i]) {
            CHECK(imt_assembler_push_datagram(&assembler, g_capture.data[i],
                                              g_capture.sizes[i]) >= 0);
        }
    }

    CHECK(assembler.latest.size == TEST_FRAME_SIZE);
    CHECK(memcmp(assembler.latest.data, g_frame, TEST_FRAME_SIZE) == 0);
    CHECK(assembler.chunks_recovered == 3); /* one per group */

    imt_assembler_destroy(&assembler);
    imt_packetizer_destroy(&packetizer);
}

static void test_assembler_short_tail_recovery(void)
{
    ImtPacketizer packetizer;
    ImtAssembler assembler;
    uint8_t weights[5] = { 200, 200, 200, 200, 200 }; /* group size 2 */
    uint8_t drop[TEST_MAX_DATAGRAMS];
    const uint32_t frame_size = 4u * IMT_DEFAULT_MAX_PAYLOAD + 100u;
    int i;

    CHECK(imt_packetizer_init(&packetizer, 0, 0) == 0);
    CHECK(imt_assembler_init(&assembler, TEST_MAX_FRAME, 0, 0) == 0);

    g_capture.count = 0;
    imt_test_fill_random(g_frame, frame_size, 777u);
    CHECK(imt_packetizer_send_frame(&packetizer, g_frame, frame_size, 1, 0, 0,
                                    weights, 5, imt_test_emit, &g_capture) == 0);

    /* groups: (0,1) (2,3) (4); the last group holds only the 100 B chunk,
     * so its parity is 100 B as well. Drop that chunk and recover it. */
    memset(drop, 0, sizeof(drop));
    for (i = 0; i < g_capture.count; ++i) {
        const ImtWireHeader* h = &g_capture.headers[i];
        if (h->type == IMT_PACKET_TYPE_SLICE && h->chunk_index == 4u) {
            CHECK(h->payload_size == 100);
            drop[i] = 1;
        }
        if (h->type == IMT_PACKET_TYPE_PARITY && h->fec_group == 2u) {
            CHECK(h->payload_size == 100);
            CHECK(h->fec_group_size == 1);
        }
    }

    imt_test_deliver(&assembler, drop);
    CHECK(assembler.latest.size == frame_size);
    CHECK(memcmp(assembler.latest.data, g_frame, frame_size) == 0);
    CHECK(assembler.chunks_recovered == 1);

    imt_assembler_destroy(&assembler);
    imt_packetizer_destroy(&packetizer);
}

static void test_assembler_map_fallback(void)
{
    ImtPacketizer packetizer;
    ImtAssembler assembler;
    ImtMap map;
    ImtMapView view;
    uint8_t pattern[7200];
    uint32_t i;

    CHECK(imt_packetizer_init(&packetizer, 0, 0) == 0);
    CHECK(imt_assembler_init(&assembler, TEST_MAX_FRAME, 0, 0) == 0);

    /* before any MAP datagram: uniform-128 fallback (R2.5) */
    CHECK(imt_assembler_get_map(&assembler, &view) == 1);
    CHECK(view.is_fallback == 1);
    CHECK(view.tile_count == IMT_DEFAULT_MAP_TILES);
    for (i = 0; i < view.tile_count; ++i) {
        if (view.weights[i] != 128) {
            break;
        }
    }
    CHECK(i == view.tile_count);

    /* deliver a MAP datagram */
    CHECK(imt_map_init(&map, 2560, 720, 16) == 0);
    imt_test_fill_random(pattern, sizeof(pattern), 31337u);
    for (i = 0; i < map.tile_count; ++i) {
        map.weights[i] = pattern[i] == 0u ? 1u : pattern[i];
    }
    g_capture.count = 0;
    CHECK(imt_packetizer_send_map(&packetizer, &map, 10, 0, imt_test_emit, &g_capture) == 0);
    CHECK(g_capture.count == 1);
    CHECK(g_capture.headers[0].type == IMT_PACKET_TYPE_MAP);
    CHECK(g_capture.sizes[0] == IMT_WIRE_HEADER_SIZE +
                                IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE + map.tile_count);
    CHECK(imt_assembler_push_datagram(&assembler, g_capture.data[0],
                                      g_capture.sizes[0]) == 1);
    CHECK(assembler.map_updates == 1);

    CHECK(imt_assembler_get_map(&assembler, &view) == 0);
    CHECK(view.is_fallback == 0);
    CHECK(view.cols == 160 && view.rows == 45 && view.tile_size == 16);
    CHECK(view.tile_count == 7200);
    CHECK(memcmp(view.weights, map.weights, map.tile_count) == 0);

    /* a newer MAP datagram is lost (never delivered): the previous map is
     * still served (R2.5 last-known-good) */
    CHECK(imt_assembler_get_map(&assembler, &view) == 0);
    CHECK(memcmp(view.weights, map.weights, map.tile_count) == 0);

    /* an out-of-date MAP (older frame_seq) does not overwrite the stored one */
    g_capture.count = 0;
    CHECK(imt_map_fill(&map, 50) == 0);
    CHECK(imt_packetizer_send_map(&packetizer, &map, 5, 0, imt_test_emit, &g_capture) == 0);
    CHECK(imt_assembler_push_datagram(&assembler, g_capture.data[0],
                                      g_capture.sizes[0]) == 1);
    CHECK(imt_assembler_get_map(&assembler, &view) == 0);
    CHECK(view.weights[0] != 50);

    imt_map_destroy(&map);
    imt_assembler_destroy(&assembler);
    imt_packetizer_destroy(&packetizer);
}

static void test_assembler_rejects_foreign_headers(void)
{
    ImtPacketizer packetizer;
    ImtAssembler assembler;
    uint8_t corrupted[TEST_MAX_DATAGRAM_SIZE];

    CHECK(imt_packetizer_init(&packetizer, 0, 0) == 0);
    CHECK(imt_assembler_init(&assembler, TEST_MAX_FRAME, 0, 0) == 0);
    CHECK(imt_test_capture_frame(&packetizer, 1, NULL, 0) == 0);

    /* wrong version: silently dropped and counted (R6.5) */
    memcpy(corrupted, g_capture.data[0], g_capture.sizes[0]);
    corrupted[2] = 9;
    CHECK(imt_assembler_push_datagram(&assembler, corrupted, g_capture.sizes[0]) == 1);
    CHECK(assembler.drops_bad_header == 1);

    /* wrong magic (e.g. legacy mpq traffic) */
    memcpy(corrupted, g_capture.data[0], g_capture.sizes[0]);
    corrupted[0] = 0x4Du;
    CHECK(imt_assembler_push_datagram(&assembler, corrupted, g_capture.sizes[0]) == 1);
    CHECK(assembler.drops_bad_header == 2);

    CHECK(assembler.latest.size == 0);
    imt_assembler_destroy(&assembler);
    imt_packetizer_destroy(&packetizer);
}

/* ------------------------------------------------------------------ */
/* R4.5 uniform-map degeneration                                      */
/* ------------------------------------------------------------------ */

static void test_uniform_degeneration(void)
{
    ImtMap map;
    ImtQpLut lut;
    static int8_t offsets[7200];
    static uint16_t order[7200];
    static uint8_t chunk_weights[14];
    ImtPacketizer packetizer;
    uint32_t i;
    int mean;

    CHECK(imt_map_init(&map, 2560, 720, 16) == 0);
    CHECK(imt_qp_lut_init(&lut) == 0);
    CHECK(imt_map_fill(&map, 128) == 0);

    /* uniform 128 -> mean 128 -> w~ 128 everywhere */
    mean = imt_map_mean(&map);
    CHECK(mean == 128);

    /* no ROI: every QP offset is 0 */
    CHECK(imt_qp_offsets_for_map(&lut, &map, offsets, 7200) == 0);
    for (i = 0; i < map.tile_count; ++i) {
        if (offsets[i] != 0) {
            break;
        }
    }
    CHECK(i == map.tile_count);

    /* no reordering: the stable sort leaves the identity order */
    CHECK(imt_sched_order(map.weights, map.tile_count, order) == 0);
    for (i = 0; i < map.tile_count; ++i) {
        if (order[i] != i) {
            break;
        }
    }
    CHECK(i == map.tile_count);

    /* uniform FEC: every group gets the same size (4 at w~ = 128, R5.2) */
    CHECK(imt_fec_group_size((uint32_t)imt_map_normalized_weight(128, (uint32_t)mean)) == 4);
    memset(chunk_weights, 128, sizeof(chunk_weights));
    CHECK(imt_packetizer_init(&packetizer, 0, 0) == 0);
    CHECK(imt_test_capture_frame(&packetizer, 1, chunk_weights, 14) == 0);
    for (i = 0; i < (uint32_t)g_capture.count; ++i) {
        const ImtWireHeader* h = &g_capture.headers[i];
        const uint8_t expected = (uint8_t)(h->fec_group == 3u ? 2u : 4u); /* 14 = 3*4 + 2 */
        if (h->fec_group_size != expected) {
            CHECK(h->fec_group_size == expected);
            break;
        }
    }
    CHECK(i == (uint32_t)g_capture.count);

    imt_packetizer_destroy(&packetizer);
    imt_map_destroy(&map);
}

/* ------------------------------------------------------------------ */
/* imt_pace (R8.4)                                                    */
/* ------------------------------------------------------------------ */

static void test_pace(void)
{
    ImtPace pace;
    const int64_t target_ns = 15000000;  /* T = 15 ms */
    const int64_t base_ns = 20000000;    /* plant: L = L0 * r, L0 = 20 ms */
    const int32_t expected_q16 = (int32_t)((target_ns * IMT_PACE_Q16_ONE) / base_ns);
    const int32_t band_q16 = expected_q16 / 20; /* +/-5 % */
    int32_t rate;
    int in_band_after_100 = 1;
    int i;

    CHECK(imt_pace_init(&pace, target_ns) == 0);
    CHECK(imt_pace_rate_q16(&pace) == IMT_PACE_Q16_ONE);
    CHECK(imt_pace_init(NULL, target_ns) < 0);
    CHECK(imt_pace_init(&pace, 0) < 0);
    CHECK(imt_pace_init(&pace, target_ns) == 0);

    rate = imt_pace_rate_q16(&pace);
    for (i = 0; i < 500; ++i) {
        const int64_t latency_ns = (base_ns * rate) / IMT_PACE_Q16_ONE;
        const int rc = imt_pace_update(&pace, latency_ns);
        CHECK(rc > 0);
        rate = (int32_t)rc;
        CHECK(rate >= IMT_PACE_RATE_MIN_Q16 && rate <= IMT_PACE_RATE_MAX_Q16);
        if (rate < IMT_PACE_RATE_MIN_Q16 || rate > IMT_PACE_RATE_MAX_Q16) {
            break;
        }
        if (i >= 100 && (rate < expected_q16 - band_q16 || rate > expected_q16 + band_q16)) {
            in_band_after_100 = 0;
        }
    }
    /* converges to the analytic fixed point r* = T/base within +/-5 % and
     * stays there (no sustained oscillation, R8.4) */
    CHECK(in_band_after_100 == 1);
    CHECK(rate >= expected_q16 - band_q16 && rate <= expected_q16 + band_q16);

    /* feedback-timeout decay toward 1.0 at 0.1/s (R8.3) */
    {
        const int32_t before = rate;
        const int decayed = imt_pace_idle(&pace, 1000000000); /* 1 s */
        CHECK(decayed > before);
        CHECK(decayed - before <= 6554 + 1);
        CHECK(imt_pace_idle(&pace, 20000000000) == IMT_PACE_Q16_ONE); /* 20 s */
        CHECK(imt_pace_rate_q16(&pace) == IMT_PACE_Q16_ONE);
    }

    /* decay also works from above 1.0 */
    CHECK(imt_pace_init(&pace, target_ns) == 0);
    CHECK(imt_pace_update(&pace, 0) > IMT_PACE_Q16_ONE); /* L = 0 -> speed up */
    {
        const int32_t high = imt_pace_rate_q16(&pace);
        const int decayed = imt_pace_idle(&pace, 1000000000);
        CHECK(decayed < high && decayed >= IMT_PACE_Q16_ONE);
        CHECK(imt_pace_idle(&pace, 20000000000) == IMT_PACE_Q16_ONE);
    }

    CHECK(imt_pace_update(NULL, 1000) < 0);
    CHECK(imt_pace_update(&pace, -1) < 0);
    CHECK(imt_pace_idle(&pace, -1) < 0);
}

/* ------------------------------------------------------------------ */
/* Micro-benchmark (informative, not pass/fail; R9.3)                 */
/* ------------------------------------------------------------------ */

static void bench_steady_state(void)
{
    ImtMap map;
    ImtQpLut lut;
    static uint8_t samples[7200];
    static int8_t offsets[7200];
    static uint16_t order[7200];
    uint64_t begin_ns;
    uint64_t elapsed_ns;
    uint32_t checksum = 0;
    int mean = 0;
    int i;

    if (imt_map_init(&map, 2560, 720, 16) != 0 || imt_qp_lut_init(&lut) != 0) {
        printf("bench: setup failed\n");
        return;
    }
    imt_test_fill_random(samples, sizeof(samples), 2026u);

    begin_ns = imt_test_now_ns();
    for (i = 0; i < 1000; ++i) {
        (void)imt_map_update_ema(&map, samples, map.tile_count);
        (void)imt_qp_offsets_for_map(&lut, &map, offsets, 7200);
        (void)imt_sched_order(map.weights, map.tile_count, order);
        checksum += (uint32_t)order[0] + (uint32_t)(uint8_t)offsets[0];
    }
    elapsed_ns = imt_test_now_ns() - begin_ns;
    mean = imt_map_mean(&map);

    printf("bench: EMA+mean+QP+sched on 160x45, 1000 iterations: "
           "%llu ns total, %llu ns/iteration (mean %d, checksum %u)\n",
           (unsigned long long)elapsed_ns,
           (unsigned long long)(elapsed_ns / 1000u),
           mean, (unsigned)checksum);

    imt_map_destroy(&map);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_map();
    test_qp();
    test_sched();
    test_fec();
    test_wire();
    test_packetize_geometry();
    test_assembler_no_loss();
    test_assembler_one_loss_per_group();
    test_assembler_unrecoverable_then_supersede();
    test_assembler_parity_only_loss();
    test_assembler_parity_first_delivery();
    test_assembler_short_tail_recovery();
    test_assembler_map_fallback();
    test_assembler_rejects_foreign_headers();
    test_uniform_degeneration();
    test_pace();
    bench_steady_state();

    printf("imt_tests: %d passed, %d failed\n", g_checks_passed, g_checks_failed);
    return g_checks_failed == 0 ? 0 : 1;
}
