#ifndef IMT_H
#define IMT_H

/*
 * imt - Importance-Map Transport core library.
 *
 * Pure C99, codec/OS agnostic (R2.1). Normative specification:
 *   docs/imt_requirements.md (rp-stereocam repository).
 *
 * Conventions (matching mp_* / mpq_* modules):
 *   - return codes: 0 = success, negative = error, positive = informational,
 *     except functions explicitly documented as value-returning.
 *   - heap allocation only in *_init, release only in *_destroy (R2.2).
 *   - steady-state paths are integer + LUT only; floating point is used
 *     exclusively during *_init (R2.3).
 *   - wire serialization is explicit little-endian, never struct memcpy (R2.4).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Wire constants (R6.1)                                              */
/* ------------------------------------------------------------------ */

#define IMT_WIRE_MAGIC0 0x49u /* 'I' */
#define IMT_WIRE_MAGIC1 0x4Du /* 'M' */
#define IMT_WIRE_VERSION 1u
#define IMT_WIRE_HEADER_SIZE 40u

#define IMT_PACKET_TYPE_SLICE 0u
#define IMT_PACKET_TYPE_MAP 1u
#define IMT_PACKET_TYPE_PARITY 2u
#define IMT_PACKET_TYPE_FEEDBACK 3u

#define IMT_PACKET_FLAG_KEYFRAME (1u << 0)
#define IMT_PACKET_FLAG_CODEC_CONFIG (1u << 1)

/* fec_index value that marks a parity packet (R6.1). */
#define IMT_FEC_INDEX_PARITY 0xFFu

/* Default maximum datagram payload in bytes (R6.2). */
#define IMT_DEFAULT_MAX_PAYLOAD 1200u

/* Default importance-map capacity in tiles (160x45, R4.1 example). */
#define IMT_DEFAULT_MAP_TILES 7200u

/* MAP payload prefix: cols(u16) rows(u16) tile_size(u16) reserved(u16) (R6.3). */
#define IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE 8u

/* FEEDBACK payload size (R6.4). */
#define IMT_WIRE_FEEDBACK_PAYLOAD_SIZE 32u

/* ------------------------------------------------------------------ */
/* imt_map - importance-map grid, EMA smoothing, normalization (R4)   */
/* ------------------------------------------------------------------ */

typedef struct ImtMap {
    uint8_t* weights;   /* cols * rows tile weights, row-major */
    uint32_t tile_count;
    uint16_t cols;
    uint16_t rows;
    uint16_t tile_size;
    uint8_t ema_shift;  /* R4.3 smoothing shift s, default 3 */
} ImtMap;

/* Computes cols = ceil(width/tile_size), rows = ceil(height/tile_size) (R4.1),
 * allocates the weight grid and fills it with 128. */
int imt_map_init(ImtMap* map, uint32_t frame_width, uint32_t frame_height,
                 uint16_t tile_size);
void imt_map_destroy(ImtMap* map);

/* Sets every tile weight to `weight` (0 is stored as 1, R4.2). */
int imt_map_fill(ImtMap* map, uint8_t weight);

/* Integer EMA update per R4.3: w <- (w*(2^s - 1) + w_new) >> s.
 * sample_count must equal map->tile_count. Sample value 0 is treated as 1. */
int imt_map_update_ema(ImtMap* map, const uint8_t* samples, uint32_t sample_count);

/* Returns the integer mean weight (>= 1), or a negative error code.
 * Weight 0 is treated as 1 (R4.2). */
int imt_map_mean(const ImtMap* map);

/* Value-returning helper: normalized weight per R5.1,
 * w~ = clamp(1, weight*128/mean, 255). weight 0 and mean 0 are treated as 1. */
int imt_map_normalized_weight(uint32_t weight, uint32_t mean);

/* ------------------------------------------------------------------ */
/* imt_qp - weight to QP-offset LUT (R5.1)                            */
/* ------------------------------------------------------------------ */

typedef struct ImtQpLut {
    int8_t dqp[256]; /* indexed by normalized weight w~; index 0 == index 1 */
} ImtQpLut;

/* Builds the LUT once: dqp(w~) = round(-6*log2(w~/128)) clamped to [-6, +12].
 * The only floating-point code in the library (init-time, R2.3). */
int imt_qp_lut_init(ImtQpLut* lut);

/* Value-returning: QP offset for one raw weight given the map mean.
 * Returns a value in [-6, +12]; returns 0 if lut is NULL. */
int imt_qp_offset(const ImtQpLut* lut, uint8_t weight, uint32_t mean);

/* Fills dst[0..tile_count) with per-tile QP offsets for the whole map.
 * dst_count must be >= map->tile_count. */
int imt_qp_offsets_for_map(const ImtQpLut* lut, const ImtMap* map,
                           int8_t* dst, size_t dst_count);

/* ------------------------------------------------------------------ */
/* imt_sched - descending-weight transmit order (R5.3)                */
/* ------------------------------------------------------------------ */

/* Stable counting sort (256 buckets, O(n)): writes indices 0..count-1 into
 * out_order so that weights[out_order[i]] is non-increasing; ties keep their
 * original (ascending index) order. count must be <= 65536. */
int imt_sched_order(const uint8_t* weights, uint32_t count, uint16_t* out_order);

/* ------------------------------------------------------------------ */
/* imt_fec - XOR parity helpers (R5.2, R7)                            */
/* ------------------------------------------------------------------ */

/* Value-returning: FEC group size from a normalized weight w~ (R5.2):
 * w~>=192 -> 2, w~>=128 -> 4, w~>=64 -> 8, otherwise 16. 0 is treated as 1. */
int imt_fec_group_size(uint32_t normalized_weight);

/* parity[i] ^= data[i] for i < data_size; bytes beyond data_size are left
 * untouched, which implements zero-extension of short payloads (R7.1).
 * data_size must be <= parity_size. */
int imt_fec_xor_accumulate(uint8_t* parity, size_t parity_size,
                           const uint8_t* data, size_t data_size);

/* ------------------------------------------------------------------ */
/* imt_wire - datagram header / payload serialization (R6)            */
/* ------------------------------------------------------------------ */

/* In-memory image of the 40-byte wire header (R6.1). magic, version,
 * header_size and reserved are constants handled by encode/decode. */
typedef struct ImtWireHeader {
    uint8_t type;                  /* IMT_PACKET_TYPE_* */
    uint16_t flags;                /* IMT_PACKET_FLAG_* */
    uint64_t frame_seq;
    uint64_t capture_timestamp_ns;
    uint32_t frame_size;
    uint16_t chunk_index;          /* for PARITY: first data chunk of group */
    uint16_t chunk_count;
    uint16_t payload_size;
    uint16_t fec_group;
    uint8_t fec_group_size;        /* data packets in the group */
    uint8_t fec_index;             /* 0xFF = parity */
} ImtWireHeader;

typedef struct ImtFeedback {
    uint64_t latest_frame_seq;
    uint64_t latest_capture_timestamp_ns;
    uint64_t frame_age_ns;
    uint32_t frames_completed;
    uint32_t frames_incomplete;
} ImtFeedback;

typedef struct ImtMapPayloadHeader {
    uint16_t cols;
    uint16_t rows;
    uint16_t tile_size;
} ImtMapPayloadHeader;

/* Writes the 40-byte header (little-endian) into dst. dst_size >= 40. */
int imt_wire_encode_header(const ImtWireHeader* header, uint8_t* dst, size_t dst_size);

/* Parses and validates a 40-byte header. Returns:
 *   0 success, -1 bad arguments / short buffer,
 *  -2 magic, version or header_size mismatch (R6.5),
 *  -3 payload_size inconsistent with the datagram size. */
int imt_wire_decode_header(const uint8_t* data, size_t size, ImtWireHeader* out_header);

/* FEEDBACK payload (32 bytes, R6.4). */
int imt_wire_encode_feedback(const ImtFeedback* feedback, uint8_t* dst, size_t dst_size);
int imt_wire_decode_feedback(const uint8_t* data, size_t size, ImtFeedback* out_feedback);

/* MAP payload prefix (8 bytes, R6.3); weights follow immediately after. */
int imt_wire_encode_map_payload_header(const ImtMapPayloadHeader* header,
                                       uint8_t* dst, size_t dst_size);
int imt_wire_decode_map_payload_header(const uint8_t* data, size_t size,
                                       ImtMapPayloadHeader* out_header);

/* ------------------------------------------------------------------ */
/* imt_packetize - sender-side frame -> datagram sequence (R6.2, R7)  */
/* ------------------------------------------------------------------ */

typedef struct ImtPacketizer {
    uint8_t* datagram;     /* header + payload scratch */
    uint8_t* parity;       /* XOR accumulator, max_payload bytes */
    size_t datagram_capacity;
    size_t max_payload;
    uint32_t max_map_tiles;
} ImtPacketizer;

/* max_payload 0 selects IMT_DEFAULT_MAX_PAYLOAD; max_map_tiles 0 selects
 * IMT_DEFAULT_MAP_TILES. */
int imt_packetizer_init(ImtPacketizer* packetizer, size_t max_payload,
                        uint32_t max_map_tiles);
void imt_packetizer_destroy(ImtPacketizer* packetizer);

/* Splits the frame into chunks of max_payload (all full except the last,
 * R6.2), groups consecutive chunks into XOR-FEC groups and emits one SLICE
 * datagram per chunk followed by one PARITY datagram per group (R7.1).
 *
 * chunk_weights_norm: optional per-chunk *normalized* weights (w~); the group
 * size is imt_fec_group_size() of the first chunk of each group. NULL selects
 * a uniform group size of 8. When non-NULL it must hold one entry per chunk.
 *
 * emit is called once per datagram with a buffer owned by the packetizer
 * (valid only during the call). emit must return 0 to continue; any nonzero
 * value aborts and is propagated as the (negative) return value. */
int imt_packetizer_send_frame(ImtPacketizer* packetizer,
                              const uint8_t* frame,
                              uint32_t frame_size,
                              uint64_t frame_seq,
                              uint64_t capture_timestamp_ns,
                              uint16_t flags,
                              const uint8_t* chunk_weights_norm,
                              uint32_t chunk_weight_count,
                              int (*emit)(const uint8_t* datagram, size_t len, void* user),
                              void* user);

/* Emits one MAP datagram carrying the whole grid (single datagram, R6.3). */
int imt_packetizer_send_map(ImtPacketizer* packetizer,
                            const ImtMap* map,
                            uint64_t frame_seq,
                            uint64_t capture_timestamp_ns,
                            int (*emit)(const uint8_t* datagram, size_t len, void* user),
                            void* user);

/* ------------------------------------------------------------------ */
/* imt_assembler - receiver-side reconstruction + FEC recovery (R7)   */
/* ------------------------------------------------------------------ */

typedef struct ImtAssemblerGroup {
    uint8_t* parity;             /* max_payload bytes */
    uint16_t base_index;         /* first data chunk index of the group */
    uint16_t parity_payload_size;
    uint8_t size;                /* data chunks in the group */
    uint8_t parity_received;
    uint8_t known;
} ImtAssemblerGroup;

typedef struct ImtFrameView {
    uint8_t* data;
    size_t size;
    size_t capacity;
    uint64_t frame_seq;
    uint64_t capture_timestamp_ns;
    uint8_t keyframe;
    uint8_t codec_config;
} ImtFrameView;

typedef struct ImtMapView {
    const uint8_t* weights;
    uint32_t tile_count;
    uint16_t cols;       /* 0 when is_fallback and no MAP ever received */
    uint16_t rows;
    uint16_t tile_size;
    uint8_t is_fallback; /* 1 = uniform-128 fallback (R2.5) */
} ImtMapView;

typedef struct ImtAssembler {
    uint8_t* staging;
    uint8_t* received_chunks;        /* one byte per chunk, 0/1 */
    ImtAssemblerGroup* groups;
    uint8_t* group_parity_block;
    uint8_t* map_weights;
    size_t staging_capacity;
    size_t max_payload;
    uint32_t max_chunks;
    uint32_t max_groups;
    uint32_t map_capacity;
    /* active (in-flight) frame */
    size_t staging_size;
    uint64_t active_frame_seq;
    uint64_t active_capture_timestamp_ns;
    uint32_t active_chunk_count;
    uint32_t active_chunks_received;
    uint32_t active_group_limit;     /* groups reset for the active frame */
    uint8_t active_valid;
    uint8_t active_keyframe;
    uint8_t active_codec_config;
    /* last-known-good importance map (R2.5) */
    uint64_t map_frame_seq;
    uint32_t map_tile_count;
    uint16_t map_cols;
    uint16_t map_rows;
    uint16_t map_tile_size;
    uint8_t map_valid;
    /* statistics */
    uint64_t frames_completed;
    uint64_t frames_incomplete;      /* superseded while incomplete (R7.3) */
    uint64_t chunks_recovered;       /* data chunks rebuilt from parity */
    uint64_t drops_bad_header;       /* magic/version/header_size (R6.5) */
    uint64_t drops_stale;
    uint64_t map_updates;
    ImtFrameView latest;
} ImtAssembler;

/* Allocates all buffers up front: staging + published frame (max_frame_size
 * each), chunk bookkeeping and one parity buffer per possible group.
 * max_payload 0 selects IMT_DEFAULT_MAX_PAYLOAD; max_map_tiles 0 selects
 * IMT_DEFAULT_MAP_TILES. */
int imt_assembler_init(ImtAssembler* assembler, size_t max_frame_size,
                       size_t max_payload, uint32_t max_map_tiles);
void imt_assembler_destroy(ImtAssembler* assembler);

/* Feeds one received datagram (header + payload). Returns:
 *   0  a frame was completed and published to `latest`,
 *   1  accepted / ignored without completing a frame (includes MAP updates,
 *      stale packets and silently discarded foreign headers per R6.5),
 *   negative on argument or protocol errors. */
int imt_assembler_push_datagram(ImtAssembler* assembler,
                                const uint8_t* datagram, size_t size);

/* Returns the last-known-good importance map, or the uniform-128 fallback
 * when no MAP datagram has ever been received (R2.5).
 * Returns 0 (stored map) or 1 (fallback view). */
int imt_assembler_get_map(const ImtAssembler* assembler, ImtMapView* out_view);

/* Copies the most recently completed frame, like mpq_frame_assembler. */
int imt_assembler_copy_latest(ImtAssembler* assembler, uint8_t* dst,
                              size_t dst_capacity, ImtFrameView* out_frame);

/* ------------------------------------------------------------------ */
/* imt_pace - PI latency-budget controller (R8)                       */
/* ------------------------------------------------------------------ */

#define IMT_PACE_Q16_ONE 65536
#define IMT_PACE_RATE_MIN_Q16 6554    /* 0.1 */
#define IMT_PACE_RATE_MAX_Q16 131072  /* 2.0 */
#define IMT_PACE_DEFAULT_KP_Q16 26214 /* 0.4 */
#define IMT_PACE_DEFAULT_KI_Q16 3277  /* 0.05 */

typedef struct ImtPace {
    int64_t target_latency_ns;  /* T */
    int64_t integ_q16;          /* integrator, anti-windup clamped */
    int64_t integ_min_q16;
    int64_t integ_max_q16;
    int32_t kp_q16;
    int32_t ki_q16;
    int32_t rate_q16;           /* current output r, Q16 */
} ImtPace;

/* No heap allocation. rate starts at 1.0, gains at the R8.2 defaults. */
int imt_pace_init(ImtPace* pace, int64_t target_latency_ns);

/* One PI step with a measured frame age (R8.1/R8.2):
 * e = (T - L)/T in Q16, integ += e (clamped), r = 1 + kp*e + ki*integ,
 * r clamped to [0.1, 2.0]. Value-returning: the new rate in Q16
 * (always positive); returns a negative error code on bad arguments. */
int imt_pace_update(ImtPace* pace, int64_t measured_latency_ns);

/* Feedback-timeout decay (R8.3): moves r linearly toward 1.0 by 0.1 (10%)
 * per second of `elapsed_ns`. The caller supplies elapsed time; the library
 * never reads a clock. Value-returning like imt_pace_update. */
int imt_pace_idle(ImtPace* pace, int64_t elapsed_ns);

/* Value-returning accessor for the current rate in Q16. */
int32_t imt_pace_rate_q16(const ImtPace* pace);

#ifdef __cplusplus
}
#endif

#endif
