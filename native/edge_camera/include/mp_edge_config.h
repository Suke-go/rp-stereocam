#ifndef MP_EDGE_CONFIG_H
#define MP_EDGE_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MpMaskMode {
    MP_MASK_SOLID = 0,
    MP_MASK_MOSAIC = 1,
    MP_MASK_GREENBACK_KEEP_HUMAN = 2
} MpMaskMode;

typedef enum MpTransportMode {
    MP_TRANSPORT_RTP_UDP = 0,
    MP_TRANSPORT_WEBRTC = 1
} MpTransportMode;

typedef struct MpEdgeConfig {
    uint32_t eye_width;
    uint32_t eye_height;
    uint32_t fps;
    uint32_t bitrate_kbps;
    uint32_t left_camera_index;
    uint32_t right_camera_index;
    uint32_t frame_queue_capacity;
    uint8_t privacy_enforced;
    uint8_t fail_closed;
    MpMaskMode mask_mode;
    MpTransportMode transport_mode;
} MpEdgeConfig;

MpEdgeConfig mp_edge_config_default(void);
int mp_edge_config_validate(const MpEdgeConfig* config);

#ifdef __cplusplus
}
#endif

#endif
