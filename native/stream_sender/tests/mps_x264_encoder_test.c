#include "mps_x264_encoder.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_WIDTH 64u
#define TEST_HEIGHT 64u
#define TEST_FPS 60u
#define TEST_GOP 4u

static int encode_frame(MpsX264Ctx* encoder, uint8_t* i420, uint32_t index,
                        int* out_keyframe)
{
    uint8_t* nal = NULL;
    const MpsX264SliceInfo* slices = NULL;
    uint32_t slice_count = 0u;
    const size_t y_size = (size_t)TEST_WIDTH * TEST_HEIGHT;
    const size_t frame_size = y_size * 3u / 2u;

    memset(i420, (int)(16u + index), y_size);
    memset(i420 + y_size, 128, frame_size - y_size);
    const int encoded = mps_x264_encode(encoder, i420, NULL, NULL, &nal,
                                        out_keyframe, &slices, &slice_count);
    if (encoded <= 0 || !nal || !slices || slice_count == 0u) {
        fprintf(stderr, "frame %u did not produce an access unit\n", index);
        return -1;
    }
    return 0;
}

int main(void)
{
    const size_t frame_size = (size_t)TEST_WIDTH * TEST_HEIGHT * 3u / 2u;
    uint8_t* i420 = (uint8_t*)malloc(frame_size);
    MpsX264Options options;
    MpsX264Ctx* encoder;
    uint32_t frame;

    if (!i420) {
        return 1;
    }

    mps_x264_default_options(&options);
    options.keyint = TEST_GOP;
    options.slice_max_size = 0u;
    encoder = mps_x264_init_ex(TEST_WIDTH, TEST_HEIGHT, TEST_FPS, 20.0f, &options);
    if (!encoder) {
        free(i420);
        return 2;
    }
    for (frame = 0u; frame < TEST_GOP + 1u; ++frame) {
        int keyframe = 0;
        if (frame == 2u && mps_x264_set_crf(encoder, 24.0f) != 0) {
            fprintf(stderr, "CRF reconfigure failed\n");
            mps_x264_destroy(encoder);
            free(i420);
            return 3;
        }
        if (encode_frame(encoder, i420, frame, &keyframe) != 0) {
            mps_x264_destroy(encoder);
            free(i420);
            return 3;
        }
        if ((frame == 0u || frame == TEST_GOP) != (keyframe != 0)) {
            fprintf(stderr, "unexpected GOP keyframe state at frame %u: %d\n", frame, keyframe);
            mps_x264_destroy(encoder);
            free(i420);
            return 4;
        }
    }
    mps_x264_destroy(encoder);

    /* The legacy entry point remains all-IDR for the existing SBS sender. */
    encoder = mps_x264_init(TEST_WIDTH, TEST_HEIGHT, TEST_FPS, 20.0f);
    if (!encoder) {
        free(i420);
        return 5;
    }
    for (frame = 0u; frame < 3u; ++frame) {
        int keyframe = 0;
        if (encode_frame(encoder, i420, frame, &keyframe) != 0 || !keyframe) {
            fprintf(stderr, "legacy encoder frame %u was not IDR\n", frame);
            mps_x264_destroy(encoder);
            free(i420);
            return 6;
        }
    }

    mps_x264_destroy(encoder);
    free(i420);
    return 0;
}
