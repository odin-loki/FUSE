// FUSE NVIDIA provider over NVIDIA Streamline (MIT, part of FUSE; docs/nvidia-plugin.md).
//
// Implements fuse_nv_plugin_abi.h by loading the Streamline interposer at runtime from the provider's
// own directory (FuseNvInitInfo::runtime_dir) and calling it through function pointers typed by the
// vendored MIT headers (Engine/lib/streamline, v2.14.1). Nothing NVIDIA is linked at build time; the
// interposer and the sl.* / nvngx_* plugins it loads are the developer's own RTX-SDK-licensed copies.
//
// Options ("k=v;k=v"):
//   interposer=<file>   interposer file name (default sl.interposer.dll; libsl.interposer.so elsewhere,
//                       which only the CI mock interposer provides: Streamline documents Windows only)
//   ota=1               allow Streamline over-the-air plugin updates (off: reproducible builds)
//
// Scope (verified only against the mock interposer in CI; see docs/nvidia-plugin.md for what needs RTX
// hardware): SR and RR evaluate through slEvaluateFeature; FG sets DLSS-G options (frames are generated
// by Streamline's present hooks, which need the interposer's swap-chain proxy); NR (DLSS 5, "3D-guided
// neural rendering") is evaluated with local "uplift" tags only - Streamline 2.14.1's public headers
// define kFeatureDLSS_NR and the uplift buffer types but no options struct, so structure/tone intensity
// are not forwarded.

#include "nv_streamline_mapping.hpp"

#include <fuse/renderer/nvidia/fuse_nv_plugin_abi.h>

#include <sl_core_api.h>

#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#include <sl_helpers_vk.h>
#define FUSE_NVSL_HAS_VULKAN 1
#else
#define FUSE_NVSL_HAS_VULKAN 0
#endif

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace nvsl = fuse::renderer::nvidia::streamline;

struct FuseNvContext {
    void* lib = nullptr;
    PFun_slInit* slInit = nullptr;
    PFun_slShutdown* slShutdown = nullptr;
    PFun_slIsFeatureSupported* slIsFeatureSupported = nullptr;
    PFun_slGetNewFrameToken* slGetNewFrameToken = nullptr;
    PFun_slSetTagForFrame* slSetTagForFrame = nullptr;
    PFun_slSetConstants* slSetConstants = nullptr;
    PFun_slEvaluateFeature* slEvaluateFeature = nullptr;
    PFun_slGetFeatureFunction* slGetFeatureFunction = nullptr;
    PFun_slSetD3DDevice* slSetD3DDevice = nullptr;
    void* slSetVulkanInfo = nullptr;
    bool initialized = false;

    FuseNvGraphicsApi api = FUSE_NV_API_NONE;
    uint64_t physical_device = 0;
    FuseNvAdapterInfo adapter{};
    std::array<FuseNvStatus, FUSE_NV_FEATURE_COUNT> features{};
    std::wstring plugin_dir;
    FuseNvLogFn log = nullptr;
    void* log_user = nullptr;
    // Tags kept for local (evaluate-time) tagging: NR needs feature-specific buffer types.
    std::map<std::pair<uint32_t, uint32_t>, std::vector<FuseNvResourceTag>> tags;
    std::map<uint32_t, FuseNvConstants> constants; // last constants per viewport
};

namespace {

void* open_lib(const std::filesystem::path& p) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryExW(p.wstring().c_str(), nullptr,
                                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
#else
    return dlopen(p.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void close_lib(void* h) {
    if (!h) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(h));
#else
    dlclose(h);
#endif
}

template <typename Fn>
Fn* sym(void* h, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<Fn*>(reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), name)));
#else
    return reinterpret_cast<Fn*>(dlsym(h, name));
#endif
}

std::string option(const std::string& opts, const std::string& key) {
    size_t pos = 0;
    while (pos <= opts.size()) {
        const size_t semi = opts.find(';', pos);
        const size_t end = semi == std::string::npos ? opts.size() : semi;
        const std::string kv = opts.substr(pos, end - pos);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) {
            return kv.substr(eq + 1);
        }
        pos = end + 1;
    }
    return {};
}

void log(const FuseNvContext* ctx, uint32_t level, const std::string& msg) {
    if (ctx && ctx->log) {
        ctx->log(ctx->log_user, level, msg.c_str());
    }
}

const char* status_string(FuseNvStatus s) {
    switch (s) {
        case FUSE_NV_OK: return "ok";
        case FUSE_NV_ERR_ABI_MISMATCH: return "abi mismatch";
        case FUSE_NV_ERR_NOT_INITIALIZED: return "Streamline not initialized";
        case FUSE_NV_ERR_UNSUPPORTED_FEATURE: return "feature not supported by Streamline on this adapter";
        case FUSE_NV_ERR_NO_NVIDIA_GPU: return "no supported NVIDIA adapter";
        case FUSE_NV_ERR_DRIVER_TOO_OLD: return "NVIDIA driver out of date";
        case FUSE_NV_ERR_RUNTIME_MISSING: return "Streamline interposer or plugins missing";
        case FUSE_NV_ERR_INVALID_ARGUMENT: return "invalid argument";
        case FUSE_NV_ERR_MISSING_INPUT: return "missing input";
        case FUSE_NV_ERR_MISSING_CONSTANTS: return "missing constants";
        case FUSE_NV_ERR_INVALID_CONSTANTS: return "invalid constants";
        case FUSE_NV_ERR_GRAPHICS_API: return "graphics API not supported";
        default: return "Streamline error";
    }
}

FuseNvStatus frame_token(FuseNvContext* ctx, uint32_t frame, sl::FrameToken*& token) {
    token = nullptr;
    const FuseNvStatus s = nvsl::from_sl_result(ctx->slGetNewFrameToken(token, &frame));
    return (s == FUSE_NV_OK && !token) ? FUSE_NV_ERR_RUNTIME_FAILURE : s;
}

// Converts FUSE tags into sl resources + tags. `resources` must outlive `out` (tags point into it).
void build_tags(const std::vector<FuseNvResourceTag>& in, FuseNvFeature feature, std::vector<sl::Resource>& resources,
                std::vector<sl::ResourceTag>& out) {
    resources.clear();
    out.clear();
    resources.reserve(in.size());
    out.reserve(in.size());
    for (const FuseNvResourceTag& t : in) {
        sl::BufferType type{};
        if (!nvsl::to_sl_buffer_type(t.kind, feature, type)) {
            continue;
        }
        resources.push_back(nvsl::to_sl_resource(t.resource));
    }
    size_t r = 0;
    for (const FuseNvResourceTag& t : in) {
        sl::BufferType type{};
        if (!nvsl::to_sl_buffer_type(t.kind, feature, type)) {
            continue;
        }
        const sl::Extent extent = nvsl::to_sl_extent(t);
        out.emplace_back(&resources[r++], type, nvsl::to_sl_lifecycle(t.lifecycle), extent ? &extent : nullptr);
    }
}

FuseNvStatus sl_init(const FuseNvInitInfo* info, FuseNvContext** out_ctx) {
    if (!info || !out_ctx || info->struct_size < sizeof(FuseNvInitInfo)) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    *out_ctx = nullptr;
    if (FUSE_NV_PLUGIN_ABI_MAJOR_OF(info->host_abi_version) != FUSE_NV_PLUGIN_ABI_MAJOR) {
        return FUSE_NV_ERR_ABI_MISMATCH;
    }
    if (info->graphics_api > FUSE_NV_API_D3D12 || (info->graphics_api == FUSE_NV_API_VULKAN && !FUSE_NVSL_HAS_VULKAN)) {
        return FUSE_NV_ERR_GRAPHICS_API;
    }
    const std::string opts = info->options ? info->options : "";
    std::string name = option(opts, "interposer");
    if (name.empty()) {
#if defined(_WIN32)
        name = "sl.interposer.dll";
#else
        name = "libsl.interposer.so";
#endif
    }
    const std::filesystem::path dir = info->runtime_dir ? std::filesystem::path(info->runtime_dir) : std::filesystem::path();
    const std::filesystem::path path = dir / name;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        if (info->log) {
            info->log(info->log_user, 2, ("Streamline interposer not found: " + path.string()).c_str());
        }
        return FUSE_NV_ERR_RUNTIME_MISSING;
    }
    // NOTE: production Windows builds should verify the interposer's NVIDIA signature before loading
    // it (Streamline's sl_security.h); tracked in docs/nvidia-plugin.md "Hardware checklist".
    auto ctx = std::make_unique<FuseNvContext>();
    ctx->log = info->log;
    ctx->log_user = info->log_user;
    ctx->lib = open_lib(path);
    if (!ctx->lib) {
        log(ctx.get(), 2, "failed to load " + path.string());
        return FUSE_NV_ERR_RUNTIME_MISSING;
    }
    ctx->slInit = sym<PFun_slInit>(ctx->lib, "slInit");
    ctx->slShutdown = sym<PFun_slShutdown>(ctx->lib, "slShutdown");
    ctx->slIsFeatureSupported = sym<PFun_slIsFeatureSupported>(ctx->lib, "slIsFeatureSupported");
    ctx->slGetNewFrameToken = sym<PFun_slGetNewFrameToken>(ctx->lib, "slGetNewFrameToken");
    ctx->slSetTagForFrame = sym<PFun_slSetTagForFrame>(ctx->lib, "slSetTagForFrame");
    ctx->slSetConstants = sym<PFun_slSetConstants>(ctx->lib, "slSetConstants");
    ctx->slEvaluateFeature = sym<PFun_slEvaluateFeature>(ctx->lib, "slEvaluateFeature");
    ctx->slGetFeatureFunction = sym<PFun_slGetFeatureFunction>(ctx->lib, "slGetFeatureFunction");
    ctx->slSetD3DDevice = sym<PFun_slSetD3DDevice>(ctx->lib, "slSetD3DDevice");
    ctx->slSetVulkanInfo = sym<void>(ctx->lib, "slSetVulkanInfo");
    if (!ctx->slInit || !ctx->slShutdown || !ctx->slIsFeatureSupported || !ctx->slGetNewFrameToken ||
        !ctx->slSetTagForFrame || !ctx->slSetConstants || !ctx->slEvaluateFeature || !ctx->slGetFeatureFunction) {
        log(ctx.get(), 2, path.string() + " is missing Streamline core exports");
        close_lib(ctx->lib);
        return FUSE_NV_ERR_RUNTIME_MISSING;
    }

    ctx->api = info->graphics_api;
    ctx->physical_device = info->physical_device;
    ctx->plugin_dir = dir.wstring();
    const wchar_t* plugin_paths[] = {ctx->plugin_dir.c_str()};
    static const sl::Feature kLoad[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_RR, sl::kFeatureDLSS_G, sl::kFeatureDLSS_NR,
                                        sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences prefs{};
    prefs.showConsole = false;
    prefs.logLevel = sl::LogLevel::eDefault;
    prefs.pathsToPlugins = plugin_paths;
    prefs.numPathsToPlugins = 1;
    prefs.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                  sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    if (option(opts, "ota") == "1") {
        prefs.flags |= sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins;
    }
    prefs.featuresToLoad = kLoad;
    prefs.numFeaturesToLoad = uint32_t(sizeof(kLoad) / sizeof(kLoad[0]));
    prefs.applicationId = info->application_id;
    prefs.engine = sl::EngineType::eCustom;
    prefs.engineVersion = "FUSE";
    prefs.projectId = nullptr;
    prefs.renderAPI = info->graphics_api == FUSE_NV_API_VULKAN ? sl::RenderAPI::eVulkan : sl::RenderAPI::eD3D12;
    FuseNvStatus s = nvsl::from_sl_result(ctx->slInit(prefs, sl::kSDKVersion));
    if (s != FUSE_NV_OK) {
        log(ctx.get(), 2, "slInit failed");
        close_lib(ctx->lib);
        return s;
    }
    ctx->initialized = true;

    if (info->graphics_api == FUSE_NV_API_D3D12 && info->device) {
        if (!ctx->slSetD3DDevice) {
            s = FUSE_NV_ERR_RUNTIME_MISSING;
        } else {
            s = nvsl::from_sl_result(ctx->slSetD3DDevice(reinterpret_cast<void*>(static_cast<uintptr_t>(info->device))));
        }
    }
#if FUSE_NVSL_HAS_VULKAN
    if (info->graphics_api == FUSE_NV_API_VULKAN && info->device) {
        if (!ctx->slSetVulkanInfo) {
            s = FUSE_NV_ERR_RUNTIME_MISSING;
        } else {
            sl::VulkanInfo vk{};
            vk.instance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(info->instance));
            vk.physicalDevice = reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(info->physical_device));
            vk.device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(info->device));
            s = nvsl::from_sl_result(reinterpret_cast<PFun_slSetVulkanInfo*>(ctx->slSetVulkanInfo)(vk));
        }
    }
#endif
    if (s != FUSE_NV_OK) {
        log(ctx.get(), 2, "Streamline device registration failed");
        ctx->slShutdown();
        close_lib(ctx->lib);
        return s;
    }

    // Feature detection.
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = reinterpret_cast<void*>(static_cast<uintptr_t>(info->physical_device));
    for (uint32_t f = 0; f < FUSE_NV_FEATURE_COUNT; ++f) {
        sl::Feature slf{};
        ctx->features[f] = nvsl::to_sl_feature(f, slf) ? nvsl::from_sl_result(ctx->slIsFeatureSupported(slf, adapter))
                                                       : FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    }
    // DLSS-G requires sl.reflex.
    if (ctx->features[FUSE_NV_FEATURE_REFLEX] != FUSE_NV_OK) {
        ctx->features[FUSE_NV_FEATURE_DLSS_FG] = FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    }
    ctx->adapter.struct_size = sizeof(FuseNvAdapterInfo);
    const bool any = ctx->features[FUSE_NV_FEATURE_DLSS_SR] == FUSE_NV_OK;
    ctx->adapter.vendor_id = any ? 0x10DEu : 0u;
    // Streamline does not report the GPU generation; infer the floor from the feature set.
    ctx->adapter.rtx_generation = ctx->features[FUSE_NV_FEATURE_DLSS_NR] == FUSE_NV_OK   ? 50u
                                  : ctx->features[FUSE_NV_FEATURE_DLSS_FG] == FUSE_NV_OK ? 40u
                                  : any                                                  ? 20u
                                                                                         : 0u;
    std::snprintf(ctx->adapter.name, sizeof(ctx->adapter.name), "%s", "NVIDIA (via Streamline)");
    *out_ctx = ctx.release();
    return FUSE_NV_OK;
}

void sl_shutdown(FuseNvContext* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->initialized && ctx->slShutdown) {
        ctx->slShutdown();
    }
    close_lib(ctx->lib);
    delete ctx;
}

FuseNvStatus sl_adapter(FuseNvContext* ctx, FuseNvAdapterInfo* out) {
    if (!ctx || !out) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    const uint32_t size = out->struct_size;
    *out = ctx->adapter;
    out->struct_size = size;
    return FUSE_NV_OK;
}

FuseNvStatus sl_query(FuseNvContext* ctx, FuseNvFeature f, FuseNvFeatureSupport* out) {
    if (!ctx || !out) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    static const uint32_t kMinGen[FUSE_NV_FEATURE_COUNT] = {20, 20, 40, 50, 20};
    const uint32_t size = out->struct_size;
    *out = FuseNvFeatureSupport{};
    out->struct_size = size;
    out->feature = f;
    out->status = f < FUSE_NV_FEATURE_COUNT ? ctx->features[f] : FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    out->min_rtx_generation = f < FUSE_NV_FEATURE_COUNT ? kMinGen[f] : 0u;
    out->version_major = SL_VERSION_MAJOR;
    out->version_minor = SL_VERSION_MINOR;
    out->version_patch = SL_VERSION_PATCH;
    return FUSE_NV_OK;
}

FuseNvStatus sl_render_size(FuseNvContext* ctx, FuseNvFeature f, FuseNvQuality q, uint32_t dw, uint32_t dh,
                            FuseNvRenderSize* out) {
    if (!ctx || !out || !dw || !dh || q >= FUSE_NV_QUALITY_COUNT) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    if ((f != FUSE_NV_FEATURE_DLSS_SR && f != FUSE_NV_FEATURE_DLSS_RR) || ctx->features[f] != FUSE_NV_OK) {
        return FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    }
    const uint32_t size = out->struct_size;
    *out = FuseNvRenderSize{};
    out->struct_size = size;
    void* fn = nullptr;
    if (f == FUSE_NV_FEATURE_DLSS_SR) {
        FuseNvStatus s = nvsl::from_sl_result(ctx->slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fn));
        if (s != FUSE_NV_OK || !fn) {
            return s != FUSE_NV_OK ? s : FUSE_NV_ERR_RUNTIME_FAILURE;
        }
        sl::DLSSOptions o{};
        o.mode = nvsl::to_sl_dlss_mode(q);
        o.outputWidth = dw;
        o.outputHeight = dh;
        sl::DLSSOptimalSettings st{};
        s = nvsl::from_sl_result(reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(fn)(o, st));
        if (s != FUSE_NV_OK) {
            return s;
        }
        out->render_width = st.optimalRenderWidth;
        out->render_height = st.optimalRenderHeight;
        out->min_width = st.renderWidthMin;
        out->min_height = st.renderHeightMin;
        out->max_width = st.renderWidthMax;
        out->max_height = st.renderHeightMax;
        return FUSE_NV_OK;
    }
    FuseNvStatus s = nvsl::from_sl_result(ctx->slGetFeatureFunction(sl::kFeatureDLSS_RR, "slDLSSDGetOptimalSettings", fn));
    if (s != FUSE_NV_OK || !fn) {
        return s != FUSE_NV_OK ? s : FUSE_NV_ERR_RUNTIME_FAILURE;
    }
    sl::DLSSDOptions o{};
    o.mode = nvsl::to_sl_dlss_mode(q);
    o.outputWidth = dw;
    o.outputHeight = dh;
    sl::DLSSDOptimalSettings st{};
    s = nvsl::from_sl_result(reinterpret_cast<PFun_slDLSSDGetOptimalSettings*>(fn)(o, st));
    if (s != FUSE_NV_OK) {
        return s;
    }
    out->render_width = st.optimalRenderWidth;
    out->render_height = st.optimalRenderHeight;
    out->min_width = st.renderWidthMin;
    out->min_height = st.renderHeightMin;
    out->max_width = st.renderWidthMax;
    out->max_height = st.renderHeightMax;
    return FUSE_NV_OK;
}

FuseNvStatus sl_set_tags(FuseNvContext* ctx, uint32_t frame, uint32_t viewport, const FuseNvResourceTag* tags,
                         uint32_t count, uint64_t cmd) {
    if (!ctx || (count && !tags)) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    std::vector<FuseNvResourceTag> in(tags, tags + count);
    for (const FuseNvResourceTag& t : in) {
        if (t.kind >= FUSE_NV_BUFFER_KIND_COUNT || t.resource.native == 0u) {
            return FUSE_NV_ERR_INVALID_ARGUMENT;
        }
    }
    // Global tags use the upscaler / frame-generation buffer types (DLSS-G has no evaluate call, it
    // reads these at present); NR re-tags locally at evaluate.
    std::vector<sl::Resource> res;
    std::vector<sl::ResourceTag> sltags;
    build_tags(in, FUSE_NV_FEATURE_DLSS_SR, res, sltags);
    sl::FrameToken* token = nullptr;
    FuseNvStatus s = frame_token(ctx, frame, token);
    if (s != FUSE_NV_OK) {
        return s;
    }
    s = nvsl::from_sl_result(ctx->slSetTagForFrame(*token, sl::ViewportHandle(viewport), sltags.data(),
                                                   uint32_t(sltags.size()),
                                                   reinterpret_cast<sl::CommandBuffer*>(static_cast<uintptr_t>(cmd))));
    if (s == FUSE_NV_OK) {
        // Keep a short window (frame-token ring), then remember this frame's tags.
        for (auto it = ctx->tags.begin(); it != ctx->tags.end();) {
            it = (frame >= it->first.first && frame - it->first.first >= 8u) ? ctx->tags.erase(it) : std::next(it);
        }
        ctx->tags[{frame, viewport}] = std::move(in);
    }
    return s;
}

FuseNvStatus sl_set_constants(FuseNvContext* ctx, uint32_t viewport, const FuseNvConstants* c) {
    if (!ctx || !c || c->struct_size < sizeof(FuseNvConstants)) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    sl::FrameToken* token = nullptr;
    FuseNvStatus s = frame_token(ctx, c->frame_index, token);
    if (s != FUSE_NV_OK) {
        return s;
    }
    sl::Constants sc{};
    nvsl::to_sl_constants(*c, sc);
    s = nvsl::from_sl_result(ctx->slSetConstants(sc, *token, sl::ViewportHandle(viewport)));
    if (s == FUSE_NV_OK) {
        ctx->constants[viewport] = *c;
    }
    return s;
}

template <typename Options>
void fill_upscale_options(Options& o, const FuseNvFeatureOptions& opt, const FuseNvConstants& c, bool have_exposure) {
    o.mode = nvsl::to_sl_dlss_mode(opt.quality);
    o.outputWidth = c.output_width;
    o.outputHeight = c.output_height;
    o.preExposure = c.pre_exposure;
    o.exposureScale = c.exposure_scale;
    o.colorBuffersHDR = (c.flags & FUSE_NV_CONST_HDR) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    (void)have_exposure;
}

FuseNvStatus sl_evaluate(FuseNvContext* ctx, uint32_t frame, uint32_t viewport, const FuseNvFeatureOptions* opt, uint64_t cmd) {
    if (!ctx || !opt || opt->struct_size < sizeof(FuseNvFeatureOptions)) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    const FuseNvFeature f = opt->feature;
    if (f >= FUSE_NV_FEATURE_COUNT || f == FUSE_NV_FEATURE_REFLEX || ctx->features[f] != FUSE_NV_OK) {
        return FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    }
    const auto cit = ctx->constants.find(viewport);
    if (cit == ctx->constants.end() || cit->second.frame_index != frame) {
        return FUSE_NV_ERR_MISSING_CONSTANTS;
    }
    const FuseNvConstants& c = cit->second;
    const auto tit = ctx->tags.find({frame, viewport});
    const std::vector<FuseNvResourceTag> no_tags;
    const std::vector<FuseNvResourceTag>& tags = tit == ctx->tags.end() ? no_tags : tit->second;
    bool have_exposure = false;
    for (const FuseNvResourceTag& t : tags) {
        have_exposure |= t.kind == FUSE_NV_BUFFER_EXPOSURE;
    }
    const sl::ViewportHandle vp(viewport);
    sl::CommandBuffer* cb = reinterpret_cast<sl::CommandBuffer*>(static_cast<uintptr_t>(cmd));
    void* fn = nullptr;
    FuseNvStatus s = FUSE_NV_OK;

    if (f == FUSE_NV_FEATURE_DLSS_FG) {
        s = nvsl::from_sl_result(ctx->slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fn));
        if (s != FUSE_NV_OK || !fn) {
            return s != FUSE_NV_OK ? s : FUSE_NV_ERR_RUNTIME_FAILURE;
        }
        sl::DLSSGOptions o{};
        o.mode = sl::DLSSGMode::eOn;
        o.numFramesToGenerate = opt->frames_to_generate ? opt->frames_to_generate : 1u;
        o.mvecDepthWidth = c.render_width;
        o.mvecDepthHeight = c.render_height;
        o.colorWidth = c.output_width;
        o.colorHeight = c.output_height;
        return nvsl::from_sl_result(reinterpret_cast<PFun_slDLSSGSetOptions*>(fn)(vp, o));
    }

    sl::Feature slf{};
    nvsl::to_sl_feature(f, slf);
    std::vector<sl::Resource> res;
    std::vector<sl::ResourceTag> local;
    std::vector<const sl::BaseStructure*> inputs{&vp};
    if (f == FUSE_NV_FEATURE_DLSS_SR) {
        s = nvsl::from_sl_result(ctx->slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fn));
        if (s == FUSE_NV_OK && fn) {
            sl::DLSSOptions o{};
            fill_upscale_options(o, *opt, c, have_exposure);
            o.useAutoExposure = have_exposure ? sl::Boolean::eFalse : sl::Boolean::eTrue;
            s = nvsl::from_sl_result(reinterpret_cast<PFun_slDLSSSetOptions*>(fn)(vp, o));
        }
    } else if (f == FUSE_NV_FEATURE_DLSS_RR) {
        s = nvsl::from_sl_result(ctx->slGetFeatureFunction(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", fn));
        if (s == FUSE_NV_OK && fn) {
            sl::DLSSDOptions o{};
            fill_upscale_options(o, *opt, c, have_exposure);
            for (uint32_t r = 0; r < 4; ++r) {
                o.worldToCameraView.row[r] = sl::float4(c.world_to_camera_view[r * 4 + 0], c.world_to_camera_view[r * 4 + 1],
                                                        c.world_to_camera_view[r * 4 + 2], c.world_to_camera_view[r * 4 + 3]);
                o.cameraViewToWorld.row[r] = sl::float4(c.camera_view_to_world[r * 4 + 0], c.camera_view_to_world[r * 4 + 1],
                                                        c.camera_view_to_world[r * 4 + 2], c.camera_view_to_world[r * 4 + 3]);
            }
            s = nvsl::from_sl_result(reinterpret_cast<PFun_slDLSSDSetOptions*>(fn)(vp, o));
        }
    } else if (f == FUSE_NV_FEATURE_DLSS_NR) {
        // Local tags override the global ones for this evaluate: colour in/out become the uplift buffers.
        build_tags(tags, FUSE_NV_FEATURE_DLSS_NR, res, local);
        for (const sl::ResourceTag& t : local) {
            inputs.push_back(&t);
        }
    }
    if (s != FUSE_NV_OK) {
        return s;
    }
    if (f != FUSE_NV_FEATURE_DLSS_NR && !fn) {
        return FUSE_NV_ERR_RUNTIME_FAILURE;
    }
    sl::FrameToken* token = nullptr;
    s = frame_token(ctx, frame, token);
    if (s != FUSE_NV_OK) {
        return s;
    }
    return nvsl::from_sl_result(ctx->slEvaluateFeature(slf, *token, inputs.data(), uint32_t(inputs.size()), cb));
}

} // namespace

extern "C" FUSE_NV_PLUGIN_EXPORT FuseNvStatus fuseNvPluginGetApi(uint32_t host_abi_version, FuseNvApi* out) {
    if (!out) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    FuseNvApi api{};
    api.struct_size = sizeof(FuseNvApi);
    api.abi_version = FUSE_NV_PLUGIN_ABI_VERSION;
    api.provider_name = "streamline";
    api.runtime_version = "2.14.1";
    api.init = sl_init;
    api.shutdown = sl_shutdown;
    api.get_adapter_info = sl_adapter;
    api.query_feature = sl_query;
    api.get_render_size = sl_render_size;
    api.set_tags = sl_set_tags;
    api.set_constants = sl_set_constants;
    api.evaluate = sl_evaluate;
    api.status_string = status_string;
    const uint32_t host_size = out->struct_size ? out->struct_size : uint32_t(sizeof(FuseNvApi));
    std::memcpy(out, &api, host_size < sizeof(FuseNvApi) ? host_size : sizeof(FuseNvApi));
    return FUSE_NV_PLUGIN_ABI_MAJOR_OF(host_abi_version) == FUSE_NV_PLUGIN_ABI_MAJOR ? FUSE_NV_OK : FUSE_NV_ERR_ABI_MISMATCH;
}
