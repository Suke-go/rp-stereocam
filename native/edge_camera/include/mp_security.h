#ifndef MP_SECURITY_H
#define MP_SECURITY_H

#include "mp_edge_config.h"
#include "mp_frame.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MpPrivacyDecision {
    MP_PRIVACY_ALLOW_FRAME = 0,
    MP_PRIVACY_DROP_FRAME = 1,
    MP_PRIVACY_MASK_FULL_FRAME = 2
} MpPrivacyDecision;

typedef struct MpPrivacyState {
    uint8_t mask_ready;
    uint8_t receiver_authenticated;
    uint8_t development_override;
} MpPrivacyState;

MpPrivacyDecision mp_security_privacy_decision(const MpEdgeConfig* config,
                                               const MpPrivacyState* state,
                                               const MpFrame* frame);

#ifdef __cplusplus
}
#endif

#endif

