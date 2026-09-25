/*
 * Minimal declarations of the Intel XeSS Super Resolution runtime entry points FUSE calls (MIT, part of FUSE; WP-4.3).
 *
 * Written from Intel's public XeSS API reference / developer guide (XeSS SDK 1.x-2.x, Vulkan path). The SDK's own
 * headers (xess.h, xess_vk.h) are under the Intel Simplified Software License and are NOT vendored or included here:
 * the types below carry FUSE names, contain only what FUSE uses, and mirror the documented C ABI (plain C structs,
 * 32-bit enums, cdecl entry points exported by libxess). Vulkan handles are declared without <vulkan/vulkan.h>:
 * dispatchable handles (VkInstance, VkPhysicalDevice, VkDevice, VkCommandBuffer) as void*, non-dispatchable ones
 * (VkImage, VkImageView, VkPipelineCache, VkDeviceMemory) as uint64_t (VK_DEFINE_NON_DISPATCHABLE_HANDLE is a
 * pointer on 64-bit targets and uint64_t otherwise: same size and alignment on the supported 64-bit platforms).
 *
 * The runtime library is never linked: XessRuntime (xess_upscaler.hpp) opens libxess (libxess.dll / libxess.so) from a
 * user-configured directory and resolves these names with GetProcAddress / dlsym. Configuring with FUSE_XESS_SDK_DIR
 * pointing at a developer's own SDK checkout compiles xess_layout_check.cpp, which static_asserts every mirror below
 * against the real headers (never in CI; the SDK is not redistributable here).
 */
#ifndef FUSE_XESS_API_H
#define FUSE_XESS_API_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define FUSE_XESS_CALL __cdecl
#else
#define FUSE_XESS_CALL
#endif

/* xess_result_t */
typedef int32_t FuseXessResult;
#define FUSE_XESS_RESULT_WARNING_NONEXISTING_FOLDER 1
#define FUSE_XESS_RESULT_WARNING_OLD_DRIVER 2
#define FUSE_XESS_RESULT_SUCCESS 0
#define FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DEVICE (-1)
#define FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DRIVER (-2)
#define FUSE_XESS_RESULT_ERROR_UNINITIALIZED (-3)
#define FUSE_XESS_RESULT_ERROR_INVALID_ARGUMENT (-4)
#define FUSE_XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY (-5)
#define FUSE_XESS_RESULT_ERROR_DEVICE (-6)
#define FUSE_XESS_RESULT_ERROR_NOT_IMPLEMENTED (-7)
#define FUSE_XESS_RESULT_ERROR_INVALID_CONTEXT (-8)
#define FUSE_XESS_RESULT_ERROR_OPERATION_IN_PROGRESS (-9)
#define FUSE_XESS_RESULT_ERROR_UNSUPPORTED (-10)
#define FUSE_XESS_RESULT_ERROR_CANT_LOAD_LIBRARY (-11)
#define FUSE_XESS_RESULT_ERROR_UNKNOWN (-1000)

/* xess_quality_settings_t (per-axis ratios from the XeSS 1.3+ developer guide) */
typedef int32_t FuseXessQuality;
#define FUSE_XESS_QUALITY_ULTRA_PERFORMANCE 100 /* 3.0x */
#define FUSE_XESS_QUALITY_PERFORMANCE 101       /* 2.3x */
#define FUSE_XESS_QUALITY_BALANCED 102          /* 2.0x */
#define FUSE_XESS_QUALITY_QUALITY 103           /* 1.7x */
#define FUSE_XESS_QUALITY_ULTRA_QUALITY 104     /* 1.5x */
#define FUSE_XESS_QUALITY_ULTRA_QUALITY_PLUS 105 /* 1.3x */
#define FUSE_XESS_QUALITY_AA 106                /* 1.0x (native anti-aliasing) */

/* xess_init_flags_t */
#define FUSE_XESS_INIT_FLAG_NONE 0u
#define FUSE_XESS_INIT_FLAG_HIGH_RES_MV (1u << 0)
#define FUSE_XESS_INIT_FLAG_INVERTED_DEPTH (1u << 1)
#define FUSE_XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE (1u << 2)
#define FUSE_XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK (1u << 3)
#define FUSE_XESS_INIT_FLAG_USE_NDC_VELOCITY (1u << 4)
#define FUSE_XESS_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP (1u << 5)
#define FUSE_XESS_INIT_FLAG_LDR_INPUT_COLOR (1u << 6)
#define FUSE_XESS_INIT_FLAG_JITTERED_MV (1u << 7)
#define FUSE_XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE (1u << 8)

typedef struct FuseXessContext_T* FuseXessContext; /* xess_context_handle_t */

typedef struct FuseXess2d { /* xess_2d_t */ /* fuse-lint-allow(namespace): C ABI, must be global */
    uint32_t x;
    uint32_t y;
} FuseXess2d;

typedef FuseXess2d FuseXessCoord; /* xess_coord_t */

typedef struct FuseXessVersion { /* xess_version_t */ /* fuse-lint-allow(namespace): C ABI, must be global */
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    uint16_t reserved;
} FuseXessVersion;

typedef struct FuseXessVkSubresourceRange { /* VkImageSubresourceRange */ /* fuse-lint-allow(namespace): C ABI, must be global */
    uint32_t aspect_mask;
    uint32_t base_mip_level;
    uint32_t level_count;
    uint32_t base_array_layer;
    uint32_t layer_count;
} FuseXessVkSubresourceRange;

typedef struct FuseXessVkImageViewInfo { /* xess_vk_image_view_info */ /* fuse-lint-allow(namespace): C ABI, must be global */
    uint64_t image_view; /* VkImageView */
    uint64_t image;      /* VkImage */
    FuseXessVkSubresourceRange subresource_range;
    int32_t format;      /* VkFormat */
    uint32_t width;
    uint32_t height;
} FuseXessVkImageViewInfo;

typedef struct FuseXessVkInitParams { /* xess_vk_init_params_t */ /* fuse-lint-allow(namespace): C ABI, must be global */
    FuseXess2d output_resolution;
    FuseXessQuality quality_setting;
    uint32_t init_flags;
    uint32_t creation_node_mask;
    uint32_t visible_node_mask;
    uint64_t temp_buffer_heap;  /* VkDeviceMemory, 0 = runtime allocates */
    uint64_t buffer_heap_offset;
    uint64_t temp_texture_heap; /* VkDeviceMemory, 0 = runtime allocates */
    uint64_t texture_heap_offset;
    uint64_t pipeline_cache;    /* VkPipelineCache */
} FuseXessVkInitParams;

typedef struct FuseXessVkExecuteParams { /* xess_vk_execute_params_t */ /* fuse-lint-allow(namespace): C ABI, must be global */
    FuseXessVkImageViewInfo color_texture;
    FuseXessVkImageViewInfo velocity_texture;
    FuseXessVkImageViewInfo depth_texture;
    FuseXessVkImageViewInfo exposure_scale_texture;
    FuseXessVkImageViewInfo responsive_pixel_mask_texture;
    FuseXessVkImageViewInfo output_texture;
    float jitter_offset_x;
    float jitter_offset_y;
    float exposure_scale;
    uint32_t reset_history;
    uint32_t input_width;
    uint32_t input_height;
    FuseXessCoord input_color_base;
    FuseXessCoord input_motion_vector_base;
    FuseXessCoord input_depth_base;
    FuseXessCoord input_responsive_mask_base;
    FuseXessCoord reserved0;
    FuseXessCoord output_color_base;
} FuseXessVkExecuteParams;

/* Entry points (exported names in the comments; resolved at run time, never linked). */
typedef FuseXessResult(FUSE_XESS_CALL* PFN_fuseXessGetVersion)(FuseXessVersion* version);                  /* xessGetVersion */
typedef FuseXessResult(FUSE_XESS_CALL* PFN_fuseXessDestroyContext)(FuseXessContext ctx);                    /* xessDestroyContext */
typedef FuseXessResult(FUSE_XESS_CALL* PFN_fuseXessGetInputResolution)(FuseXessContext ctx, const FuseXess2d* output,
                                                                      FuseXessQuality quality, FuseXess2d* input); /* xessGetInputResolution */
typedef FuseXessResult(FUSE_XESS_CALL* PFN_fuseXessSetVelocityScale)(FuseXessContext ctx, float x, float y); /* xessSetVelocityScale */
typedef FuseXessResult(FUSE_XESS_CALL* PFN_fuseXessSetJitterScale)(FuseXessContext ctx, float x, float y);   /* xessSetJitterScale */
typedef FuseXessResult(FUSE_XESS_CALL* PFN_fuseXessSetExposureMultiplier)(FuseXessContext ctx, float scale); /* xessSetExposureMultiplier */
typedef FuseXessResult(FUSE_XESS_CALL* PFN_fuseXessVkCreateContext)(void* instance, void* physical_device, void* device,
                                                                   FuseXessContext* out_ctx);           /* xessVKCreateContext */
typedef FuseXessResult(FUSE_XESS_CALL* PFN_fuseXessVkBuildPipelines)(FuseXessContext ctx, uint64_t pipeline_cache,
                                                                    bool blocking, uint32_t init_flags); /* xessVKBuildPipelines */
typedef FuseXessResult(FUSE_XESS_CALL* PFN_fuseXessVkInit)(FuseXessContext ctx, const FuseXessVkInitParams* params); /* xessVKInit */
typedef FuseXessResult(FUSE_XESS_CALL* PFN_fuseXessVkExecute)(FuseXessContext ctx, void* command_buffer,
                                                             const FuseXessVkExecuteParams* params);    /* xessVKExecute */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FUSE_XESS_API_H */
