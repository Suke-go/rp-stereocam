#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "mps_rvm_importance.h"
#include <onnxruntime_c_api.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define RVM_NUM_RECURRENT 4u

/* Input / output tensor names for rvm_mobilenetv3_fp32.onnx */
static const char* const k_input_names[6] = {
    "src", "r1i", "r2i", "r3i", "r4i", "downsample_ratio"
};
static const char* const k_output_names[6] = {
    "fgr", "pha", "r1o", "r2o", "r3o", "r4o"
};

struct MpsRvmCtx {
    const OrtApi*   ort;
    OrtEnv*         env;
    OrtSession*     session;
    OrtMemoryInfo*  mem_info;

    uint32_t width;
    uint32_t height;

    float* rgb_buf;  /* NCHW float32 [1,3,H,W] */
    float* pha_buf;  /* float32 [H,W]           */

    /* Recurrent state r1..r4 — caller-owned buffers reused across frames. */
    float*   r_bufs[RVM_NUM_RECURRENT];
    size_t   r_sizes[RVM_NUM_RECURRENT]; /* element counts */
    int64_t  r_shapes[RVM_NUM_RECURRENT][4];
};

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static int clamp_i(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* BT.601 limited-range YUV420 → NCHW float32 RGB [1,3,H,W] in [0,1]. */
static void yuv420_to_nchw_rgb(float* restrict rgb, const uint8_t* restrict yuv,
                                uint32_t w, uint32_t h)
{
    const uint8_t* y_p = yuv;
    const uint8_t* u_p = yuv + (size_t)w * h;
    const uint8_t* v_p = u_p + (size_t)w * h / 4u;
    const size_t plane = (size_t)w * h;
    uint32_t r, c;

    for (r = 0u; r < h; ++r) {
        for (c = 0u; c < w; ++c) {
            const int yy  = y_p[(size_t)r * w + c];
            const int uu  = u_p[(r / 2u) * (w / 2u) + (c / 2u)];
            const int vv  = v_p[(r / 2u) * (w / 2u) + (c / 2u)];
            const int cy  = 298 * (yy - 16);
            const int d   = uu - 128;
            const int e   = vv - 128;
            const size_t idx = (size_t)r * w + c;

            rgb[0u * plane + idx] = (float)clamp_i((cy + 409*e + 128) >> 8, 0, 255) / 255.0f;
            rgb[1u * plane + idx] = (float)clamp_i((cy - 100*d - 208*e + 128) >> 8, 0, 255) / 255.0f;
            rgb[2u * plane + idx] = (float)clamp_i((cy + 516*d + 128) >> 8, 0, 255) / 255.0f;
        }
    }
}

#define ORT_CHECK(st, label, retval) \
    do { \
        if ((st)) { \
            fprintf(stderr, "[rvm] ORT error: %s\n", ctx->ort->GetErrorMessage(st)); \
            ctx->ort->ReleaseStatus(st); \
            rc = (retval); goto label; \
        } \
    } while (0)

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

MpsRvmCtx* mps_rvm_init(const char* model_path, uint32_t width, uint32_t height)
{
    OrtStatus* st;
    OrtSessionOptions* opts = NULL;
    uint32_t i;

    MpsRvmCtx* ctx = (MpsRvmCtx*)calloc(1u, sizeof(MpsRvmCtx));
    if (!ctx) return NULL;

    ctx->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!ctx->ort) {
        fprintf(stderr, "[rvm] OrtGetApiBase()->GetApi() failed\n");
        free(ctx);
        return NULL;
    }

    st = ctx->ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "rvm", &ctx->env);
    if (st) {
        fprintf(stderr, "[rvm] CreateEnv: %s\n", ctx->ort->GetErrorMessage(st));
        ctx->ort->ReleaseStatus(st);
        mps_rvm_destroy(ctx);
        return NULL;
    }

    st = ctx->ort->CreateSessionOptions(&opts);
    if (st) {
        fprintf(stderr, "[rvm] CreateSessionOptions: %s\n", ctx->ort->GetErrorMessage(st));
        ctx->ort->ReleaseStatus(st);
        mps_rvm_destroy(ctx);
        return NULL;
    }
    /* Pi 5: 2 intra-op threads, 1 inter-op thread for best single-session perf */
    ctx->ort->SetSessionExecutionMode(opts, ORT_SEQUENTIAL);
    ctx->ort->SetInterOpNumThreads(opts, 1);
    ctx->ort->SetIntraOpNumThreads(opts, 2);

    st = ctx->ort->CreateSession(ctx->env, model_path, opts, &ctx->session);
    ctx->ort->ReleaseSessionOptions(opts);
    if (st) {
        fprintf(stderr, "[rvm] CreateSession(%s): %s\n",
                model_path, ctx->ort->GetErrorMessage(st));
        ctx->ort->ReleaseStatus(st);
        mps_rvm_destroy(ctx);
        return NULL;
    }

    st = ctx->ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                        &ctx->mem_info);
    if (st) {
        fprintf(stderr, "[rvm] CreateCpuMemoryInfo: %s\n", ctx->ort->GetErrorMessage(st));
        ctx->ort->ReleaseStatus(st);
        mps_rvm_destroy(ctx);
        return NULL;
    }

    ctx->width  = width;
    ctx->height = height;

    ctx->rgb_buf = (float*)malloc((size_t)width * height * 3u * sizeof(float));
    ctx->pha_buf = (float*)malloc((size_t)width * height * sizeof(float));
    if (!ctx->rgb_buf || !ctx->pha_buf) {
        mps_rvm_destroy(ctx);
        return NULL;
    }

    /* Initial recurrent state: shape [1,1,1,1], value 0.0 */
    for (i = 0u; i < RVM_NUM_RECURRENT; ++i) {
        ctx->r_bufs[i] = (float*)calloc(1u, sizeof(float));
        if (!ctx->r_bufs[i]) { mps_rvm_destroy(ctx); return NULL; }
        ctx->r_sizes[i]    = 1u;
        ctx->r_shapes[i][0] = 1; ctx->r_shapes[i][1] = 1;
        ctx->r_shapes[i][2] = 1; ctx->r_shapes[i][3] = 1;
    }

    return ctx;
}

void mps_rvm_destroy(MpsRvmCtx* ctx)
{
    uint32_t i;
    if (!ctx) return;
    if (ctx->ort) {
        if (ctx->session)  ctx->ort->ReleaseSession(ctx->session);
        if (ctx->mem_info) ctx->ort->ReleaseMemoryInfo(ctx->mem_info);
        if (ctx->env)      ctx->ort->ReleaseEnv(ctx->env);
    }
    free(ctx->rgb_buf);
    free(ctx->pha_buf);
    for (i = 0u; i < RVM_NUM_RECURRENT; ++i) {
        free(ctx->r_bufs[i]);
    }
    free(ctx);
}

int mps_rvm_update_map(MpsRvmCtx* ctx,
                       const uint8_t* yuv420, uint32_t yuv_size,
                       ImtMap* map, uint32_t eye_x_offset)
{
    OrtValue* inputs[6]  = {NULL, NULL, NULL, NULL, NULL, NULL};
    OrtValue* outputs[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
    OrtStatus* st;
    int64_t src_shape[4];
    float   ds_ratio = 0.25f;  /* must live past Run() — declared at function scope */
    int64_t ds_shape[1] = {1};
    uint32_t i;
    int rc = 0;

    if (!ctx || !yuv420 || !map || !map->weights || map->tile_count == 0u) return -1;
    if ((size_t)yuv_size < (size_t)ctx->width * ctx->height) return -2;

    /* Step 1: YUV420 → NCHW float32 RGB */
    yuv420_to_nchw_rgb(ctx->rgb_buf, yuv420, ctx->width, ctx->height);

    /* Step 2: build input OrtValues */

    src_shape[0] = 1;
    src_shape[1] = 3;
    src_shape[2] = (int64_t)ctx->height;
    src_shape[3] = (int64_t)ctx->width;
    st = ctx->ort->CreateTensorWithDataAsOrtValue(
        ctx->mem_info,
        ctx->rgb_buf, (size_t)ctx->width * ctx->height * 3u * sizeof(float),
        src_shape, 4u, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[0]);
    ORT_CHECK(st, cleanup, -3);

    for (i = 0u; i < RVM_NUM_RECURRENT; ++i) {
        st = ctx->ort->CreateTensorWithDataAsOrtValue(
            ctx->mem_info,
            ctx->r_bufs[i], ctx->r_sizes[i] * sizeof(float),
            ctx->r_shapes[i], 4u, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
            &inputs[1u + i]);
        ORT_CHECK(st, cleanup, -4);
    }

    st = ctx->ort->CreateTensorWithDataAsOrtValue(
        ctx->mem_info,
        &ds_ratio, sizeof(float),
        ds_shape, 1u, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[5]);
    ORT_CHECK(st, cleanup, -5);

    /* Step 3: run inference */
    st = ctx->ort->Run(ctx->session, NULL,
                       k_input_names,  (const OrtValue* const*)inputs,  6u,
                       k_output_names, 6u, outputs);
    ORT_CHECK(st, cleanup, -6);

    /* Copy pha (outputs[1]) into scratch buffer */
    {
        float* pha_data = NULL;
        st = ctx->ort->GetTensorMutableData(outputs[1], (void**)&pha_data);
        ORT_CHECK(st, cleanup, -7);
        memcpy(ctx->pha_buf, pha_data, (size_t)ctx->width * ctx->height * sizeof(float));
    }

    /* Copy recurrent outputs (outputs[2..5] = r1o..r4o) into r_bufs */
    for (i = 0u; i < RVM_NUM_RECURRENT; ++i) {
        OrtTensorTypeAndShapeInfo* info = NULL;
        size_t elem_count = 0u;
        size_t ndim = 0u;
        int64_t dims[4] = {1, 1, 1, 1};
        float* r_data = NULL;

        st = ctx->ort->GetTensorTypeAndShape(outputs[2u + i], &info);
        ORT_CHECK(st, cleanup, -8);
        ctx->ort->GetTensorShapeElementCount(info, &elem_count);
        ctx->ort->GetDimensionsCount(info, &ndim);
        if (ndim > 4u) ndim = 4u;
        ctx->ort->GetDimensions(info, dims, ndim);
        ctx->ort->ReleaseTensorTypeAndShapeInfo(info);

        if (elem_count == 0u) { rc = -9; goto cleanup; }

        if (elem_count != ctx->r_sizes[i]) {
            float* new_buf = (float*)realloc(ctx->r_bufs[i], elem_count * sizeof(float));
            if (!new_buf) { rc = -10; goto cleanup; }
            ctx->r_bufs[i]  = new_buf;
            ctx->r_sizes[i] = elem_count;
        }
        memcpy(ctx->r_shapes[i], dims, ndim * sizeof(int64_t));

        st = ctx->ort->GetTensorMutableData(outputs[2u + i], (void**)&r_data);
        ORT_CHECK(st, cleanup, -11);
        memcpy(ctx->r_bufs[i], r_data, elem_count * sizeof(float));
    }

    /* Step 4: alpha → tile weights → EMA */
    {
        uint8_t* tile_samples = (uint8_t*)malloc(map->tile_count);
        uint32_t col_start, col_end, tile_row, tile_col;

        if (!tile_samples) { rc = -12; goto cleanup; }
        memset(tile_samples, 128u, map->tile_count); /* neutral for non-updated tiles */

        col_start = eye_x_offset / (uint32_t)map->tile_size;
        col_end   = (eye_x_offset + ctx->width + (uint32_t)map->tile_size - 1u)
                    / (uint32_t)map->tile_size;
        if (col_end > (uint32_t)map->cols) col_end = (uint32_t)map->cols;

        for (tile_row = 0u; tile_row < (uint32_t)map->rows; ++tile_row) {
            for (tile_col = col_start; tile_col < col_end; ++tile_col) {
                const uint32_t px_r0  = tile_row * (uint32_t)map->tile_size;
                const uint32_t px_r1  = px_r0 + (uint32_t)map->tile_size;
                /* eye-local pixel column start: tile may start before eye_x_offset */
                const uint32_t abs_c0 = tile_col * (uint32_t)map->tile_size;
                const uint32_t px_c0  = abs_c0 >= eye_x_offset ? abs_c0 - eye_x_offset : 0u;
                const uint32_t px_c1  = px_c0 + (uint32_t)map->tile_size;
                const uint32_t r_end  = px_r1 < ctx->height ? px_r1 : ctx->height;
                const uint32_t c_end  = px_c1 < ctx->width  ? px_c1 : ctx->width;
                float sum = 0.0f;
                uint32_t count = 0u, pr, pc;

                for (pr = px_r0; pr < r_end; ++pr) {
                    for (pc = px_c0; pc < c_end; ++pc) {
                        sum += ctx->pha_buf[(size_t)pr * ctx->width + pc];
                        ++count;
                    }
                }

                tile_samples[tile_row * (uint32_t)map->cols + tile_col] =
                    (count > 0u && (sum / (float)count) > 0.5f)
                    ? (uint8_t)MPS_RVM_WEIGHT_FG
                    : (uint8_t)MPS_RVM_WEIGHT_BG;
            }
        }

        imt_map_update_ema(map, tile_samples, map->tile_count);
        free(tile_samples);
    }

cleanup:
    for (i = 0u; i < 6u; ++i) {
        if (inputs[i])  ctx->ort->ReleaseValue(inputs[i]);
        if (outputs[i]) ctx->ort->ReleaseValue(outputs[i]);
    }
    return rc;
}
