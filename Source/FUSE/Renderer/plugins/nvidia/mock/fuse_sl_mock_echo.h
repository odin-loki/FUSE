/*
 * Test-only: what the mock Streamline interposer (fuse_sl_mock_interposer.cpp, MIT) received from the
 * FUSE Streamline provider. Resolved by the gates with dlsym on the interposer module.
 */
#ifndef FUSE_SL_MOCK_ECHO_H
#define FUSE_SL_MOCK_ECHO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FUSE_SL_MOCK_ECHO_NAME "fuseSlMockGetEcho"
#define FUSE_SL_MOCK_MAX_TAGS 32u

typedef struct FuseSlMockEcho {
    uint32_t init_count, shutdown_count;
    uint64_t sdk_version;
    uint32_t render_api;        /* sl::RenderAPI */
    uint64_t preference_flags;  /* sl::PreferenceFlags */
    uint32_t num_features_to_load;
    uint32_t num_plugin_paths;
    uint32_t application_id;
    uint32_t set_tag_calls;
    uint32_t last_tag_frame, last_tag_viewport, last_tag_count;
    uint32_t last_tag_types[FUSE_SL_MOCK_MAX_TAGS]; /* sl::BufferType */
    uint64_t last_tag_native[FUSE_SL_MOCK_MAX_TAGS];
    uint32_t last_tag_width[FUSE_SL_MOCK_MAX_TAGS];
    uint32_t set_constants_calls;
    uint32_t last_constants_frame;
    float last_jitter[2], last_mvec_scale[2];
    int32_t last_depth_inverted, last_reset, last_camera_motion_included; /* sl::Boolean */
    float last_view_to_clip_00, last_prev_clip_to_clip_30, last_clip_to_lens_clip_11;
    uint32_t evaluate_calls;
    uint32_t last_eval_feature, last_eval_frame, last_eval_input_count;
    uint32_t last_eval_local_tag_count;
    uint32_t last_eval_local_tag_types[FUSE_SL_MOCK_MAX_TAGS];
    uint32_t dlss_options_calls, last_dlss_mode, last_dlss_output_w, last_dlss_output_h;
    int32_t last_dlss_hdr, last_dlss_auto_exposure;
    float last_dlss_pre_exposure;
    uint32_t dlssd_options_calls;
    float last_dlssd_world_to_view_30;
    uint32_t dlssg_options_calls, last_dlssg_mode, last_dlssg_frames;
} FuseSlMockEcho;

typedef void (*FuseSlMockGetEchoFn)(FuseSlMockEcho* out);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_SL_MOCK_ECHO_H */
