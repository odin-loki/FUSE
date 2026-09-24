/*
 * Test-only extension of the mock NVIDIA provider (MIT). Not part of the provider ABI: the gates
 * resolve these extra exports with NvPlugin::symbol() to see exactly what reached the provider.
 */
#ifndef FUSE_NV_MOCK_ECHO_H
#define FUSE_NV_MOCK_ECHO_H

#include <fuse/renderer/nvidia/fuse_nv_plugin_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FUSE_NV_MOCK_ECHO_NAME "fuseNvMockGetEcho"
#define FUSE_NV_MOCK_LIVE_NAME "fuseNvMockLiveContexts"

typedef struct FuseNvMockEcho {
    uint32_t init_count;
    uint32_t set_tags_count;
    uint32_t set_constants_count;
    uint32_t evaluate_count;       /* successful evaluates */
    uint32_t rejected_count;       /* evaluates the mock refused */
    FuseNvStatus last_status;      /* status of the last evaluate */
    FuseNvFeature last_feature;
    uint32_t last_frame_index;
    uint32_t last_viewport;
    uint32_t last_tag_mask;        /* 1u << FuseNvBufferKind of tags seen for that frame */
    FuseNvBufferKind last_missing; /* first missing required kind, FUSE_NV_BUFFER_KIND_COUNT if none */
    uint32_t last_flags;
    uint32_t last_render_width, last_render_height, last_output_width, last_output_height;
    float last_jitter_px[2];
    float last_mvec_scale[2];
    float last_pre_exposure;
    FuseNvQuality last_quality;
    uint32_t last_frames_to_generate;
    float last_nr_structure_intensity, last_nr_tone_intensity;
    uint64_t last_color_in_native; /* echo of the COLOR_IN handle, proves tags are not reordered */
    uint32_t pending_frames;       /* frames with tags/constants still buffered */
    uint32_t graphics_api;
} FuseNvMockEcho;

typedef void (*FuseNvMockGetEchoFn)(FuseNvMockEcho* out);
typedef uint32_t (*FuseNvMockLiveContextsFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_NV_MOCK_ECHO_H */
