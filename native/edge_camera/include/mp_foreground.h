#ifndef MP_FOREGROUND_H
#define MP_FOREGROUND_H

#include "mp_frame.h"
#include "mp_mask.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MpForegroundConfig {
    MpGreenbackKeyConfig greenback;
    uint8_t background_grow_iterations;
    uint8_t mask_threshold;
} MpForegroundConfig;

typedef struct MpForegroundWorkspace {
    uint8_t* mask;
    uint8_t* scratch;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    size_t buffer_size;
} MpForegroundWorkspace;

MpForegroundConfig mp_foreground_config_default(void);
int mp_foreground_workspace_init(MpForegroundWorkspace* workspace, uint32_t width, uint32_t height);
void mp_foreground_workspace_destroy(MpForegroundWorkspace* workspace);
int mp_foreground_greenback_remove_nv12(MpFrame* nv12_frame,
                                        MpForegroundWorkspace* workspace,
                                        const MpForegroundConfig* config);

#ifdef __cplusplus
}
#endif

#endif

