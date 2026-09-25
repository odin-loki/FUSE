/* Test-only exports of the mock NRD provider (MIT, part of FUSE). */
#ifndef FUSE_NRD_MOCK_ECHO_H
#define FUSE_NRD_MOCK_ECHO_H

#include <fuse/renderer/nrd/fuse_nrd_plugin_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FuseNrdMockEcho { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t init_calls, shutdown_calls;
    uint32_t create_calls, destroy_calls, live_instances;
    uint32_t common_calls, settings_calls, dispatch_calls;
    uint32_t last_create_denoiser, last_create_width, last_create_height;
    FuseNrdCommonSettings last_common;
    FuseNrdDenoiserSettings last_settings;
    FuseNrdResourceBinding last_bindings[FUSE_NRD_SLOT_COUNT];
    uint32_t last_binding_count;
    uint32_t last_dispatch_denoiser;
    uint64_t last_command_buffer;
    FuseNrdStatus last_dispatch_status;
    uint64_t output_signature; /* deterministic function of what the last successful dispatch consumed */
} FuseNrdMockEcho;

typedef void (*FuseNrdMockEchoFn)(FuseNrdMockEcho* out);
typedef void (*FuseNrdMockResetFn)(void);
#define FUSE_NRD_MOCK_ECHO_NAME "fuseNrdMockEcho"
#define FUSE_NRD_MOCK_RESET_NAME "fuseNrdMockReset"

#ifdef __cplusplus
}
#endif

#endif /* FUSE_NRD_MOCK_ECHO_H */
