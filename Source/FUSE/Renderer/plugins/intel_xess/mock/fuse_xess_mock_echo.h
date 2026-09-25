/* Test-only exports of the mock libxess (MIT, part of FUSE). */
#ifndef FUSE_XESS_MOCK_ECHO_H
#define FUSE_XESS_MOCK_ECHO_H

#include <fuse/renderer/xess/xess_api.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FuseXessMockEcho { /* fuse-lint-allow(namespace): C ABI test hook, must be global */
    uint32_t create_calls, destroy_calls, live_contexts;
    uint32_t build_calls, init_calls, execute_calls;
    uint32_t velocity_calls, jitter_scale_calls, exposure_multiplier_calls, input_resolution_calls;
    void* last_instance;
    void* last_physical_device;
    void* last_device;
    uint32_t last_build_flags;
    uint32_t last_build_blocking;
    FuseXessVkInitParams last_init;
    FuseXessVkExecuteParams last_execute;
    void* last_command_buffer;
    float velocity_scale[2];
    float jitter_scale[2];
    float exposure_multiplier;
    uint64_t output_signature; /* deterministic function of the last successful execute */
} FuseXessMockEcho;

/* Results the next calls return (0 = success). */
typedef struct FuseXessMockControl { /* fuse-lint-allow(namespace): C ABI test hook, must be global */
    FuseXessResult create_result;
    FuseXessResult init_result;
    FuseXessResult execute_result;
} FuseXessMockControl;

typedef void (*FuseXessMockEchoFn)(FuseXessMockEcho* out);
typedef void (*FuseXessMockControlFn)(const FuseXessMockControl* control);
typedef void (*FuseXessMockResetFn)(void);
#define FUSE_XESS_MOCK_ECHO_NAME "fuseXessMockEcho"
#define FUSE_XESS_MOCK_CONTROL_NAME "fuseXessMockControl"
#define FUSE_XESS_MOCK_RESET_NAME "fuseXessMockReset"

#ifdef __cplusplus
}
#endif

#endif /* FUSE_XESS_MOCK_ECHO_H */
