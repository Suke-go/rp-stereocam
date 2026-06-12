#include "imt.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TRACE_DEFAULT_SEED 1u
#define TRACE_PPM_SCALE 1000000u
#define TRACE_DEFAULT_FRAME_SIZE 17000u
#define TRACE_PERF_FRAMES 10000u

typedef struct TraceFrame {
    uint64_t frame_seq;
    uint32_t size_bytes;
    uint64_t capture_ts_ns;
} TraceFrame;

typedef struct TraceList {
    TraceFrame* frames;
    size_t count;
    size_t capacity;
} TraceList;

typedef enum LossKind {
    LOSS_NONE = 0,
    LOSS_IID,
    LOSS_GE,
    LOSS_TRACE,
    LOSS_ONE_PER_GROUP
} LossKind;

typedef struct LossTraceList {
    uint64_t* packet_ids;
    size_t count;
    size_t capacity;
} LossTraceList;

typedef struct LossModel {
    LossKind kind;
    uint32_t seed;
    uint32_t state;
    uint32_t iid_ppm;
    uint32_t ge_p_ppm;
    uint32_t ge_r_ppm;
    uint32_t ge_h_ppm;
    uint32_t ge_k_ppm;
    int ge_bad;
    LossTraceList trace;
    size_t trace_pos;
} LossModel;

typedef enum WeightKind {
    WEIGHT_UNIFORM = 0,
    WEIGHT_TWOLEVEL,
    WEIGHT_MAP_FILE
} WeightKind;

typedef struct WeightConfig {
    WeightKind kind;
    uint8_t uniform_weight;
    uint8_t high_weight;
    uint8_t low_weight;
    uint8_t fraction_q8;
    uint8_t* map_weights_norm;
    uint32_t map_tile_count;
} WeightConfig;

typedef struct ReplayOptions {
    LossModel loss;
    WeightConfig weights;
    size_t max_payload;
    uint32_t seed;
} ReplayOptions;

typedef struct ReplayStats {
    uint64_t frames;
    uint64_t frames_completed;
    uint64_t frames_bit_exact;
    uint64_t recovered_frames;
    uint64_t chunks_recovered;
    uint64_t packets_data;
    uint64_t packets_parity;
    uint64_t packets_lost;
    uint64_t data_bytes;
    uint64_t parity_bytes;
    uint64_t delivered_payload_bytes;
    uint64_t completed_bytes;
} ReplayStats;

typedef struct ReplayOutput {
    FILE* file;
    char* buffer;
    size_t length;
    size_t capacity;
    int enabled;
} ReplayOutput;

typedef struct EmitContext {
    ImtAssembler* assembler;
    LossModel* loss;
    uint64_t* packet_seq;
    uint32_t packets_data;
    uint32_t packets_parity;
    uint32_t packets_lost;
    uint32_t parity_payload_bytes;
    uint32_t delivered_payload_bytes;
    int error;
} EmitContext;

static uint32_t trace_lcg_next(uint32_t* state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static uint32_t trace_lcg_ppm(uint32_t* state)
{
    return trace_lcg_next(state) % TRACE_PPM_SCALE;
}

static uint32_t trace_seed_for_frame(uint32_t seed, uint64_t frame_seq)
{
    uint32_t state = seed ^ (uint32_t)frame_seq ^
        (uint32_t)(frame_seq >> 32) ^ UINT32_C(0x9E3779B9);
    if (state == 0u) {
        state = UINT32_C(0xA341316C);
    }
    return state;
}

static void trace_fill_frame(uint8_t* dst, size_t size, uint32_t seed, uint64_t frame_seq)
{
    size_t i;
    uint32_t state = trace_seed_for_frame(seed, frame_seq);

    for (i = 0; i < size; ++i) {
        dst[i] = (uint8_t)(trace_lcg_next(&state) >> 24);
    }
}

static int trace_append_frame(TraceList* list, TraceFrame frame)
{
    TraceFrame* grown;
    size_t new_capacity;

    if (list->count == list->capacity) {
        new_capacity = list->capacity == 0u ? 128u : list->capacity * 2u;
        grown = (TraceFrame*)realloc(list->frames, new_capacity * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        list->frames = grown;
        list->capacity = new_capacity;
    }
    list->frames[list->count] = frame;
    list->count += 1u;
    return 0;
}

static void trace_list_destroy(TraceList* list)
{
    free(list->frames);
    list->frames = NULL;
    list->count = 0u;
    list->capacity = 0u;
}

static int trace_append_loss_id(LossTraceList* list, uint64_t packet_id)
{
    uint64_t* grown;
    size_t new_capacity;

    if (list->count == list->capacity) {
        new_capacity = list->capacity == 0u ? 128u : list->capacity * 2u;
        grown = (uint64_t*)realloc(list->packet_ids, new_capacity * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        list->packet_ids = grown;
        list->capacity = new_capacity;
    }
    list->packet_ids[list->count] = packet_id;
    list->count += 1u;
    return 0;
}

static void trace_loss_list_destroy(LossTraceList* list)
{
    free(list->packet_ids);
    list->packet_ids = NULL;
    list->count = 0u;
    list->capacity = 0u;
}

static int trace_u64_compare(const void* a, const void* b)
{
    const uint64_t va = *(const uint64_t*)a;
    const uint64_t vb = *(const uint64_t*)b;
    return va < vb ? -1 : (va > vb ? 1 : 0);
}

static uint16_t trace_read_u16_le(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint8_t trace_min_one_u8(unsigned long value)
{
    if (value == 0ul) {
        return 1u;
    }
    if (value > 255ul) {
        return 255u;
    }
    return (uint8_t)value;
}

static int trace_output_write(ReplayOutput* out, const char* text, size_t text_len)
{
    char* grown;
    size_t new_capacity;

    if (!out || !out->enabled) {
        return 0;
    }
    if (out->file) {
        return fwrite(text, 1u, text_len, out->file) == text_len ? 0 : -1;
    }
    if (out->length + text_len + 1u > out->capacity) {
        new_capacity = out->capacity == 0u ? 4096u : out->capacity;
        while (new_capacity < out->length + text_len + 1u) {
            new_capacity *= 2u;
        }
        grown = (char*)realloc(out->buffer, new_capacity);
        if (!grown) {
            return -1;
        }
        out->buffer = grown;
        out->capacity = new_capacity;
    }
    memcpy(out->buffer + out->length, text, text_len);
    out->length += text_len;
    out->buffer[out->length] = '\0';
    return 0;
}

static int trace_output_printf(ReplayOutput* out, const char* fmt, ...)
{
    char stack[512];
    char* heap = NULL;
    va_list args;
    va_list args_copy;
    int needed;
    int rc;

    if (!out || !out->enabled) {
        return 0;
    }

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(stack, sizeof(stack), fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return -1;
    }
    if ((size_t)needed < sizeof(stack)) {
        va_end(args_copy);
        return trace_output_write(out, stack, (size_t)needed);
    }

    heap = (char*)malloc((size_t)needed + 1u);
    if (!heap) {
        va_end(args_copy);
        return -1;
    }
    rc = vsnprintf(heap, (size_t)needed + 1u, fmt, args_copy);
    va_end(args_copy);
    if (rc != needed) {
        free(heap);
        return -1;
    }
    rc = trace_output_write(out, heap, (size_t)needed);
    free(heap);
    return rc;
}

static void trace_output_destroy(ReplayOutput* out)
{
    free(out->buffer);
    out->buffer = NULL;
    out->length = 0u;
    out->capacity = 0u;
}

static char* trace_trim_left(char* s)
{
    while (*s != '\0' && isspace((unsigned char)*s)) {
        ++s;
    }
    return s;
}

static int trace_parse_u64_field(char** cursor, uint64_t* value, int require_comma)
{
    char* end;
    char* p = trace_trim_left(*cursor);

    if (*p == '\0') {
        return -1;
    }
    *value = strtoull(p, &end, 10);
    if (end == p) {
        return -1;
    }
    end = trace_trim_left(end);
    if (require_comma) {
        if (*end != ',') {
            return -1;
        }
        end += 1;
    }
    *cursor = end;
    return 0;
}

static int trace_load_csv(const char* path, TraceList* list)
{
    FILE* f = fopen(path, "r");
    char line[512];
    uint32_t line_no = 0u;

    if (!f) {
        fprintf(stderr, "error: failed to open trace CSV: %s\n", path);
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        TraceFrame frame;
        uint64_t seq;
        uint64_t size;
        uint64_t ts;
        char* p;

        line_no += 1u;
        p = trace_trim_left(line);
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') {
            continue;
        }
        if (!isdigit((unsigned char)*p)) {
            continue; /* allow a header row */
        }
        if (trace_parse_u64_field(&p, &seq, 1) != 0 ||
            trace_parse_u64_field(&p, &size, 1) != 0 ||
            trace_parse_u64_field(&p, &ts, 0) != 0 ||
            size == 0u || size > UINT32_MAX) {
            fprintf(stderr, "error: bad CSV line %u\n", (unsigned)line_no);
            fclose(f);
            return -1;
        }
        frame.frame_seq = seq;
        frame.size_bytes = (uint32_t)size;
        frame.capture_ts_ns = ts;
        if (trace_append_frame(list, frame) != 0) {
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    if (list->count == 0u) {
        fprintf(stderr, "error: trace CSV has no frames\n");
        return -1;
    }
    return 0;
}

static int trace_make_synthetic(TraceList* list, uint32_t frame_count, uint32_t frame_size)
{
    uint32_t i;

    for (i = 0; i < frame_count; ++i) {
        TraceFrame frame;
        frame.frame_seq = (uint64_t)i + 1u;
        frame.size_bytes = frame_size;
        frame.capture_ts_ns = (uint64_t)i * UINT64_C(16666667);
        if (trace_append_frame(list, frame) != 0) {
            return -1;
        }
    }
    return 0;
}

static int trace_load_loss_trace(const char* path, LossTraceList* list)
{
    FILE* f = fopen(path, "r");
    char line[256];

    if (!f) {
        fprintf(stderr, "error: failed to open loss trace: %s\n", path);
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char* p = trace_trim_left(line);
        char* end;
        uint64_t packet_id;

        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') {
            continue;
        }
        packet_id = strtoull(p, &end, 10);
        if (end == p) {
            fclose(f);
            return -1;
        }
        if (trace_append_loss_id(list, packet_id) != 0) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    if (list->count > 1u) {
        qsort(list->packet_ids, list->count, sizeof(list->packet_ids[0]), trace_u64_compare);
    }
    return 0;
}

static int trace_parse_ppm(const char* text, uint32_t* out)
{
    char* end;
    unsigned long value = strtoul(text, &end, 10);

    if (end == text || *end != '\0' || value > TRACE_PPM_SCALE) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

static int trace_parse_uint_csv(const char* text,
                                unsigned long* values,
                                int value_count,
                                unsigned long max_value)
{
    const char* p = text;
    int i;

    for (i = 0; i < value_count; ++i) {
        char* end;

        if (*p == '\0') {
            return -1;
        }
        values[i] = strtoul(p, &end, 10);
        if (end == p || values[i] > max_value) {
            return -1;
        }
        if (i + 1 < value_count) {
            if (*end != ',') {
                return -1;
            }
            p = end + 1;
        } else if (*end != '\0') {
            return -1;
        }
    }
    return 0;
}

static int trace_parse_loss_ge(const char* text, LossModel* loss)
{
    unsigned long values[4];

    if (trace_parse_uint_csv(text, values, 4, TRACE_PPM_SCALE) != 0) {
        return -1;
    }
    loss->kind = LOSS_GE;
    loss->ge_p_ppm = (uint32_t)values[0];
    loss->ge_r_ppm = (uint32_t)values[1];
    loss->ge_h_ppm = (uint32_t)values[2];
    loss->ge_k_ppm = (uint32_t)values[3];
    return 0;
}

static int trace_parse_weights(const char* text, WeightConfig* weights)
{
    if (strncmp(text, "uniform:", 8) == 0) {
        char* end;
        unsigned long value = strtoul(text + 8, &end, 10);
        if (*end != '\0' || value > 255ul) {
            return -1;
        }
        weights->kind = WEIGHT_UNIFORM;
        weights->uniform_weight = trace_min_one_u8(value);
        return 0;
    }
    if (strncmp(text, "twolevel:", 9) == 0) {
        unsigned long values[3];

        if (trace_parse_uint_csv(text + 9, values, 3, 255ul) != 0) {
            return -1;
        }
        weights->kind = WEIGHT_TWOLEVEL;
        weights->high_weight = trace_min_one_u8(values[0]);
        weights->low_weight = trace_min_one_u8(values[1]);
        weights->fraction_q8 = (uint8_t)values[2];
        return 0;
    }
    return -1;
}

static int trace_load_map_file(const char* path, WeightConfig* weights)
{
    FILE* f = fopen(path, "rb");
    long file_size_long;
    size_t file_size;
    uint8_t* file_data;
    uint32_t cols;
    uint32_t rows;
    uint32_t tile_count;
    uint64_t sum = 0u;
    uint64_t mean;
    uint32_t i;

    if (!f) {
        fprintf(stderr, "error: failed to open map file: %s\n", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    file_size_long = ftell(f);
    if (file_size_long < (long)IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    file_size = (size_t)file_size_long;
    file_data = (uint8_t*)malloc(file_size);
    if (!file_data) {
        fclose(f);
        return -1;
    }
    if (fread(file_data, 1u, file_size, f) != file_size) {
        free(file_data);
        fclose(f);
        return -1;
    }
    fclose(f);

    cols = trace_read_u16_le(file_data + 0);
    rows = trace_read_u16_le(file_data + 2);
    tile_count = cols * rows;
    if (tile_count == 0u ||
        file_size != (size_t)IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE + tile_count) {
        free(file_data);
        return -1;
    }

    free(weights->map_weights_norm);
    weights->map_weights_norm = (uint8_t*)malloc(tile_count);
    if (!weights->map_weights_norm) {
        free(file_data);
        return -1;
    }
    for (i = 0; i < tile_count; ++i) {
        const uint8_t w = file_data[IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE + i];
        sum += w > 1u ? (uint64_t)w : 1u;
    }
    mean = (sum + tile_count / 2u) / tile_count;
    if (mean == 0u) {
        mean = 1u;
    }
    for (i = 0; i < tile_count; ++i) {
        const uint8_t w = file_data[IMT_WIRE_MAP_PAYLOAD_HEADER_SIZE + i];
        weights->map_weights_norm[i] =
            (uint8_t)imt_map_normalized_weight(w, (uint32_t)mean);
    }
    weights->kind = WEIGHT_MAP_FILE;
    weights->map_tile_count = tile_count;
    free(file_data);
    return 0;
}

static void trace_weight_destroy(WeightConfig* weights)
{
    free(weights->map_weights_norm);
    weights->map_weights_norm = NULL;
    weights->map_tile_count = 0u;
}

static int trace_make_chunk_weights(const WeightConfig* weights,
                                    uint32_t chunk_count,
                                    uint8_t* chunk_weights)
{
    uint32_t i;

    if (!weights || !chunk_weights || chunk_count == 0u) {
        return -1;
    }

    if (weights->kind == WEIGHT_UNIFORM) {
        memset(chunk_weights, weights->uniform_weight, chunk_count);
        return 0;
    }
    if (weights->kind == WEIGHT_TWOLEVEL) {
        const uint32_t high_count =
            ((uint32_t)chunk_count * weights->fraction_q8) / 256u;
        for (i = 0; i < chunk_count; ++i) {
            chunk_weights[i] = i < high_count ? weights->high_weight : weights->low_weight;
        }
        return 0;
    }
    if (weights->kind == WEIGHT_MAP_FILE) {
        if (!weights->map_weights_norm || weights->map_tile_count == 0u) {
            return -1;
        }
        for (i = 0; i < chunk_count; ++i) {
            chunk_weights[i] = weights->map_weights_norm[i % weights->map_tile_count];
        }
        return 0;
    }
    return -1;
}

static void trace_loss_reset(LossModel* loss)
{
    loss->state = loss->seed == 0u ? TRACE_DEFAULT_SEED : loss->seed;
    loss->ge_bad = 0;
    loss->trace_pos = 0u;
}

static int trace_loss_drop(LossModel* loss, uint64_t packet_id, const ImtWireHeader* header)
{
    uint32_t drop_ppm;
    int drop;

    if (!loss) {
        return 0;
    }

    if (loss->kind == LOSS_NONE) {
        return 0;
    }
    if (loss->kind == LOSS_ONE_PER_GROUP) {
        return header->type == IMT_PACKET_TYPE_SLICE && header->fec_index == 0u;
    }
    if (loss->kind == LOSS_TRACE) {
        while (loss->trace_pos < loss->trace.count &&
               loss->trace.packet_ids[loss->trace_pos] < packet_id) {
            loss->trace_pos += 1u;
        }
        if (loss->trace_pos < loss->trace.count &&
            loss->trace.packet_ids[loss->trace_pos] == packet_id) {
            loss->trace_pos += 1u;
            return 1;
        }
        return 0;
    }
    if (loss->kind == LOSS_IID) {
        return trace_lcg_ppm(&loss->state) < loss->iid_ppm;
    }

    drop_ppm = loss->ge_bad ? loss->ge_k_ppm : loss->ge_h_ppm;
    drop = trace_lcg_ppm(&loss->state) < drop_ppm;
    if (!loss->ge_bad) {
        if (trace_lcg_ppm(&loss->state) < loss->ge_p_ppm) {
            loss->ge_bad = 1;
        }
    } else {
        if (trace_lcg_ppm(&loss->state) < loss->ge_r_ppm) {
            loss->ge_bad = 0;
        }
    }
    return drop;
}

static int trace_emit_packet(const uint8_t* datagram, size_t len, void* user)
{
    EmitContext* ctx = (EmitContext*)user;
    ImtWireHeader header;
    const uint64_t packet_id = *ctx->packet_seq;
    int drop;
    int rc;

    *ctx->packet_seq += 1u;
    rc = imt_wire_decode_header(datagram, len, &header);
    if (rc != 0) {
        ctx->error = rc;
        return rc;
    }

    if (header.type == IMT_PACKET_TYPE_SLICE) {
        ctx->packets_data += 1u;
    } else if (header.type == IMT_PACKET_TYPE_PARITY) {
        ctx->packets_parity += 1u;
        ctx->parity_payload_bytes += header.payload_size;
    }

    drop = trace_loss_drop(ctx->loss, packet_id, &header);
    if (drop) {
        ctx->packets_lost += 1u;
        return 0;
    }

    ctx->delivered_payload_bytes += header.payload_size;
    rc = imt_assembler_push_datagram(ctx->assembler, datagram, len);
    if (rc < 0) {
        ctx->error = rc;
        return rc;
    }
    return 0;
}

static uint32_t trace_chunk_count(uint32_t frame_size, size_t max_payload)
{
    return (uint32_t)(((size_t)frame_size + max_payload - 1u) / max_payload);
}

static uint32_t trace_max_frame_size(const TraceList* trace)
{
    uint32_t max_size = 0u;
    size_t i;

    for (i = 0; i < trace->count; ++i) {
        if (trace->frames[i].size_bytes > max_size) {
            max_size = trace->frames[i].size_bytes;
        }
    }
    return max_size;
}

static int trace_run_replay(const TraceList* trace,
                            const ReplayOptions* options,
                            ReplayOutput* out,
                            ReplayStats* stats)
{
    ImtPacketizer packetizer;
    ImtAssembler assembler;
    LossModel loss;
    uint8_t* frame = NULL;
    uint8_t* chunk_weights = NULL;
    uint32_t max_frame;
    uint32_t max_chunks;
    uint64_t packet_seq = 0u;
    size_t i;
    int rc = 0;

    memset(stats, 0, sizeof(*stats));
    memset(&packetizer, 0, sizeof(packetizer));
    memset(&assembler, 0, sizeof(assembler));

    if (!trace || trace->count == 0u || !options || !stats) {
        return -1;
    }

    max_frame = trace_max_frame_size(trace);
    max_chunks = trace_chunk_count(max_frame, options->max_payload);
    frame = (uint8_t*)malloc(max_frame);
    chunk_weights = (uint8_t*)malloc(max_chunks);
    if (!frame || !chunk_weights) {
        rc = -1;
        goto cleanup;
    }
    if (imt_packetizer_init(&packetizer, options->max_payload, 0) != 0 ||
        imt_assembler_init(&assembler, max_frame, options->max_payload, 0) != 0) {
        rc = -1;
        goto cleanup;
    }

    loss = options->loss;
    trace_loss_reset(&loss);

    if (trace_output_printf(out,
            "frame_seq,size,chunks,groups,packets_data,packets_parity,"
            "packets_lost,chunks_recovered,completed,bit_exact\n") != 0) {
        rc = -1;
        goto cleanup;
    }

    for (i = 0; i < trace->count; ++i) {
        const TraceFrame* tf = &trace->frames[i];
        const uint32_t chunks = trace_chunk_count(tf->size_bytes, options->max_payload);
        const uint64_t recovered_before = assembler.chunks_recovered;
        uint64_t recovered_delta;
        EmitContext emit_ctx;
        int completed;
        int bit_exact;

        trace_fill_frame(frame, tf->size_bytes, options->seed, tf->frame_seq);
        if (trace_make_chunk_weights(&options->weights, chunks, chunk_weights) != 0) {
            rc = -1;
            goto cleanup;
        }

        memset(&emit_ctx, 0, sizeof(emit_ctx));
        emit_ctx.assembler = &assembler;
        emit_ctx.loss = &loss;
        emit_ctx.packet_seq = &packet_seq;

        rc = imt_packetizer_send_frame(&packetizer,
                                       frame,
                                       tf->size_bytes,
                                       tf->frame_seq,
                                       tf->capture_ts_ns,
                                       0,
                                       chunk_weights,
                                       chunks,
                                       trace_emit_packet,
                                       &emit_ctx);
        if (rc != 0 || emit_ctx.error != 0) {
            rc = rc != 0 ? rc : emit_ctx.error;
            goto cleanup;
        }

        completed = assembler.latest.size == tf->size_bytes &&
            assembler.latest.frame_seq == tf->frame_seq;
        bit_exact = completed &&
            memcmp(assembler.latest.data, frame, tf->size_bytes) == 0;
        recovered_delta = assembler.chunks_recovered - recovered_before;

        stats->frames += 1u;
        stats->frames_completed += completed ? 1u : 0u;
        stats->frames_bit_exact += bit_exact ? 1u : 0u;
        stats->recovered_frames += completed && recovered_delta > 0u ? 1u : 0u;
        stats->chunks_recovered += recovered_delta;
        stats->packets_data += emit_ctx.packets_data;
        stats->packets_parity += emit_ctx.packets_parity;
        stats->packets_lost += emit_ctx.packets_lost;
        stats->data_bytes += tf->size_bytes;
        stats->parity_bytes += emit_ctx.parity_payload_bytes;
        stats->delivered_payload_bytes += emit_ctx.delivered_payload_bytes;
        stats->completed_bytes += completed ? tf->size_bytes : 0u;

        if (trace_output_printf(out,
                "%llu,%u,%u,%u,%u,%u,%u,%llu,%d,%d\n",
                (unsigned long long)tf->frame_seq,
                (unsigned)tf->size_bytes,
                (unsigned)chunks,
                (unsigned)emit_ctx.packets_parity,
                (unsigned)emit_ctx.packets_data,
                (unsigned)emit_ctx.packets_parity,
                (unsigned)emit_ctx.packets_lost,
                (unsigned long long)recovered_delta,
                completed,
                bit_exact) != 0) {
            rc = -1;
            goto cleanup;
        }
    }

    {
        const double completion_rate = stats->frames == 0u ? 0.0 :
            (double)stats->frames_completed / (double)stats->frames;
        const double recovery_contribution = stats->frames == 0u ? 0.0 :
            (double)stats->recovered_frames / (double)stats->frames;
        const double overhead_pct = stats->data_bytes == 0u ? 0.0 :
            (double)stats->parity_bytes * 100.0 / (double)stats->data_bytes;
        const double goodput = stats->delivered_payload_bytes == 0u ? 0.0 :
            (double)stats->completed_bytes * 100.0 /
            (double)stats->delivered_payload_bytes;

        if (trace_output_printf(out,
                "summary,frames,%llu,completed,%llu,bit_exact,%llu,"
                "recovered_frames,%llu,chunks_recovered,%llu,"
                "completion_rate,%.6f,recovery_contribution,%.6f,"
                "overhead_pct,%.6f,goodput,%.6f\n",
                (unsigned long long)stats->frames,
                (unsigned long long)stats->frames_completed,
                (unsigned long long)stats->frames_bit_exact,
                (unsigned long long)stats->recovered_frames,
                (unsigned long long)stats->chunks_recovered,
                completion_rate,
                recovery_contribution,
                overhead_pct,
                goodput) != 0) {
            rc = -1;
        }
    }

cleanup:
    imt_assembler_destroy(&assembler);
    imt_packetizer_destroy(&packetizer);
    free(frame);
    free(chunk_weights);
    return rc;
}

static ReplayOptions trace_default_options(void)
{
    ReplayOptions options;

    memset(&options, 0, sizeof(options));
    options.max_payload = IMT_DEFAULT_MAX_PAYLOAD;
    options.seed = TRACE_DEFAULT_SEED;
    options.loss.kind = LOSS_NONE;
    options.loss.seed = TRACE_DEFAULT_SEED;
    options.weights.kind = WEIGHT_UNIFORM;
    options.weights.uniform_weight = 128u;
    return options;
}

static int trace_self_test_case(const char* name,
                                const TraceList* trace,
                                const ReplayOptions* options,
                                ReplayStats* stats,
                                char** csv_out)
{
    ReplayOutput out;
    int rc;

    memset(&out, 0, sizeof(out));
    out.enabled = 1;
    rc = trace_run_replay(trace, options, &out, stats);
    if (rc != 0) {
        trace_output_destroy(&out);
        fprintf(stderr, "self-test %s: replay failed (%d)\n", name, rc);
        return -1;
    }
    if (csv_out) {
        *csv_out = out.buffer;
        out.buffer = NULL;
        out.length = 0u;
        out.capacity = 0u;
    }
    trace_output_destroy(&out);
    return 0;
}

static int trace_run_self_test(void)
{
    TraceList trace;
    ReplayOptions options;
    ReplayStats stats;
    char* csv_a = NULL;
    char* csv_b = NULL;
    clock_t begin_clock;
    clock_t end_clock;
    double elapsed_sec;
    int ok = 1;

    memset(&trace, 0, sizeof(trace));
    if (trace_make_synthetic(&trace, 64u, TRACE_DEFAULT_FRAME_SIZE) != 0) {
        trace_list_destroy(&trace);
        return 1;
    }

    options = trace_default_options();
    if (trace_self_test_case("loss0", &trace, &options, &stats, NULL) != 0 ||
        stats.frames_completed != stats.frames ||
        stats.frames_bit_exact != stats.frames) {
        fprintf(stderr, "self-test R13.6(1): FAIL\n");
        ok = 0;
    } else {
        fprintf(stderr, "self-test R13.6(1): PASS completion_rate=1.000000 bit_exact=all\n");
    }

    options = trace_default_options();
    options.loss.kind = LOSS_ONE_PER_GROUP;
    if (trace_self_test_case("one-loss-per-group", &trace, &options, &stats, &csv_a) != 0 ||
        stats.frames_completed != stats.frames ||
        stats.chunks_recovered == 0u ||
        !csv_a ||
        strstr(csv_a, ",4,1,1\n") == NULL) {
        fprintf(stderr, "self-test R13.6(2): FAIL\n");
        ok = 0;
    } else {
        fprintf(stderr,
                "self-test R13.6(2): PASS completion_rate=1.000000 chunks_recovered=%llu\n",
                (unsigned long long)stats.chunks_recovered);
    }
    free(csv_a);
    csv_a = NULL;

    options = trace_default_options();
    options.loss.kind = LOSS_IID;
    options.loss.iid_ppm = 25000u;
    options.loss.seed = 424242u;
    options.seed = 777u;
    if (trace_self_test_case("determinism-a", &trace, &options, &stats, &csv_a) != 0 ||
        trace_self_test_case("determinism-b", &trace, &options, &stats, &csv_b) != 0 ||
        !csv_a || !csv_b || strcmp(csv_a, csv_b) != 0) {
        fprintf(stderr, "self-test R13.6(3): FAIL\n");
        ok = 0;
    } else {
        fprintf(stderr, "self-test R13.6(3): PASS same-seed CSV identical\n");
    }
    free(csv_a);
    free(csv_b);
    trace_list_destroy(&trace);

    memset(&trace, 0, sizeof(trace));
    if (trace_make_synthetic(&trace, TRACE_PERF_FRAMES, TRACE_DEFAULT_FRAME_SIZE) != 0) {
        trace_list_destroy(&trace);
        return 1;
    }
    options = trace_default_options();
    begin_clock = clock();
    {
        ReplayOutput out;
        memset(&out, 0, sizeof(out));
        out.enabled = 0;
        if (trace_run_replay(&trace, &options, &out, &stats) != 0) {
            fprintf(stderr, "self-test R13.5: FAIL replay error\n");
            ok = 0;
        }
    }
    end_clock = clock();
    elapsed_sec = (double)(end_clock - begin_clock) / (double)CLOCKS_PER_SEC;
    fprintf(stderr, "self-test R13.5: 10000 frames %.3f s\n", elapsed_sec);
    if (elapsed_sec > 10.0) {
        fprintf(stderr, "self-test R13.5: FAIL >10s\n");
        ok = 0;
    }
    trace_list_destroy(&trace);

    return ok ? 0 : 1;
}

static void trace_print_usage(FILE* file)
{
    fprintf(file,
            "usage: imt_trace_replay --trace frames.csv [options]\n"
            "       imt_trace_replay --self-test\n"
            "options:\n"
            "  --loss-iid <ppm>                 independent packet loss\n"
            "  --loss-ge <p,r,h,k>              Gilbert-Elliott ppm parameters\n"
            "  --loss-trace <file>              global packet ids to drop\n"
            "  --weights uniform:<w>            normalized chunk weight\n"
            "  --weights twolevel:<hi>,<lo>,<fr_q8>\n"
            "  --map-file <file>                binary MAP payload header + weights\n"
            "  --seed <n>                       deterministic payload/loss seed\n"
            "  --max-payload <bytes>            defaults to 1200\n");
}

int main(int argc, char** argv)
{
    ReplayOptions options = trace_default_options();
    TraceList trace;
    ReplayStats stats;
    ReplayOutput out;
    const char* trace_path = NULL;
    int self_test = 0;
    int i;
    int rc;

    memset(&trace, 0, sizeof(trace));
    memset(&out, 0, sizeof(out));
    out.enabled = 1;
    out.file = stdout;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--self-test") == 0) {
            self_test = 1;
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (strcmp(argv[i], "--loss-iid") == 0 && i + 1 < argc) {
            options.loss.kind = LOSS_IID;
            if (trace_parse_ppm(argv[++i], &options.loss.iid_ppm) != 0) {
                fprintf(stderr, "error: bad --loss-iid ppm\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--loss-ge") == 0 && i + 1 < argc) {
            if (trace_parse_loss_ge(argv[++i], &options.loss) != 0) {
                fprintf(stderr, "error: bad --loss-ge p,r,h,k\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--loss-trace") == 0 && i + 1 < argc) {
            options.loss.kind = LOSS_TRACE;
            if (trace_load_loss_trace(argv[++i], &options.loss.trace) != 0) {
                return 2;
            }
        } else if (strcmp(argv[i], "--weights") == 0 && i + 1 < argc) {
            if (trace_parse_weights(argv[++i], &options.weights) != 0) {
                fprintf(stderr, "error: bad --weights\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--map-file") == 0 && i + 1 < argc) {
            if (trace_load_map_file(argv[++i], &options.weights) != 0) {
                return 2;
            }
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            char* end;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end != '\0') {
                fprintf(stderr, "error: bad --seed\n");
                return 2;
            }
            options.seed = (uint32_t)value;
            options.loss.seed = (uint32_t)value;
        } else if (strcmp(argv[i], "--max-payload") == 0 && i + 1 < argc) {
            char* end;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || value == 0ul || value > 65535ul) {
                fprintf(stderr, "error: bad --max-payload\n");
                return 2;
            }
            options.max_payload = (size_t)value;
        } else if (strcmp(argv[i], "--help") == 0) {
            trace_print_usage(stdout);
            return 0;
        } else {
            trace_print_usage(stderr);
            return 2;
        }
    }

    if (self_test) {
        rc = trace_run_self_test();
        trace_weight_destroy(&options.weights);
        trace_loss_list_destroy(&options.loss.trace);
        return rc;
    }

    if (!trace_path) {
        trace_print_usage(stderr);
        trace_weight_destroy(&options.weights);
        trace_loss_list_destroy(&options.loss.trace);
        return 2;
    }
    if (trace_load_csv(trace_path, &trace) != 0) {
        trace_weight_destroy(&options.weights);
        trace_loss_list_destroy(&options.loss.trace);
        trace_list_destroy(&trace);
        return 2;
    }

    rc = trace_run_replay(&trace, &options, &out, &stats);
    trace_weight_destroy(&options.weights);
    trace_loss_list_destroy(&options.loss.trace);
    trace_list_destroy(&trace);
    return rc == 0 ? 0 : 1;
}
