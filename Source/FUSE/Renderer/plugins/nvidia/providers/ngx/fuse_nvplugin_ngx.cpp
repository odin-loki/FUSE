// FUSE NVIDIA provider over the NGX / DLSS SDK on Linux (MIT source, part of FUSE; docs/nvidia-plugin.md).
//
// This file is FUSE's own code. It is compiled ONLY on a developer machine, with
// -DFUSE_ENABLE_NVIDIA_PLUGIN=ON -DFUSE_NVIDIA_DLSS_SDK_DIR=<their DLSS SDK checkout>: it includes the SDK's
// headers (NVIDIA RTX SDKs License, not vendored in FUSE) and statically links the SDK's NGX loader
// (libnvsdk_ngx.a). The resulting libfuse_nvplugin_ngx.so therefore contains NVIDIA object code, is covered
// by the RTX SDKs License, and must never be committed (.gitignore + ctest fuse_nvidia_no_committed_binaries).
// At runtime NGX loads libnvidia-ngx-dlss.so.<ver> from the provider's directory (FuseNvInitInfo::runtime_dir).
//
// Scope: DLSS Super Resolution / DLAA on Vulkan. Ray reconstruction, frame generation and DLSS 5 neural
// rendering report FUSE_NV_ERR_UNSUPPORTED_FEATURE on this path (use Streamline; see the doc's feature
// matrix). NOT verified in CI (no SDK, no GPU): see docs/nvidia-plugin.md "Hardware checklist".

#include <fuse/renderer/nvidia/fuse_nv_plugin_abi.h>

#include <vulkan/vulkan.h>

#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

struct FuseNvContext {
    VkDevice device = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* caps = nullptr;
    bool sr_available = false;
    bool driver_too_old = false;
    std::wstring runtime_dir;
    FuseNvLogFn log = nullptr;
    void* log_user = nullptr;
    struct Feature {
        NVSDK_NGX_Handle* handle = nullptr;
        std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, int, int> key{};
    };
    std::map<uint32_t, Feature> features; // per viewport
    std::map<std::pair<uint32_t, uint32_t>, std::vector<FuseNvResourceTag>> tags;
    std::map<uint32_t, FuseNvConstants> constants;
};

namespace {

FuseNvStatus from_ngx(NVSDK_NGX_Result r) {
    if (NVSDK_NGX_SUCCEED(r)) {
        return FUSE_NV_OK;
    }
    switch (r) {
        case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
        case NVSDK_NGX_Result_FAIL_FeatureNotFound: return FUSE_NV_ERR_UNSUPPORTED_FEATURE;
        case NVSDK_NGX_Result_FAIL_OutOfDate: return FUSE_NV_ERR_DRIVER_TOO_OLD;
        case NVSDK_NGX_Result_FAIL_NotInitialized: return FUSE_NV_ERR_NOT_INITIALIZED;
        case NVSDK_NGX_Result_FAIL_InvalidParameter:
        case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:
        case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return FUSE_NV_ERR_INVALID_ARGUMENT;
        case NVSDK_NGX_Result_FAIL_MissingInput: return FUSE_NV_ERR_MISSING_INPUT;
        case NVSDK_NGX_Result_FAIL_PlatformError: return FUSE_NV_ERR_NO_NVIDIA_GPU;
        default: return FUSE_NV_ERR_RUNTIME_FAILURE;
    }
}

const char* ngx_status_string(FuseNvStatus s) {
    switch (s) {
        case FUSE_NV_OK: return "ok";
        case FUSE_NV_ERR_UNSUPPORTED_FEATURE: return "not supported by the NGX bridge / this GPU";
        case FUSE_NV_ERR_DRIVER_TOO_OLD: return "NVIDIA driver or DLSS snippet out of date";
        case FUSE_NV_ERR_NO_NVIDIA_GPU: return "NGX platform error (no NVIDIA RTX GPU?)";
        case FUSE_NV_ERR_GRAPHICS_API: return "the NGX bridge needs a Vulkan device";
        case FUSE_NV_ERR_MISSING_INPUT: return "missing input";
        case FUSE_NV_ERR_MISSING_CONSTANTS: return "missing constants";
        default: return "NGX error";
    }
}

NVSDK_NGX_PerfQuality_Value to_perf(FuseNvQuality q) {
    switch (q) {
        case FUSE_NV_QUALITY_DLAA: return NVSDK_NGX_PerfQuality_Value_DLAA;
        case FUSE_NV_QUALITY_BALANCED: return NVSDK_NGX_PerfQuality_Value_Balanced;
        case FUSE_NV_QUALITY_PERFORMANCE: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case FUSE_NV_QUALITY_ULTRA_PERFORMANCE: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        case FUSE_NV_QUALITY_ULTRA_QUALITY: return NVSDK_NGX_PerfQuality_Value_UltraQuality;
        default: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
}

NVSDK_NGX_Resource_VK to_vk(const FuseNvResourceTag& t, bool depth, bool read_write) {
    VkImageSubresourceRange range{};
    range.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    const uint32_t w = t.extent_w ? t.extent_w : t.resource.width;
    const uint32_t h = t.extent_w ? t.extent_h : t.resource.height;
    return NVSDK_NGX_Create_ImageView_Resource_VK(reinterpret_cast<VkImageView>(static_cast<uintptr_t>(t.resource.view)),
                                                  reinterpret_cast<VkImage>(static_cast<uintptr_t>(t.resource.native)), range,
                                                  static_cast<VkFormat>(t.resource.native_format), w, h, read_write);
}

FuseNvStatus ngx_init(const FuseNvInitInfo* info, FuseNvContext** out_ctx) {
    if (!info || !out_ctx || info->struct_size < sizeof(FuseNvInitInfo)) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    *out_ctx = nullptr;
    if (FUSE_NV_PLUGIN_ABI_MAJOR_OF(info->host_abi_version) != FUSE_NV_PLUGIN_ABI_MAJOR) {
        return FUSE_NV_ERR_ABI_MISMATCH;
    }
    if (info->graphics_api != FUSE_NV_API_VULKAN || !info->instance || !info->physical_device || !info->device) {
        return FUSE_NV_ERR_GRAPHICS_API;
    }
    auto ctx = std::make_unique<FuseNvContext>();
    ctx->log = info->log;
    ctx->log_user = info->log_user;
    const std::string dir = info->runtime_dir ? info->runtime_dir : ".";
    ctx->runtime_dir.assign(dir.begin(), dir.end()); // paths are ASCII in practice; NGX takes wchar_t
    const wchar_t* paths[] = {ctx->runtime_dir.c_str()};
    NVSDK_NGX_FeatureCommonInfo common{};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;
    ctx->device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(info->device));
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init(info->application_id, ctx->runtime_dir.c_str(),
                                               reinterpret_cast<VkInstance>(static_cast<uintptr_t>(info->instance)),
                                               reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(info->physical_device)),
                                               ctx->device, nullptr, nullptr, &common);
    if (NVSDK_NGX_FAILED(r)) {
        const FuseNvStatus s = from_ngx(r);
        return s == FUSE_NV_ERR_UNSUPPORTED_FEATURE ? FUSE_NV_ERR_RUNTIME_MISSING : s;
    }
    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&ctx->caps);
    if (NVSDK_NGX_FAILED(r) || !ctx->caps) {
        NVSDK_NGX_VULKAN_Shutdown1(ctx->device);
        return from_ngx(r);
    }
    int available = 0, needs_driver = 0;
    NVSDK_NGX_Parameter_GetI(ctx->caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(ctx->caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    ctx->sr_available = available != 0;
    ctx->driver_too_old = needs_driver != 0;
    *out_ctx = ctx.release();
    return FUSE_NV_OK;
}

void ngx_shutdown(FuseNvContext* ctx) {
    if (!ctx) {
        return;
    }
    for (auto& [vp, f] : ctx->features) {
        (void)vp;
        if (f.handle) {
            NVSDK_NGX_VULKAN_ReleaseFeature(f.handle);
        }
    }
    if (ctx->caps) {
        NVSDK_NGX_VULKAN_DestroyParameters(ctx->caps);
    }
    NVSDK_NGX_VULKAN_Shutdown1(ctx->device);
    delete ctx;
}

FuseNvStatus ngx_adapter(FuseNvContext* ctx, FuseNvAdapterInfo* out) {
    if (!ctx || !out) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    const uint32_t size = out->struct_size;
    *out = FuseNvAdapterInfo{};
    out->struct_size = size;
    out->vendor_id = 0x10DE;
    out->rtx_generation = ctx->sr_available ? 20u : 0u; // NGX does not report the generation
    std::snprintf(out->name, sizeof(out->name), "%s", "NVIDIA (via NGX)");
    return FUSE_NV_OK;
}

FuseNvStatus ngx_query(FuseNvContext* ctx, FuseNvFeature f, FuseNvFeatureSupport* out) {
    if (!ctx || !out) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    const uint32_t size = out->struct_size;
    *out = FuseNvFeatureSupport{};
    out->struct_size = size;
    out->feature = f;
    if (f == FUSE_NV_FEATURE_DLSS_SR) {
        out->status = ctx->driver_too_old ? FUSE_NV_ERR_DRIVER_TOO_OLD : ctx->sr_available ? FUSE_NV_OK : FUSE_NV_ERR_UNSUPPORTED_FEATURE;
        out->min_rtx_generation = 20;
    } else {
        out->status = FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    }
    return FUSE_NV_OK;
}

FuseNvStatus ngx_render_size(FuseNvContext* ctx, FuseNvFeature f, FuseNvQuality q, uint32_t dw, uint32_t dh, FuseNvRenderSize* out) {
    if (!ctx || !out || !dw || !dh) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    if (f != FUSE_NV_FEATURE_DLSS_SR || !ctx->sr_available) {
        return FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    }
    const uint32_t size = out->struct_size;
    *out = FuseNvRenderSize{};
    out->struct_size = size;
    float sharpness = 0.f;
    return from_ngx(NGX_DLSS_GET_OPTIMAL_SETTINGS(ctx->caps, dw, dh, to_perf(q), &out->render_width, &out->render_height,
                                                  &out->max_width, &out->max_height, &out->min_width, &out->min_height,
                                                  &sharpness));
}

FuseNvStatus ngx_set_tags(FuseNvContext* ctx, uint32_t frame, uint32_t viewport, const FuseNvResourceTag* tags, uint32_t count,
                          uint64_t) {
    if (!ctx || (count && !tags)) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    for (auto it = ctx->tags.begin(); it != ctx->tags.end();) {
        it = (frame >= it->first.first && frame - it->first.first >= 8u) ? ctx->tags.erase(it) : std::next(it);
    }
    ctx->tags[{frame, viewport}] = std::vector<FuseNvResourceTag>(tags, tags + count);
    return FUSE_NV_OK;
}

FuseNvStatus ngx_set_constants(FuseNvContext* ctx, uint32_t viewport, const FuseNvConstants* c) {
    if (!ctx || !c || c->struct_size < sizeof(FuseNvConstants)) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    ctx->constants[viewport] = *c;
    return FUSE_NV_OK;
}

FuseNvStatus ngx_evaluate(FuseNvContext* ctx, uint32_t frame, uint32_t viewport, const FuseNvFeatureOptions* o, uint64_t cmd) {
    if (!ctx || !o || o->struct_size < sizeof(FuseNvFeatureOptions) || !cmd) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    if (o->feature != FUSE_NV_FEATURE_DLSS_SR || !ctx->sr_available) {
        return FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    }
    const auto cit = ctx->constants.find(viewport);
    if (cit == ctx->constants.end() || cit->second.frame_index != frame) {
        return FUSE_NV_ERR_MISSING_CONSTANTS;
    }
    const FuseNvConstants& c = cit->second;
    const auto tit = ctx->tags.find({frame, viewport});
    if (tit == ctx->tags.end()) {
        return FUSE_NV_ERR_MISSING_INPUT;
    }
    const FuseNvResourceTag* by_kind[FUSE_NV_BUFFER_KIND_COUNT] = {};
    for (const FuseNvResourceTag& t : tit->second) {
        if (t.kind < FUSE_NV_BUFFER_KIND_COUNT) {
            by_kind[t.kind] = &t;
        }
    }
    if (!by_kind[FUSE_NV_BUFFER_COLOR_IN] || !by_kind[FUSE_NV_BUFFER_COLOR_OUT] || !by_kind[FUSE_NV_BUFFER_DEPTH] ||
        !by_kind[FUSE_NV_BUFFER_MOTION_VECTORS]) {
        return FUSE_NV_ERR_MISSING_INPUT;
    }
    const VkCommandBuffer cb = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(cmd));
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    flags |= (c.flags & FUSE_NV_CONST_HDR) ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0;
    flags |= (c.flags & FUSE_NV_CONST_DEPTH_INVERTED) ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0;
    flags |= (c.flags & FUSE_NV_CONST_MV_JITTERED) ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0;
    flags |= by_kind[FUSE_NV_BUFFER_EXPOSURE] ? 0 : NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    // (Re)create the DLSS feature when size / quality / flags change.
    FuseNvContext::Feature& feat = ctx->features[viewport];
    const auto key = std::make_tuple(c.render_width, c.render_height, c.output_width, c.output_height,
                                     static_cast<int>(to_perf(o->quality)), flags);
    if (!feat.handle || feat.key != key) {
        if (feat.handle) {
            NVSDK_NGX_VULKAN_ReleaseFeature(feat.handle);
            feat.handle = nullptr;
        }
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = c.render_width;
        create.Feature.InHeight = c.render_height;
        create.Feature.InTargetWidth = c.output_width;
        create.Feature.InTargetHeight = c.output_height;
        create.Feature.InPerfQualityValue = to_perf(o->quality);
        create.InFeatureCreateFlags = flags;
        const NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT(cb, 1, 1, &feat.handle, ctx->caps, &create);
        if (NVSDK_NGX_FAILED(r)) {
            feat.handle = nullptr;
            return from_ngx(r);
        }
        feat.key = key;
    }

    NVSDK_NGX_Resource_VK color = to_vk(*by_kind[FUSE_NV_BUFFER_COLOR_IN], false, false);
    NVSDK_NGX_Resource_VK output = to_vk(*by_kind[FUSE_NV_BUFFER_COLOR_OUT], false, true);
    NVSDK_NGX_Resource_VK depth = to_vk(*by_kind[FUSE_NV_BUFFER_DEPTH], true, false);
    NVSDK_NGX_Resource_VK motion = to_vk(*by_kind[FUSE_NV_BUFFER_MOTION_VECTORS], false, false);
    NVSDK_NGX_Resource_VK exposure{};
    NVSDK_NGX_Resource_VK reactive{};
    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &color;
    eval.Feature.pInOutput = &output;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion;
    if (by_kind[FUSE_NV_BUFFER_EXPOSURE]) {
        exposure = to_vk(*by_kind[FUSE_NV_BUFFER_EXPOSURE], false, false);
        eval.pInExposureTexture = &exposure;
    }
    if (by_kind[FUSE_NV_BUFFER_REACTIVE_MASK]) {
        reactive = to_vk(*by_kind[FUSE_NV_BUFFER_REACTIVE_MASK], false, false);
        eval.pInBiasCurrentColorMask = &reactive;
    }
    eval.InJitterOffsetX = c.jitter_offset_px[0];
    eval.InJitterOffsetY = c.jitter_offset_px[1];
    eval.InRenderSubrectDimensions.Width = c.render_width;
    eval.InRenderSubrectDimensions.Height = c.render_height;
    eval.InReset = (c.flags & FUSE_NV_CONST_RESET) ? 1 : 0;
    // The ABI's mvec_scale is Streamline's (to [-1,1] UV); NGX's InMVScale converts to render pixels.
    eval.InMVScaleX = c.mvec_scale[0] * static_cast<float>(c.render_width);
    eval.InMVScaleY = c.mvec_scale[1] * static_cast<float>(c.render_height);
    eval.InPreExposure = c.pre_exposure;
    eval.InExposureScale = c.exposure_scale;
    eval.InFrameTimeDeltaInMsec = c.frame_time_ms;
    return from_ngx(NGX_VULKAN_EVALUATE_DLSS_EXT(cb, feat.handle, ctx->caps, &eval));
}

} // namespace

extern "C" FUSE_NV_PLUGIN_EXPORT FuseNvStatus fuseNvPluginGetApi(uint32_t host_abi_version, FuseNvApi* out) {
    if (!out) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    FuseNvApi api{};
    api.struct_size = sizeof(FuseNvApi);
    api.abi_version = FUSE_NV_PLUGIN_ABI_VERSION;
    api.provider_name = "ngx";
    api.runtime_version = "dlss-sdk";
    api.init = ngx_init;
    api.shutdown = ngx_shutdown;
    api.get_adapter_info = ngx_adapter;
    api.query_feature = ngx_query;
    api.get_render_size = ngx_render_size;
    api.set_tags = ngx_set_tags;
    api.set_constants = ngx_set_constants;
    api.evaluate = ngx_evaluate;
    api.status_string = ngx_status_string;
    const uint32_t host_size = out->struct_size ? out->struct_size : uint32_t(sizeof(FuseNvApi));
    std::memcpy(out, &api, host_size < sizeof(FuseNvApi) ? host_size : sizeof(FuseNvApi));
    return FUSE_NV_PLUGIN_ABI_MAJOR_OF(host_abi_version) == FUSE_NV_PLUGIN_ABI_MAJOR ? FUSE_NV_OK : FUSE_NV_ERR_ABI_MISMATCH;
}
