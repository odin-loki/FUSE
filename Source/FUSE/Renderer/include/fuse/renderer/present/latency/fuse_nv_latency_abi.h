/*
 * FUSE NVIDIA provider latency extension, C ABI (MIT, part of FUSE). Contains no NVIDIA code. WP-4.4.
 *
 * NVIDIA Reflex reaches FUSE only through the runtime provider of docs/nvidia-plugin.md (fuse_nv_plugin_abi.h):
 * FUSE never links Streamline / NVAPI / the Reflex SDK. ABI 1.0 of that plugin reports FUSE_NV_FEATURE_REFLEX
 * but has no entry points for it; this header is the optional, separately versioned extension a provider may
 * export next to fuseNvPluginGetApi:
 *
 *   FuseNvStatus fuseNvPluginGetLatencyApi(uint32_t host_abi_version, FuseNvLatencyApi* out_api);
 *
 * resolved by the host with NvPlugin::symbol(FUSE_NV_LATENCY_ENTRY_NAME) after a successful plugin load, and
 * called with the provider's own FuseNvContext. The Streamline provider maps it onto sl::reflexSetOptions /
 * slReflexSleep / slPCLSetMarker / slReflexGetState (and the NGX bridge onto VK_NV_low_latency2); the in-process
 * mock of fuse_rp_latency_cpu drives the gates. A provider without the export simply has no Reflex backend.
 *
 * Rules as in fuse_nv_plugin_abi.h: plain C, fixed-width integers, no enums, no bool, struct_size first.
 * Marker values are VkLatencyMarkerNV / sl::PCLMarker (they agree for 0..11).
 */
#ifndef FUSE_NV_LATENCY_ABI_H
#define FUSE_NV_LATENCY_ABI_H

#include <fuse/renderer/nvidia/fuse_nv_plugin_abi.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FUSE_NV_LATENCY_ABI_MAJOR 1u
#define FUSE_NV_LATENCY_ABI_MINOR 0u
#define FUSE_NV_LATENCY_ABI_VERSION ((FUSE_NV_LATENCY_ABI_MAJOR << 16) | FUSE_NV_LATENCY_ABI_MINOR)
#define FUSE_NV_LATENCY_ENTRY_NAME "fuseNvPluginGetLatencyApi"

typedef uint32_t FuseNvLatencyMarker;
#define FUSE_NV_LATENCY_MARKER_SIMULATION_START 0u
#define FUSE_NV_LATENCY_MARKER_SIMULATION_END 1u
#define FUSE_NV_LATENCY_MARKER_RENDERSUBMIT_START 2u
#define FUSE_NV_LATENCY_MARKER_RENDERSUBMIT_END 3u
#define FUSE_NV_LATENCY_MARKER_PRESENT_START 4u
#define FUSE_NV_LATENCY_MARKER_PRESENT_END 5u
#define FUSE_NV_LATENCY_MARKER_INPUT_SAMPLE 6u
#define FUSE_NV_LATENCY_MARKER_TRIGGER_FLASH 7u
#define FUSE_NV_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_START 8u
#define FUSE_NV_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_END 9u
#define FUSE_NV_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START 10u
#define FUSE_NV_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END 11u
#define FUSE_NV_LATENCY_MARKER_COUNT 12u

typedef struct FuseNvLatencyOptions { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    uint32_t low_latency;             /* 0 off, 1 on (Reflex "On") */
    uint32_t boost;                   /* 1: Reflex "On + Boost" (requires low_latency) */
    uint32_t minimum_interval_us;     /* frame limiter; 0 = none */
    uint32_t use_markers_to_optimize; /* Reflex marker-based pacing */
    uint32_t reserved;
} FuseNvLatencyOptions;

/* One frame of latency statistics (microsecond timestamps on the provider's clock, 0 = not recorded). */
typedef struct FuseNvLatencyFrameReport { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t frame_id;
    uint64_t input_sample_us;
    uint64_t sim_start_us, sim_end_us;
    uint64_t render_submit_start_us, render_submit_end_us;
    uint64_t present_start_us, present_end_us;
    uint64_t driver_start_us, driver_end_us;
    uint64_t os_render_queue_start_us, os_render_queue_end_us;
    uint64_t gpu_render_start_us, gpu_render_end_us;
} FuseNvLatencyFrameReport;

typedef struct FuseNvLatencyApi { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    uint32_t abi_version; /* provider's FUSE_NV_LATENCY_ABI_VERSION */
    FuseNvStatus (*set_options)(FuseNvContext* ctx, const FuseNvLatencyOptions* options);
    /* Frame start: blocks until the provider releases the CPU for `frame_id` (Reflex sleep). */
    FuseNvStatus (*sleep)(FuseNvContext* ctx, uint64_t frame_id);
    FuseNvStatus (*set_marker)(FuseNvContext* ctx, uint64_t frame_id, FuseNvLatencyMarker marker);
    /* Copies up to `capacity` of the most recent completed frames, oldest first; *out_count = copied. */
    FuseNvStatus (*get_reports)(FuseNvContext* ctx, FuseNvLatencyFrameReport* out, uint32_t capacity, uint32_t* out_count);
} FuseNvLatencyApi;

/* Fills *out_api (out_api->struct_size set by the host). FUSE_NV_ERR_ABI_MISMATCH when
 * FUSE_NV_PLUGIN_ABI_MAJOR_OF(host_abi_version) != FUSE_NV_LATENCY_ABI_MAJOR. */
typedef FuseNvStatus (*FuseNvPluginGetLatencyApiFn)(uint32_t host_abi_version, FuseNvLatencyApi* out_api);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FUSE_NV_LATENCY_ABI_H */
