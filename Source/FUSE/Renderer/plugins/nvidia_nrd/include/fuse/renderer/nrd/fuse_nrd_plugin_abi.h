/*
 * FUSE NRD provider C ABI (MIT, part of FUSE; WP-6.4b). Contains no NVIDIA code and no NRD header.
 *
 * FUSE never links NVIDIA Real-time Denoisers (NRD, NVIDIA RTX SDKs License). At runtime the loader
 * (nrd_denoiser.hpp) dlopen()s / LoadLibrary()s a *provider* shared library from a user-configured directory
 * (project setting, else env FUSE_NRD_SDK_DIR) and resolves one symbol, FUSE_NRD_PLUGIN_ENTRY_NAME.
 *
 *   fuse_nrdplugin_nri    provider built by the developer against their own NRD + NRI checkout (it links NRD,
 *                         so its binary is RTX-SDK-licensed and is never committed). Not in this repository yet
 *                         (WP-6.4b Open): this header is its contract.
 *   fuse_nrdplugin_mock   in-tree, MIT: validates and echoes inputs, returns deterministic results; CI gates.
 *
 * Rules (same as fuse_nv_plugin_abi.h): plain C, fixed-width integers, no enums and no bool, every struct
 * starts with struct_size. Resources are native API handles carried as uint64_t (VkImage / VkImageView).
 * Matrices are column-major float[16] for column vectors (NRD's convention; FUSE math::Mat4 storage as is).
 */
#ifndef FUSE_NRD_PLUGIN_ABI_H
#define FUSE_NRD_PLUGIN_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FUSE_NRD_PLUGIN_ABI_MAJOR 1u
#define FUSE_NRD_PLUGIN_ABI_MINOR 0u
#define FUSE_NRD_PLUGIN_ABI_VERSION ((FUSE_NRD_PLUGIN_ABI_MAJOR << 16) | FUSE_NRD_PLUGIN_ABI_MINOR)
#define FUSE_NRD_PLUGIN_ABI_MAJOR_OF(v) (((uint32_t)(v)) >> 16)
#define FUSE_NRD_PLUGIN_ENTRY_NAME "fuseNrdPluginGetApi"

#if defined(_WIN32)
#define FUSE_NRD_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FUSE_NRD_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* ---- status ---------------------------------------------------------------------------------- */

typedef uint32_t FuseNrdStatus;
#define FUSE_NRD_OK 0u
#define FUSE_NRD_ERR_ABI_MISMATCH 1u
#define FUSE_NRD_ERR_NOT_INITIALIZED 2u
#define FUSE_NRD_ERR_UNSUPPORTED_DENOISER 3u
#define FUSE_NRD_ERR_RUNTIME_MISSING 4u   /* provider found, NRD / NRI libraries not */
#define FUSE_NRD_ERR_GRAPHICS_API 5u      /* requested graphics API not supported */
#define FUSE_NRD_ERR_INVALID_ARGUMENT 6u
#define FUSE_NRD_ERR_MISSING_INPUT 7u     /* a required resource slot is not bound */
#define FUSE_NRD_ERR_MISSING_SETTINGS 8u  /* dispatch without common settings for the frame */
#define FUSE_NRD_ERR_RUNTIME_FAILURE 9u

/* ---- denoisers ------------------------------------------------------------------------------- */

typedef uint32_t FuseNrdDenoiser;
#define FUSE_NRD_DENOISER_SIGMA_SHADOW 0u    /* nrd::Denoiser::SIGMA_SHADOW */
#define FUSE_NRD_DENOISER_REBLUR_DIFFUSE 1u  /* nrd::Denoiser::REBLUR_DIFFUSE */
#define FUSE_NRD_DENOISER_REBLUR_SPECULAR 2u /* nrd::Denoiser::REBLUR_SPECULAR */
#define FUSE_NRD_DENOISER_RELAX_DIFFUSE 3u   /* nrd::Denoiser::RELAX_DIFFUSE */
#define FUSE_NRD_DENOISER_RELAX_SPECULAR 4u  /* nrd::Denoiser::RELAX_SPECULAR */
#define FUSE_NRD_DENOISER_COUNT 5u
#define FUSE_NRD_DENOISER_BIT(d) (1u << (d))

typedef uint32_t FuseNrdGraphicsApi;
#define FUSE_NRD_API_NONE 0u /* headless: capability queries only */
#define FUSE_NRD_API_VULKAN 1u
#define FUSE_NRD_API_D3D12 2u

/* ---- resources ------------------------------------------------------------------------------- */

typedef uint32_t FuseNrdSlot;
#define FUSE_NRD_IN_MV 0u                     /* motion (see FuseNrdCommonSettings::motion_vector_scale) */
#define FUSE_NRD_IN_NORMAL_ROUGHNESS 1u       /* NRD-packed normal + roughness */
#define FUSE_NRD_IN_VIEWZ 2u                  /* linear view Z */
#define FUSE_NRD_IN_DIFF_RADIANCE_HITDIST 3u  /* REBLUR / RELAX diffuse */
#define FUSE_NRD_IN_SPEC_RADIANCE_HITDIST 4u  /* REBLUR / RELAX specular */
#define FUSE_NRD_IN_PENUMBRA 5u               /* SIGMA */
#define FUSE_NRD_OUT_DIFF_RADIANCE_HITDIST 6u
#define FUSE_NRD_OUT_SPEC_RADIANCE_HITDIST 7u
#define FUSE_NRD_OUT_SHADOW_TRANSLUCENCY 8u
#define FUSE_NRD_OUT_VALIDATION 9u            /* optional debug overlay */
#define FUSE_NRD_SLOT_COUNT 10u
#define FUSE_NRD_SLOT_BIT(s) (1u << (s))

typedef struct FuseNrdResource { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint64_t native;        /* VkImage / ID3D12Resource*; 0 = not bound */
    uint64_t view;          /* VkImageView (Vulkan), else 0 */
    uint32_t native_format; /* VkFormat / DXGI_FORMAT */
    uint32_t width;
    uint32_t height;
    uint32_t layout;        /* VkImageLayout / D3D12_RESOURCE_STATES the image is in when dispatch runs */
} FuseNrdResource;

typedef struct FuseNrdResourceBinding { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    FuseNrdSlot slot;
    uint32_t reserved;
    FuseNrdResource resource;
} FuseNrdResourceBinding;

/* ---- per-frame common settings (nrd::CommonSettings subset) ---------------------------------- */

#define FUSE_NRD_COMMON_RESET (1u << 0)                /* accumulationMode = CLEAR_AND_RESTART */
#define FUSE_NRD_COMMON_MV_WORLD_SPACE (1u << 1)       /* isMotionVectorInWorldSpace */
#define FUSE_NRD_COMMON_VALIDATION (1u << 2)           /* enableValidation (OUT_VALIDATION bound) */

typedef struct FuseNrdCommonSettings { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    uint32_t frame_index;
    uint32_t flags; /* FUSE_NRD_COMMON_* */
    uint32_t resource_width, resource_height; /* resourceSize (== resourceSizePrev unless resized) */
    uint32_t rect_width, rect_height;         /* rectSize: the denoised sub-rectangle */
    float view_to_clip[16];       /* unjittered projection */
    float view_to_clip_prev[16];
    float world_to_view[16];
    float world_to_view_prev[16];
    /* NRD: pixelUvPrev = pixelUv + mv.xy * motion_vector_scale.xy (2D). FUSE motion is uv_cur - uv_prev,
     * so the adapter passes (-1, -1, 0). */
    float motion_vector_scale[3];
    float camera_jitter[2];       /* render-pixel sample jitter of this frame, [-0.5, 0.5] */
    float camera_jitter_prev[2];
    float resolution_scale[2];    /* rect / resource */
    float denoising_range;        /* viewZ beyond this is sky / not denoised */
    float disocclusion_threshold; /* relative depth */
    float time_delta_ms;
} FuseNrdCommonSettings;

/* ---- per-denoiser settings (the fields FUSE maps; the provider keeps NRD defaults for the rest) ---- */

typedef struct FuseNrdDenoiserSettings { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    FuseNrdDenoiser denoiser;
    uint32_t max_accumulated_frames;      /* REBLUR / RELAX maxAccumulatedFrameNum (SIGMA: ignored) */
    uint32_t max_fast_accumulated_frames; /* REBLUR / RELAX maxFastAccumulatedFrameNum */
    uint32_t anti_firefly;                /* 0 / 1: enableAntiFirefly */
    uint32_t atrous_iterations;           /* RELAX atrousIterationNum (0 = default) */
    float hit_distance_a, hit_distance_b, hit_distance_c; /* REBLUR hitDistanceParameters (A, B, C) */
    float plane_distance_sensitivity;     /* SIGMA planeDistanceSensitivity (0 = default) */
} FuseNrdDenoiserSettings;

/* ---- init / capability ----------------------------------------------------------------------- */

typedef void (*FuseNrdLogFn)(void* user, uint32_t level, const char* message);

typedef struct FuseNrdInitInfo { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    uint32_t host_abi_version;
    FuseNrdGraphicsApi graphics_api;
    uint32_t reserved;
    uint64_t instance;        /* VkInstance */
    uint64_t physical_device; /* VkPhysicalDevice */
    uint64_t device;          /* VkDevice / ID3D12Device* */
    const char* runtime_dir;  /* directory the provider was loaded from (UTF-8) */
    const char* options;      /* "key=value;key=value" (FUSE_NRD_PLUGIN_OPTIONS / project) */
    FuseNrdLogFn log;
    void* log_user;
} FuseNrdInitInfo;

typedef struct FuseNrdDenoiserSupport { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    FuseNrdDenoiser denoiser;
    FuseNrdStatus status; /* FUSE_NRD_OK = usable */
    uint32_t required_inputs; /* FUSE_NRD_SLOT_BIT set of required IN_* slots */
    uint32_t outputs;         /* FUSE_NRD_SLOT_BIT set of written OUT_* slots */
    uint32_t version_major, version_minor, version_patch;
} FuseNrdDenoiserSupport;

typedef struct FuseNrdContext FuseNrdContext;   /* opaque, provider-owned */
typedef struct FuseNrdInstance FuseNrdInstance; /* one denoiser at one resource size */

typedef struct FuseNrdApi { /* fuse-lint-allow(namespace): C ABI plugin boundary, must be global */
    uint32_t struct_size;
    uint32_t abi_version;
    const char* provider_name;   /* static string: "nri", "mock" */
    const char* runtime_version; /* static string: NRD version, e.g. "4.x" */

    FuseNrdStatus (*init)(const FuseNrdInitInfo* info, FuseNrdContext** out_ctx);
    void (*shutdown)(FuseNrdContext* ctx);
    FuseNrdStatus (*query_denoiser)(FuseNrdContext* ctx, FuseNrdDenoiser denoiser, FuseNrdDenoiserSupport* out);
    /* Instances own NRD's internal pools; created on resize / method change, never per frame. */
    FuseNrdStatus (*create_instance)(FuseNrdContext* ctx, FuseNrdDenoiser denoiser, uint32_t width, uint32_t height,
                                     FuseNrdInstance** out_instance);
    void (*destroy_instance)(FuseNrdContext* ctx, FuseNrdInstance* instance);
    FuseNrdStatus (*set_common_settings)(FuseNrdInstance* instance, const FuseNrdCommonSettings* settings);
    FuseNrdStatus (*set_denoiser_settings)(FuseNrdInstance* instance, const FuseNrdDenoiserSettings* settings);
    /* Records NRD's dispatches into command_buffer (VkCommandBuffer); consumes this frame's common settings. */
    FuseNrdStatus (*dispatch)(FuseNrdInstance* instance, const FuseNrdResourceBinding* bindings, uint32_t binding_count,
                              uint64_t command_buffer);
    const char* (*status_string)(FuseNrdStatus status);
} FuseNrdApi;

typedef FuseNrdStatus (*FuseNrdPluginGetApiFn)(uint32_t host_abi_version, FuseNrdApi* out_api);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FUSE_NRD_PLUGIN_ABI_H */
