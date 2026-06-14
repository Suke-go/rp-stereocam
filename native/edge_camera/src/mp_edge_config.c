#include "mp_edge_config.h"

MpEdgeConfig mp_edge_config_default(void)
{
    MpEdgeConfig config;
    config.eye_width = 1280;
    config.eye_height = 720;
    config.fps = 60;
    config.bitrate_kbps = 18000;
    config.left_camera_index = 1;
    config.right_camera_index = 0;
    config.frame_queue_capacity = 2;
    config.privacy_enforced = 1;
    config.fail_closed = 1;
    config.mask_mode = MP_MASK_SOLID;
    config.transport_mode = MP_TRANSPORT_RTP_UDP;
    return config;
}

int mp_edge_config_validate(const MpEdgeConfig* config)
{
    if (!config) {
        return -1;
    }
    if (config->eye_width < 320 || config->eye_height < 240) {
        return -2;
    }
    if (config->eye_width > 1920 || config->eye_height > 1080) {
        return -3;
    }
    if (config->fps == 0 || config->fps > 120) {
        return -4;
    }
    if (config->bitrate_kbps < 1000 || config->bitrate_kbps > 80000) {
        return -5;
    }
    if (config->left_camera_index > 15 || config->right_camera_index > 15) {
        return -6;
    }
    if (config->left_camera_index == config->right_camera_index) {
        return -7;
    }
    if (config->frame_queue_capacity == 0 || config->frame_queue_capacity > 4) {
        return -8;
    }
    if (config->mask_mode != MP_MASK_SOLID &&
        config->mask_mode != MP_MASK_MOSAIC &&
        config->mask_mode != MP_MASK_GREENBACK_KEEP_HUMAN) {
        return -9;
    }
    if (config->transport_mode != MP_TRANSPORT_RTP_UDP && config->transport_mode != MP_TRANSPORT_WEBRTC) {
        return -10;
    }
    return 0;
}
