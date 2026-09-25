// Developer-only layout check (MIT, part of FUSE; WP-4.3). Compiled only when FUSE_XESS_SDK_DIR points at the
// developer's own XeSS SDK checkout (cmake/rp_wp43.cmake); never in CI, and the SDK headers are never committed.
// It pins every mirror in xess_api.h against Intel's real declarations, so a mismatch fails the build instead of
// corrupting parameters on hardware.
#include <fuse/renderer/xess/xess_api.h>

#include <cstddef>

#include <vulkan/vulkan.h>
#include <xess/xess.h>
#include <xess/xess_vk.h>

#define FUSE_XESS_SAME(fuse_t, xess_t) \
    static_assert(sizeof(fuse_t) == sizeof(xess_t) && alignof(fuse_t) == alignof(xess_t), #fuse_t " vs " #xess_t)
#define FUSE_XESS_AT(fuse_t, fuse_f, xess_t, xess_f) \
    static_assert(offsetof(fuse_t, fuse_f) == offsetof(xess_t, xess_f), #fuse_t "::" #fuse_f " vs " #xess_t "::" #xess_f)

FUSE_XESS_SAME(FuseXessResult, xess_result_t);
FUSE_XESS_SAME(FuseXessQuality, xess_quality_settings_t);
FUSE_XESS_SAME(FuseXess2d, xess_2d_t);
FUSE_XESS_SAME(FuseXessVersion, xess_version_t);
FUSE_XESS_SAME(FuseXessVkSubresourceRange, VkImageSubresourceRange);
FUSE_XESS_SAME(FuseXessVkImageViewInfo, xess_vk_image_view_info);
FUSE_XESS_AT(FuseXessVkImageViewInfo, image, xess_vk_image_view_info, image);
FUSE_XESS_AT(FuseXessVkImageViewInfo, subresource_range, xess_vk_image_view_info, subresourceRange);
FUSE_XESS_AT(FuseXessVkImageViewInfo, format, xess_vk_image_view_info, format);
FUSE_XESS_AT(FuseXessVkImageViewInfo, width, xess_vk_image_view_info, width);
FUSE_XESS_SAME(FuseXessVkInitParams, xess_vk_init_params_t);
FUSE_XESS_AT(FuseXessVkInitParams, quality_setting, xess_vk_init_params_t, qualitySetting);
FUSE_XESS_AT(FuseXessVkInitParams, init_flags, xess_vk_init_params_t, initFlags);
FUSE_XESS_AT(FuseXessVkInitParams, temp_buffer_heap, xess_vk_init_params_t, tempBufferHeap);
FUSE_XESS_AT(FuseXessVkInitParams, pipeline_cache, xess_vk_init_params_t, pipelineCache);
FUSE_XESS_SAME(FuseXessVkExecuteParams, xess_vk_execute_params_t);
FUSE_XESS_AT(FuseXessVkExecuteParams, velocity_texture, xess_vk_execute_params_t, velocityTexture);
FUSE_XESS_AT(FuseXessVkExecuteParams, output_texture, xess_vk_execute_params_t, outputTexture);
FUSE_XESS_AT(FuseXessVkExecuteParams, jitter_offset_x, xess_vk_execute_params_t, jitterOffsetX);
FUSE_XESS_AT(FuseXessVkExecuteParams, reset_history, xess_vk_execute_params_t, resetHistory);
FUSE_XESS_AT(FuseXessVkExecuteParams, input_width, xess_vk_execute_params_t, inputWidth);
FUSE_XESS_AT(FuseXessVkExecuteParams, output_color_base, xess_vk_execute_params_t, outputColorBase);

static_assert(FUSE_XESS_QUALITY_ULTRA_PERFORMANCE == XESS_QUALITY_SETTING_ULTRA_PERFORMANCE);
static_assert(FUSE_XESS_QUALITY_AA == XESS_QUALITY_SETTING_AA);
static_assert(FUSE_XESS_INIT_FLAG_INVERTED_DEPTH == XESS_INIT_FLAG_INVERTED_DEPTH);
static_assert(FUSE_XESS_INIT_FLAG_LDR_INPUT_COLOR == XESS_INIT_FLAG_LDR_INPUT_COLOR);
static_assert(FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DEVICE == XESS_RESULT_ERROR_UNSUPPORTED_DEVICE);

int fuse_xess_layout_check_anchor = 0;
