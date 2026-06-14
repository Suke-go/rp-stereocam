#include "mp_security.h"

MpPrivacyDecision mp_security_privacy_decision(const MpEdgeConfig* config,
                                               const MpPrivacyState* state,
                                               const MpFrame* frame)
{
    if (!config || !state || !frame || !frame->data) {
        return MP_PRIVACY_DROP_FRAME;
    }

    if (!config->privacy_enforced && state->development_override) {
        return state->receiver_authenticated ? MP_PRIVACY_ALLOW_FRAME : MP_PRIVACY_DROP_FRAME;
    }

    if (!state->receiver_authenticated) {
        return MP_PRIVACY_DROP_FRAME;
    }

    if (!config->privacy_enforced) {
        return MP_PRIVACY_ALLOW_FRAME;
    }

    if (state->mask_ready) {
        return MP_PRIVACY_ALLOW_FRAME;
    }

    return config->fail_closed ? MP_PRIVACY_MASK_FULL_FRAME : MP_PRIVACY_DROP_FRAME;
}

