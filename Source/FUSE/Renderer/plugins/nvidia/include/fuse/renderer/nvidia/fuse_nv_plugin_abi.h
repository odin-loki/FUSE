/*
 * FUSE NVIDIA provider C ABI (MIT, part of FUSE). Contains no NVIDIA code.
 *
 * FUSE never links NVIDIA libraries at build time. At runtime the loader
 * (nv_plugin_loader.hpp) dlopen()s / LoadLibrary()s a *provider* shared library from a
 * user-configured directory and resolves one symbol, FUSE_NV_PLUGIN_ENTRY_NAME. Providers:
 *
 *   fuse_nvplugin_streamline  in-tree, MIT: implements this ABI on top of NVIDIA Streamline and
 *                             loads sl.interposer.dll (Windows) from the same directory.
 *   fuse_nvplugin_ngx         Linux bridge, built by the developer against their own DLSS SDK
 *                             (statically links libnvsdk_ngx.a, so the *binary* is RTX-SDK-licensed
 *                             and is never committed; only its FUSE source is MIT).
 *   fuse_nvplugin_mock        in-tree, MIT: validates and echoes inputs; drives the CI gates.
 *
 * Rules: plain C, fixed-width integers, no enums (size is not ABI-stable) and no bool. Every struct
 * starts with struct_size so a newer host can talk to an older provider within one ABI major.
 * Resources are native API handles carried as uint64_t (VkImage / ID3D12Resource*).
 * Matrices are row-major float[16] (row vector * matrix, as Streamline expects).
 */
#ifndef FUSE_NV_PLUGIN_ABI_H
#define FUSE_NV_PLUGIN_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- versioning ------------------------------------------------------------------------------ */

#define FUSE_NV_PLUGIN_ABI_MAJOR 1u
#define FUSE_NV_PLUGIN_ABI_MINOR 0u
#define FUSE_NV_PLUGIN_ABI_VERSION ((FUSE_NV_PLUGIN_ABI_MAJOR << 16) | FUSE_NV_PLUGIN_ABI_MINOR)
#define FUSE_NV_PLUGIN_ABI_MAJOR_OF(v) (((uint32_t)(v)) >> 16)
#define FUSE_NV_PLUGIN_ENTRY_NAME "fuseNvPluginGetApi"

#if defined(_WIN32)
#define FUSE_NV_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FUSE_NV_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* ---- status ---------------------------------------------------------------------------------- */

typedef uint32_t FuseNvStatus;
#define FUSE_NV_OK 0u
#define FUSE_NV_ERR_ABI_MISMATCH 1u        /* provider built for another ABI major */
#define FUSE_NV_ERR_NOT_INITIALIZED 2u
#define FUSE_NV_ERR_UNSUPPORTED_FEATURE 3u /* feature unknown or not supported on this GPU/driver */
#define FUSE_NV_ERR_NO_NVIDIA_GPU 4u
#define FUSE_NV_ERR_DRIVER_TOO_OLD 5u
#define FUSE_NV_ERR_RUNTIME_MISSING 6u     /* provider found, NVIDIA runtime (interposer/NGX) not */
#define FUSE_NV_ERR_INVALID_ARGUMENT 7u
#define FUSE_NV_ERR_MISSING_INPUT 8u       /* a required resource tag was not set for the frame */
#define FUSE_NV_ERR_MISSING_CONSTANTS 9u   /* evaluate without set_constants for the frame */
#define FUSE_NV_ERR_INVALID_CONSTANTS 10u
#define FUSE_NV_ERR_GRAPHICS_API 11u       /* requested graphics API not supported by provider */
#define FUSE_NV_ERR_RUNTIME_FAILURE 12u    /* the NVIDIA runtime returned an error */

/* ---- features -------------------------------------------------------------------------------- */

typedef uint32_t FuseNvFeature;
#define FUSE_NV_FEATURE_DLSS_SR 0u /* super resolution / DLAA           -> backend "dlss_sr"    */
#define FUSE_NV_FEATURE_DLSS_RR 1u /* ray reconstruction (DLSS-D)        -> backend "dlss_rr"    */
#define FUSE_NV_FEATURE_DLSS_FG 2u /* frame generation (DLSS-G)          -> backend "dlss_fg"    */
#define FUSE_NV_FEATURE_DLSS_NR 3u /* 3D-guided neural rendering, DLSS 5 -> look node "dlss5_look" */
#define FUSE_NV_FEATURE_REFLEX 4u  /* latency markers (FG prerequisite) */
#define FUSE_NV_FEATURE_COUNT 5u
#define FUSE_NV_FEATURE_BIT(f) (1u << (f))

typedef uint32_t FuseNvGraphicsApi;
#define FUSE_NV_API_NONE 0u /* headless: capability queries only (tests, tooling) */
#define FUSE_NV_API_VULKAN 1u
#define FUSE_NV_API_D3D12 2u

/* ---- resources ------------------------------------------------------------------------------- */

typedef uint32_t FuseNvBufferKind;
#define FUSE_NV_BUFFER_COLOR_IN 0u              /* render-res jittered colour (pre-post HDR) */
#define FUSE_NV_BUFFER_COLOR_OUT 1u             /* display-res output (SR/RR) or in-place (NR) */
#define FUSE_NV_BUFFER_DEPTH 2u
#define FUSE_NV_BUFFER_MOTION_VECTORS 3u
#define FUSE_NV_BUFFER_EXPOSURE 4u              /* 1x1; optional (auto-exposure when absent) */
#define FUSE_NV_BUFFER_REACTIVE_MASK 5u
#define FUSE_NV_BUFFER_TRANSPARENCY_MASK 6u
#define FUSE_NV_BUFFER_HUDLESS_COLOR 7u         /* FG */
#define FUSE_NV_BUFFER_UI_COLOR_ALPHA 8u        /* FG */
#define FUSE_NV_BUFFER_DIFFUSE_ALBEDO 9u        /* RR guides ... */
#define FUSE_NV_BUFFER_SPECULAR_ALBEDO 10u
#define FUSE_NV_BUFFER_NORMALS 11u
#define FUSE_NV_BUFFER_ROUGHNESS 12u
#define FUSE_NV_BUFFER_SPECULAR_HIT_DISTANCE 13u
#define FUSE_NV_BUFFER_NEURAL_CONTROL_MASK 14u  /* NR engine mask ("exclude from enhancement") */
#define FUSE_NV_BUFFER_KIND_COUNT 15u

typedef uint32_t FuseNvLifecycle;
#define FUSE_NV_LIFECYCLE_VALID_UNTIL_PRESENT 0u
#define FUSE_NV_LIFECYCLE_VALID_UNTIL_EVALUATE 1u
#define FUSE_NV_LIFECYCLE_ONLY_VALID_NOW 2u

typedef struct FuseNvResource { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint64_t native;        /* VkImage / ID3D12Resource*; 0 = not provided */
    uint64_t view;          /* VkImageView (Vulkan), else 0 */
    uint64_t memory;        /* VkDeviceMemory (Vulkan), else 0 */
    uint32_t native_format; /* VkFormat / DXGI_FORMAT */
    uint32_t width;
    uint32_t height;
    uint32_t state; /* VkImageLayout / D3D12_RESOURCE_STATES */
} FuseNvResource;

typedef struct FuseNvResourceTag { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    FuseNvBufferKind kind;
    FuseNvLifecycle lifecycle;
    FuseNvResource resource;
    uint32_t extent_x; /* sub-rectangle actually used; extent_w == 0 means whole resource */
    uint32_t extent_y;
    uint32_t extent_w;
    uint32_t extent_h;
} FuseNvResourceTag;

/* ---- per-frame constants --------------------------------------------------------------------- */

#define FUSE_NV_CONST_DEPTH_INVERTED (1u << 0)        /* reversed-Z */
#define FUSE_NV_CONST_CAMERA_MOTION_INCLUDED (1u << 1) /* MVs include camera motion */
#define FUSE_NV_CONST_MV_JITTERED (1u << 2)
#define FUSE_NV_CONST_MV_DILATED (1u << 3)
#define FUSE_NV_CONST_RESET (1u << 4)                 /* camera cut / teleport / resize */
#define FUSE_NV_CONST_ORTHOGRAPHIC (1u << 5)
#define FUSE_NV_CONST_HDR (1u << 6)                   /* colour is linear HDR (pre-tonemap) */
#define FUSE_NV_CONST_MV_3D (1u << 7)

typedef struct FuseNvConstants { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    uint32_t frame_index;
    uint32_t flags; /* FUSE_NV_CONST_* */
    uint32_t render_width, render_height;
    uint32_t output_width, output_height;
    float camera_view_to_clip[16]; /* projection without jitter */
    float clip_to_camera_view[16];
    float clip_to_prev_clip[16];
    float prev_clip_to_clip[16];
    float world_to_camera_view[16]; /* RR: world-space normals -> view */
    float camera_view_to_world[16];
    float jitter_offset_px[2]; /* this frame's sub-pixel jitter in render pixels, [-0.5, 0.5] */
    /* Streamline convention: mv * mvec_scale = (previous - current) position in [-1,1] UV units.
     * (NGX's own InMVScale is in pixels; the NGX bridge multiplies by the render size.) */
    float mvec_scale[2];
    float camera_pos[3], camera_up[3], camera_right[3], camera_fwd[3];
    float camera_near, camera_far, camera_vfov_rad, camera_aspect;
    float pre_exposure;   /* colour was multiplied by this before the pass */
    float exposure_scale; /* scalar exposure when no FUSE_NV_BUFFER_EXPOSURE tag is bound */
    float frame_time_ms;
} FuseNvConstants;

/* ---- per-evaluate options -------------------------------------------------------------------- */

typedef uint32_t FuseNvQuality;
#define FUSE_NV_QUALITY_DLAA 0u
#define FUSE_NV_QUALITY_QUALITY 1u
#define FUSE_NV_QUALITY_BALANCED 2u
#define FUSE_NV_QUALITY_PERFORMANCE 3u
#define FUSE_NV_QUALITY_ULTRA_PERFORMANCE 4u
#define FUSE_NV_QUALITY_ULTRA_QUALITY 5u
#define FUSE_NV_QUALITY_COUNT 6u

typedef struct FuseNvFeatureOptions { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    FuseNvFeature feature;
    FuseNvQuality quality;
    uint32_t preset;               /* 0 = runtime default */
    uint32_t frames_to_generate;   /* FG: 1..5 */
    float nr_structure_intensity;  /* NR: 0..1 (high-frequency detail) */
    float nr_tone_intensity;       /* NR: 0..1 (low-frequency lighting / colour) */
    uint32_t nr_model;             /* NR: 0 = runtime default */
} FuseNvFeatureOptions;

/* ---- init / capability ----------------------------------------------------------------------- */

typedef void (*FuseNvLogFn)(void* user, uint32_t level, const char* message); /* 0 info 1 warn 2 error */

typedef struct FuseNvInitInfo { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    uint32_t host_abi_version;
    FuseNvGraphicsApi graphics_api;
    uint32_t application_id; /* NVIDIA-issued id; 0 = development id */
    uint64_t instance;        /* VkInstance */
    uint64_t physical_device; /* VkPhysicalDevice / IDXGIAdapter* */
    uint64_t device;          /* VkDevice / ID3D12Device* */
    const char* runtime_dir;  /* directory the provider was loaded from (UTF-8) */
    const char* options;      /* "key=value;key=value" (FUSE_NVIDIA_PLUGIN_OPTIONS / project) */
    FuseNvLogFn log;
    void* log_user;
} FuseNvInitInfo;

typedef struct FuseNvAdapterInfo { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    uint32_t vendor_id; /* 0x10DE for NVIDIA */
    uint32_t device_id;
    uint32_t rtx_generation; /* 0 = not RTX, 20, 30, 40, 50, ... */
    uint32_t driver_major, driver_minor;
    char name[128];
} FuseNvAdapterInfo;

typedef struct FuseNvFeatureSupport { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    FuseNvFeature feature;
    FuseNvStatus status; /* FUSE_NV_OK = usable now */
    uint32_t min_rtx_generation;
    uint32_t version_major, version_minor, version_patch; /* runtime feature version, if known */
    uint32_t reserved;
} FuseNvFeatureSupport;

typedef struct FuseNvRenderSize { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    uint32_t render_width, render_height;
    uint32_t min_width, min_height, max_width, max_height; /* dynamic-resolution range */
} FuseNvRenderSize;

typedef struct FuseNvContext FuseNvContext; /* opaque, owned by the provider */

typedef struct FuseNvApi { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    uint32_t abi_version;          /* provider's FUSE_NV_PLUGIN_ABI_VERSION */
    const char* provider_name;     /* static string, e.g. "streamline", "ngx", "mock" */
    const char* runtime_version;   /* static string, e.g. "2.14.1" */

    FuseNvStatus (*init)(const FuseNvInitInfo* info, FuseNvContext** out_ctx);
    void (*shutdown)(FuseNvContext* ctx);
    FuseNvStatus (*get_adapter_info)(FuseNvContext* ctx, FuseNvAdapterInfo* out);
    FuseNvStatus (*query_feature)(FuseNvContext* ctx, FuseNvFeature feature, FuseNvFeatureSupport* out);
    FuseNvStatus (*get_render_size)(FuseNvContext* ctx, FuseNvFeature feature, FuseNvQuality quality,
                                    uint32_t display_width, uint32_t display_height, FuseNvRenderSize* out);
    /* Tags/constants are per (frame_index, viewport); evaluate consumes the ones set for that frame. */
    FuseNvStatus (*set_tags)(FuseNvContext* ctx, uint32_t frame_index, uint32_t viewport,
                             const FuseNvResourceTag* tags, uint32_t tag_count, uint64_t command_buffer);
    FuseNvStatus (*set_constants)(FuseNvContext* ctx, uint32_t viewport, const FuseNvConstants* constants);
    FuseNvStatus (*evaluate)(FuseNvContext* ctx, uint32_t frame_index, uint32_t viewport,
                             const FuseNvFeatureOptions* options, uint64_t command_buffer);
    const char* (*status_string)(FuseNvStatus status);
} FuseNvApi;

/* The single exported entry point. Fills *out_api (out_api->struct_size set by the host) and
 * returns FUSE_NV_ERR_ABI_MISMATCH when FUSE_NV_PLUGIN_ABI_MAJOR_OF(host_abi_version) differs.
 * The host re-checks out_api->abi_version itself and never trusts the return code alone. */
typedef FuseNvStatus (*FuseNvPluginGetApiFn)(uint32_t host_abi_version, FuseNvApi* out_api);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FUSE_NV_PLUGIN_ABI_H */
