#include "mp_edge_config.h"
#include "mp_clock_sync.h"
#include "mp_foreground.h"
#include "mp_frame.h"
#include "mp_frame_ring.h"
#include "mp_mask.h"
#include "mp_preprocess.h"
#include "mp_security.h"
#include "mp_stats.h"
#include "mp_sync_monitor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_core_self_test(void)
{
    enum { width = 320, height = 240 };
    const size_t frame_size = mp_nv12_size(width, height);
    const size_t sbs_size = mp_nv12_size(width * 2u, height);
    MpEdgeConfig config = mp_edge_config_default();
    MpFrameRing ring;
    MpFrame left_frame;
    MpFrame right_frame;
    MpFrame sbs_frame;
    MpFrame out_frame;
    MpPrivacyState privacy;
    MpEdgeStats stats;
    MpSolidMaskColor matte = {16, 128, 128};
    MpClockSync left_clock;
    MpClockSync right_clock;
    MpPtpSample ptp_sample;
    MpPreprocessConfig preprocess;
    MpPreprocessResult preprocess_result;
    MpSyncMonitor sync_monitor;
    MpSyncMonitorConfig sync_monitor_config;
    MpForegroundWorkspace foreground_workspace;
    uint8_t* left_storage = NULL;
    uint8_t* right_storage = NULL;
    uint8_t* sbs_storage = NULL;
    uint8_t* out_storage = NULL;
    int rc;

    config.eye_width = width;
    config.eye_height = height;
    config.fps = 60;
    config.privacy_enforced = 0;

    rc = mp_edge_config_validate(&config);
    if (rc != 0) {
        fprintf(stderr, "config validation failed: %d\n", rc);
        return 1;
    }

    left_storage = (uint8_t*)malloc(frame_size);
    right_storage = (uint8_t*)malloc(frame_size);
    sbs_storage = (uint8_t*)malloc(sbs_size);
    out_storage = (uint8_t*)malloc(sbs_size);
    if (!left_storage || !right_storage || !sbs_storage || !out_storage) {
        free(left_storage);
        free(right_storage);
        free(sbs_storage);
        free(out_storage);
        return 2;
    }

    memset(left_storage, 96, frame_size);
    memset(right_storage, 104, frame_size);
    memset(sbs_storage, 0, sbs_size);

    left_frame.data = left_storage;
    left_frame.data_size = frame_size;
    left_frame.width = width;
    left_frame.height = height;
    left_frame.stride_y = width;
    left_frame.stride_uv = width;
    left_frame.timestamp_ns = 1000;
    left_frame.sequence = 1;
    left_frame.format = MP_PIXFMT_NV12;

    right_frame = left_frame;
    right_frame.data = right_storage;
    right_frame.timestamp_ns = 1000 + 1000000u;
    right_frame.sequence = 1;

    sbs_frame.data = sbs_storage;
    sbs_frame.data_size = sbs_size;
    sbs_frame.width = width * 2u;
    sbs_frame.height = height;
    sbs_frame.stride_y = width * 2u;
    sbs_frame.stride_uv = width * 2u;
    sbs_frame.timestamp_ns = 0;
    sbs_frame.sequence = 0;
    sbs_frame.format = MP_PIXFMT_NV12;

    mp_stats_reset(&stats);
    mp_stats_on_capture(&stats, left_frame.timestamp_ns);
    mp_stats_on_capture(&stats, right_frame.timestamp_ns);

    privacy.mask_ready = 1;
    privacy.receiver_authenticated = 1;
    privacy.development_override = 0;

    if (mp_security_privacy_decision(&config, &privacy, &left_frame) == MP_PRIVACY_MASK_FULL_FRAME) {
        rc = mp_mask_fill_solid_nv12(&left_frame, matte);
        if (rc != 0) {
            fprintf(stderr, "full mask failed: %d\n", rc);
            free(left_storage);
            free(right_storage);
            free(sbs_storage);
            free(out_storage);
            return 3;
        }
        mp_stats_on_full_mask(&stats);
    }

    preprocess = mp_preprocess_config_default(config.fps);
    mp_clock_sync_reset(&left_clock);
    mp_clock_sync_reset(&right_clock);
    ptp_sample.local_tx_ns = 1000000000ull;
    ptp_sample.remote_rx_ns = 1000020000ull;
    ptp_sample.remote_tx_ns = 1000025000ull;
    ptp_sample.local_rx_ns = 1000006000ull;
    rc = mp_clock_sync_update_ptp(&left_clock, &ptp_sample);
    if (rc != 0) {
        fprintf(stderr, "left clock sync failed: %d\n", rc);
        free(left_storage);
        free(right_storage);
        free(sbs_storage);
        free(out_storage);
        return 8;
    }
    ptp_sample.local_tx_ns = 1001000000ull;
    ptp_sample.remote_rx_ns = 1001020000ull;
    ptp_sample.remote_tx_ns = 1001025000ull;
    ptp_sample.local_rx_ns = 1001006000ull;
    rc = mp_clock_sync_update_ptp(&left_clock, &ptp_sample);
    if (rc != 0) {
        fprintf(stderr, "left clock sync update failed: %d\n", rc);
        free(left_storage);
        free(right_storage);
        free(sbs_storage);
        free(out_storage);
        return 9;
    }
    ptp_sample.local_tx_ns = 1000000000ull;
    ptp_sample.remote_rx_ns = 1000021000ull;
    ptp_sample.remote_tx_ns = 1000026000ull;
    ptp_sample.local_rx_ns = 1000006000ull;
    rc = mp_clock_sync_update_ptp(&right_clock, &ptp_sample);
    if (rc != 0) {
        fprintf(stderr, "right clock sync failed: %d\n", rc);
        free(left_storage);
        free(right_storage);
        free(sbs_storage);
        free(out_storage);
        return 10;
    }
    ptp_sample.local_tx_ns = 1001000000ull;
    ptp_sample.remote_rx_ns = 1001021000ull;
    ptp_sample.remote_tx_ns = 1001026000ull;
    ptp_sample.local_rx_ns = 1001006000ull;
    rc = mp_clock_sync_update_ptp(&right_clock, &ptp_sample);
    if (rc != 0) {
        fprintf(stderr, "right clock sync update failed: %d\n", rc);
        free(left_storage);
        free(right_storage);
        free(sbs_storage);
        free(out_storage);
        return 11;
    }
    preprocess.use_clock_sync = 1;
    preprocess.left_clock = &left_clock;
    preprocess.right_clock = &right_clock;
    sync_monitor_config = mp_sync_monitor_config_default(config.fps);
    sync_monitor_config.warmup_pairs = 1;
    mp_sync_monitor_init(&sync_monitor, &sync_monitor_config);
    preprocess.sync_monitor = &sync_monitor;
    rc = mp_foreground_workspace_init(&foreground_workspace, width, height);
    if (rc != 0) {
        fprintf(stderr, "foreground workspace init failed: %d\n", rc);
        free(left_storage);
        free(right_storage);
        free(sbs_storage);
        free(out_storage);
        return 12;
    }
    preprocess.foreground_workspace = &foreground_workspace;
    rc = mp_preprocess_stereo_to_sbs_nv12(&preprocess,
                                          &left_frame,
                                          &right_frame,
                                          &sbs_frame,
                                          &stats,
                                          &preprocess_result);
    if (rc != 0) {
        fprintf(stderr, "preprocess failed: %d\n", rc);
        free(left_storage);
        free(right_storage);
        free(sbs_storage);
        free(out_storage);
        mp_foreground_workspace_destroy(&foreground_workspace);
        return 7;
    }

    rc = mp_frame_ring_init(&ring, config.frame_queue_capacity, sbs_size);
    if (rc != 0) {
        fprintf(stderr, "ring init failed: %d\n", rc);
        free(left_storage);
        free(right_storage);
        free(sbs_storage);
        free(out_storage);
        mp_foreground_workspace_destroy(&foreground_workspace);
        return 4;
    }

    rc = mp_frame_ring_write_latest(&ring, &sbs_frame);
    if (rc != 0) {
        fprintf(stderr, "ring write failed: %d\n", rc);
        mp_frame_ring_destroy(&ring);
        free(left_storage);
        free(right_storage);
        free(sbs_storage);
        free(out_storage);
        mp_foreground_workspace_destroy(&foreground_workspace);
        return 5;
    }

    rc = mp_frame_ring_read_latest(&ring, &out_frame, out_storage, sbs_size);
    if (rc != 0) {
        fprintf(stderr, "ring read failed: %d\n", rc);
        mp_frame_ring_destroy(&ring);
        free(left_storage);
        free(right_storage);
        free(sbs_storage);
        free(out_storage);
        mp_foreground_workspace_destroy(&foreground_workspace);
        return 6;
    }

    mp_stats_on_encode(&stats);
    mp_stats_on_transmit(&stats, 2000);

    printf("mp_edge_camera self-test ok: left_camera=%u right_camera=%u eye=%ux%u sbs=%ux%u fps=%u skew_ns=%llu sync_state=%d delay_ns=%llu captured=%llu masked=%llu tx=%llu\n",
           config.left_camera_index,
           config.right_camera_index,
           config.eye_width,
           config.eye_height,
           out_frame.width,
           out_frame.height,
           config.fps,
           (unsigned long long)preprocess_result.stereo_skew_ns,
           (int)preprocess_result.sync_state,
           (unsigned long long)((left_clock.one_way_delay_ns + right_clock.one_way_delay_ns) / 2u),
           (unsigned long long)stats.captured_frames,
           (unsigned long long)stats.masked_frames,
           (unsigned long long)stats.transmitted_frames);

    mp_frame_ring_destroy(&ring);
    free(left_storage);
    free(right_storage);
    free(sbs_storage);
    free(out_storage);
    mp_foreground_workspace_destroy(&foreground_workspace);
    return 0;
}

int main(void)
{
    return run_core_self_test();
}
