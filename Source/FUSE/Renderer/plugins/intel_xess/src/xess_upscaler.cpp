// WP-4.3 XeSS adapter (MIT): runtime loader, parameter mapping, XessUpscaler, registration. xess_upscaler.hpp.
#include <fuse/renderer/xess/xess_upscaler.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>

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

namespace fuse::renderer::xess {

namespace {

namespace fs = std::filesystem;
using upscale::HistoryResetReason;
using upscale::QualityMode;
using upscale::UpscaleStatus;

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read at start-up only
    return v ? std::string(v) : std::string();
}

void* open_library(const std::string& path, std::string& error) {
#if defined(_WIN32)
    const std::wstring wide = fs::path(path).wstring();
    HMODULE h = LoadLibraryExW(wide.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!h) {
        error = "LoadLibraryExW failed, GetLastError=" + std::to_string(GetLastError());
    }
    return reinterpret_cast<void*>(h);
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        error = e ? e : "dlopen failed";
    }
    return h;
#endif
}

void close_library(void* h) {
    if (!h) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(h));
#else
    dlclose(h);
#endif
}

void* find_symbol(void* h, const char* name) {
    if (!h) {
        return nullptr;
    }
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
#else
    return dlsym(h, name);
#endif
}

template <typename Fn>
void resolve(void* lib, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(find_symbol(lib, name));
}

XessLoadResult fail(XessStatus status, std::string detail) {
    XessLoadResult r;
    r.status = status;
    r.detail = std::move(detail);
    return r;
}

XessStatus status_from_create(FuseXessResult r) {
    switch (r) {
        case FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return XessStatus::DeviceUnsupported;
        case FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return XessStatus::DriverUnsupported;
        default: return XessStatus::ContextFailed;
    }
}

bool succeeded(FuseXessResult r) { return r >= FUSE_XESS_RESULT_SUCCESS; } // warnings are positive

void fill_view(FuseXessVkImageViewInfo& v, u32 w, u32 h) {
    if (v.image == 0u) {
        v = FuseXessVkImageViewInfo{};
        return;
    }
    if (v.width == 0u || v.height == 0u) {
        v.width = w;
        v.height = h;
    }
    if (v.subresource_range.level_count == 0u) {
        v.subresource_range.level_count = 1u;
    }
    if (v.subresource_range.layer_count == 0u) {
        v.subresource_range.layer_count = 1u;
    }
    if (v.subresource_range.aspect_mask == 0u) {
        v.subresource_range.aspect_mask = 1u; // VK_IMAGE_ASPECT_COLOR_BIT (depth views must set DEPTH explicitly)
    }
}

std::mutex g_bindingMutex;
std::shared_ptr<XessRuntime> g_runtime;
XessDeviceBinding g_device{};

std::unique_ptr<upscale::IUpscaler> make_xess() {
    std::shared_ptr<XessRuntime> rt;
    XessDeviceBinding dev{};
    {
        std::lock_guard<std::mutex> lock(g_bindingMutex);
        rt = g_runtime;
        dev = g_device;
    }
    return std::make_unique<XessUpscaler>(std::move(rt), dev);
}

} // namespace

// ---- names -----------------------------------------------------------------------------------------------------

std::string_view xess_status_name(XessStatus s) noexcept {
    switch (s) {
        case XessStatus::Available: return "available";
        case XessStatus::DisabledAtBuild: return "disabled at build (FUSE_ENABLE_XESS_PLUGIN=OFF)";
        case XessStatus::NotConfigured: return "not configured (set FUSE_XESS_SDK_DIR)";
        case XessStatus::LibraryNotFound: return "libxess not found";
        case XessStatus::LoadFailed: return "libxess failed to load";
        case XessStatus::EntryPointMissing: return "libxess entry point missing";
        case XessStatus::VersionUnsupported: return "libxess version unsupported";
        case XessStatus::DeviceUnsupported: return "GPU not supported by XeSS";
        case XessStatus::DriverUnsupported: return "driver not supported by XeSS";
        case XessStatus::ContextFailed: return "XeSS context creation failed";
    }
    return "unknown";
}

const char* xess_result_name(FuseXessResult r) noexcept {
    switch (r) {
        case FUSE_XESS_RESULT_WARNING_NONEXISTING_FOLDER: return "warning: nonexisting folder";
        case FUSE_XESS_RESULT_WARNING_OLD_DRIVER: return "warning: old driver";
        case FUSE_XESS_RESULT_SUCCESS: return "success";
        case FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported device";
        case FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return "unsupported driver";
        case FUSE_XESS_RESULT_ERROR_UNINITIALIZED: return "uninitialized";
        case FUSE_XESS_RESULT_ERROR_INVALID_ARGUMENT: return "invalid argument";
        case FUSE_XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "device out of memory";
        case FUSE_XESS_RESULT_ERROR_DEVICE: return "device error";
        case FUSE_XESS_RESULT_ERROR_NOT_IMPLEMENTED: return "not implemented";
        case FUSE_XESS_RESULT_ERROR_INVALID_CONTEXT: return "invalid context";
        case FUSE_XESS_RESULT_ERROR_OPERATION_IN_PROGRESS: return "operation in progress";
        case FUSE_XESS_RESULT_ERROR_UNSUPPORTED: return "unsupported";
        case FUSE_XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return "can't load library";
        default: return "unknown error";
    }
}

// ---- loader ------------------------------------------------------------------------------------------------------

std::string_view default_xess_library_name() noexcept {
#if defined(_WIN32)
    return "libxess.dll";
#elif defined(__APPLE__)
    return "libxess.dylib"; // no runtime exists; kept for a uniform "not found"
#else
    return "libxess.so";
#endif
}

XessConfig resolve_xess_config(const XessConfig& project) {
    XessConfig c = project;
    if (c.sdk_dir.empty()) {
        c.sdk_dir = env_or_empty("FUSE_XESS_SDK_DIR");
    }
    if (c.library_name.empty()) {
        c.library_name = env_or_empty("FUSE_XESS_LIB");
    }
    return c;
}

XessRuntime::~XessRuntime() {
    close_library(m_lib);
    m_lib = nullptr;
}

void* XessRuntime::symbol(const char* name) const noexcept { return find_symbol(m_lib, name); }

XessStatus XessRuntime::probe_device(const XessDeviceBinding& device, std::string* detail) const {
    if (device.instance == nullptr || device.physical_device == nullptr || device.device == nullptr) {
        if (detail) {
            *detail = "no Vulkan device bound";
        }
        return XessStatus::ContextFailed;
    }
    FuseXessContext ctx = nullptr;
    const FuseXessResult r = m_fn.vk_create_context(device.instance, device.physical_device, device.device, &ctx);
    if (!succeeded(r) || ctx == nullptr) {
        if (detail) {
            *detail = std::string("xessVKCreateContext: ") + xess_result_name(r);
        }
        return status_from_create(succeeded(r) ? FUSE_XESS_RESULT_ERROR_UNKNOWN : r);
    }
    m_fn.destroy_context(ctx);
    if (detail) {
        *detail = r == FUSE_XESS_RESULT_SUCCESS ? "ok" : xess_result_name(r);
    }
    return XessStatus::Available;
}

bool XessLoader::enabled_at_build() noexcept {
#if defined(FUSE_XESS_PLUGIN_ENABLED) && FUSE_XESS_PLUGIN_ENABLED
    return true;
#else
    return false;
#endif
}

XessLoadResult XessLoader::load(const XessConfig& config) {
    if (config.sdk_dir.empty()) {
        return fail(XessStatus::NotConfigured, "no XeSS runtime directory configured (project setting or FUSE_XESS_SDK_DIR)");
    }
    const std::string name = config.library_name.empty() ? std::string(default_xess_library_name()) : config.library_name;
    return load_file((fs::path(config.sdk_dir) / name).string());
}

XessLoadResult XessLoader::load_file(const std::string& library_path) {
    std::error_code ec;
    if (library_path.empty() || !fs::is_regular_file(fs::path(library_path), ec)) {
        return fail(XessStatus::LibraryNotFound, "no XeSS runtime at '" + library_path + "'");
    }
    std::string err;
    void* lib = open_library(library_path, err);
    if (!lib) {
        return fail(XessStatus::LoadFailed, library_path + ": " + err);
    }
    std::unique_ptr<XessRuntime> rt(new XessRuntime());
    rt->m_lib = lib;
    rt->m_path = library_path;
    XessFunctions& f = rt->m_fn;
    resolve(lib, "xessGetVersion", f.get_version);
    resolve(lib, "xessDestroyContext", f.destroy_context);
    resolve(lib, "xessVKCreateContext", f.vk_create_context);
    resolve(lib, "xessVKInit", f.vk_init);
    resolve(lib, "xessVKExecute", f.vk_execute);
    resolve(lib, "xessSetVelocityScale", f.set_velocity_scale);
    resolve(lib, "xessVKBuildPipelines", f.vk_build_pipelines);
    resolve(lib, "xessGetInputResolution", f.get_input_resolution);
    resolve(lib, "xessSetJitterScale", f.set_jitter_scale);
    resolve(lib, "xessSetExposureMultiplier", f.set_exposure_multiplier);
    const char* missing = !f.get_version            ? "xessGetVersion"
                          : !f.destroy_context      ? "xessDestroyContext"
                          : !f.vk_create_context    ? "xessVKCreateContext"
                          : !f.vk_init              ? "xessVKInit"
                          : !f.vk_execute           ? "xessVKExecute"
                          : !f.set_velocity_scale   ? "xessSetVelocityScale"
                                                    : nullptr;
    if (missing) {
        return fail(XessStatus::EntryPointMissing, library_path + ": no export '" + missing + "'");
    }
    const FuseXessResult vr = f.get_version(&rt->m_version);
    const FuseXessVersion& v = rt->m_version;
    const std::string vs = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    if (!succeeded(vr) || v.major < XessRuntime::kMinMajor || v.major > XessRuntime::kMaxMajor) {
        return fail(XessStatus::VersionUnsupported, library_path + ": XeSS " + vs + " (xessGetVersion: " + xess_result_name(vr) +
                                                        "), supported majors " + std::to_string(XessRuntime::kMinMajor) + ".." +
                                                        std::to_string(XessRuntime::kMaxMajor));
    }
    XessLoadResult r;
    r.status = XessStatus::Available;
    r.detail = library_path + ": XeSS " + vs;
    r.runtime = std::move(rt);
    return r;
}

XessLoadResult XessLoader::probe(const XessConfig& project) {
    if (!enabled_at_build()) {
        return fail(XessStatus::DisabledAtBuild, "configure with -DFUSE_ENABLE_XESS_PLUGIN=ON to probe for libxess");
    }
    return load(resolve_xess_config(project));
}

// ---- mapping -------------------------------------------------------------------------------------------------------

FuseXessQuality to_xess_quality(QualityMode mode) noexcept {
    switch (mode) {
        case QualityMode::NativeAA: return FUSE_XESS_QUALITY_AA;
        case QualityMode::UltraQuality: return FUSE_XESS_QUALITY_ULTRA_QUALITY_PLUS;
        case QualityMode::Quality: return FUSE_XESS_QUALITY_ULTRA_QUALITY;
        case QualityMode::Balanced: return FUSE_XESS_QUALITY_QUALITY;
        case QualityMode::Performance: return FUSE_XESS_QUALITY_BALANCED;
        case QualityMode::UltraPerformance: return FUSE_XESS_QUALITY_ULTRA_PERFORMANCE;
    }
    return FUSE_XESS_QUALITY_ULTRA_QUALITY;
}

f32 xess_quality_ratio(FuseXessQuality q) noexcept {
    switch (q) {
        case FUSE_XESS_QUALITY_ULTRA_PERFORMANCE: return 3.0f;
        case FUSE_XESS_QUALITY_PERFORMANCE: return 2.3f;
        case FUSE_XESS_QUALITY_BALANCED: return 2.0f;
        case FUSE_XESS_QUALITY_QUALITY: return 1.7f;
        case FUSE_XESS_QUALITY_ULTRA_QUALITY: return 1.5f;
        case FUSE_XESS_QUALITY_ULTRA_QUALITY_PLUS: return 1.3f;
        case FUSE_XESS_QUALITY_AA: return 1.0f;
        default: return 0.f;
    }
}

UpscaleStatus to_upscale_status(FuseXessResult r) noexcept {
    if (succeeded(r)) {
        return UpscaleStatus::Ok;
    }
    switch (r) {
        case FUSE_XESS_RESULT_ERROR_INVALID_ARGUMENT: return UpscaleStatus::InvalidInputs;
        case FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DEVICE:
        case FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DRIVER:
        case FUSE_XESS_RESULT_ERROR_UNINITIALIZED:
        case FUSE_XESS_RESULT_ERROR_INVALID_CONTEXT:
        case FUSE_XESS_RESULT_ERROR_NOT_IMPLEMENTED:
        case FUSE_XESS_RESULT_ERROR_UNSUPPORTED:
        case FUSE_XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return UpscaleStatus::BackendUnavailable;
        default: return UpscaleStatus::LaunchFailed;
    }
}

XessMappedFrame map_upscale_inputs(const UpscaleInputs& in, const XessGpuFrame& gpu, bool reset) noexcept {
    XessMappedFrame m;
    FuseXessVkExecuteParams& p = m.execute;
    const UpscaleResolution& r = in.resolution;
    p.color_texture = gpu.color;
    p.velocity_texture = gpu.velocity;
    p.depth_texture = gpu.depth;
    p.exposure_scale_texture = gpu.exposure;
    p.responsive_pixel_mask_texture = gpu.responsive;
    p.output_texture = gpu.output;
    fill_view(p.color_texture, r.render_width, r.render_height);
    fill_view(p.velocity_texture, r.render_width, r.render_height);
    fill_view(p.depth_texture, r.render_width, r.render_height);
    fill_view(p.exposure_scale_texture, 1u, 1u);
    fill_view(p.responsive_pixel_mask_texture, r.render_width, r.render_height);
    fill_view(p.output_texture, r.display_width, r.display_height);
    const f32 sign = gpu.debug_flip_jitter_sign ? 1.f : -1.f;
    p.jitter_offset_x = sign * in.jitter_px.x;
    p.jitter_offset_y = sign * in.jitter_px.y;
    p.exposure_scale = in.exposure;
    p.reset_history = (reset || in.reset_history) ? 1u : 0u;
    p.input_width = r.render_width;
    p.input_height = r.render_height;
    m.init_flags = FUSE_XESS_INIT_FLAG_NONE;
    m.init_flags |= gpu.depth_reversed_z ? FUSE_XESS_INIT_FLAG_INVERTED_DEPTH : 0u;
    m.init_flags |= gpu.hdr ? 0u : FUSE_XESS_INIT_FLAG_LDR_INPUT_COLOR;
    m.init_flags |= gpu.responsive.image != 0u ? FUSE_XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK : 0u;
    m.init_flags |= gpu.exposure.image != 0u ? FUSE_XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE : 0u;
    // FUSE UV motion (cur - prev) -> render-pixel velocity towards the previous position.
    m.velocity_scale[0] = -static_cast<f32>(r.render_width);
    m.velocity_scale[1] = -static_cast<f32>(r.render_height);
    return m;
}

// ---- caps ------------------------------------------------------------------------------------------------------------

const upscale::UpscalerCaps& xess_caps() {
    static const upscale::UpscalerCaps kCaps = [] {
        upscale::UpscalerCaps c{};
        c.name = kXessName;
        c.display_name = "Intel XeSS Super Resolution";
        // Adapter MIT; the runtime is the user's Intel-licensed libxess.
        c.license = "LicenseRef-Intel-Simplified-Software-License";
        c.kind = upscale::UpscalerKind::Temporal;
        c.temporal = true;
        c.needs_depth = true;
        c.needs_motion_vectors = true;
        c.needs_jitter = true;
        c.accepts_reactive_mask = true; // responsive pixel mask
        c.accepts_transparency_mask = false;
        c.accepts_exposure = true;
        c.hdr_input = true;
        c.supports_dynamic_resolution = true;
        c.min_ratio = 1.f;
        c.max_ratio = 3.f;
        c.quality_modes = upscale::kAllQualityModes;
        c.apis = upscale::api_bit(upscale::UpscalerApi::Vulkan); // never selected for CPU dispatch
        return c;
    }();
    return kCaps;
}

// ---- XessUpscaler ------------------------------------------------------------------------------------------------------

XessUpscaler::XessUpscaler(std::shared_ptr<XessRuntime> runtime, const XessDeviceBinding& device)
    : m_runtime(std::move(runtime)), m_device(device) {}

XessUpscaler::~XessUpscaler() {
    if (m_ctx && m_runtime) {
        m_runtime->fn().destroy_context(m_ctx);
    }
    m_ctx = nullptr;
}

upscale::Extent2D XessUpscaler::render_size(upscale::Extent2D display, QualityMode mode) const {
    if (m_ctx && m_runtime && m_runtime->fn().get_input_resolution && display.valid()) {
        const FuseXess2d out{display.width, display.height};
        FuseXess2d in{0u, 0u};
        if (succeeded(m_runtime->fn().get_input_resolution(m_ctx, &out, to_xess_quality(mode), &in)) && in.x && in.y) {
            return {in.x, in.y};
        }
    }
    return upscale::render_extent(display, mode);
}

void XessUpscaler::invalidate_history(HistoryResetReason reason) {
    ++m_generation;
    m_lastReason = reason;
    m_accumulated = 0;
    m_pendingReset = true;
}

void XessUpscaler::bind_gpu_frame(const XessGpuFrame& frame) {
    m_gpu = frame;
    m_gpuBound = true;
}

bool XessUpscaler::ensure_context() {
    if (m_ctx) {
        return true;
    }
    if (!m_device.instance || !m_device.physical_device || !m_device.device) {
        m_lastResult = FUSE_XESS_RESULT_ERROR_UNINITIALIZED;
        return false;
    }
    FuseXessContext ctx = nullptr;
    m_lastResult = m_runtime->fn().vk_create_context(m_device.instance, m_device.physical_device, m_device.device, &ctx);
    if (!succeeded(m_lastResult) || !ctx) {
        return false;
    }
    m_ctx = ctx;
    m_initialised = false;
    return true;
}

UpscaleStatus XessUpscaler::evaluate(const upscale::UpscaleDispatch& dispatch, const UpscaleInputs& inputs,
                                     const upscale::TemporalUpscaleOutputs& outputs) {
    (void)dispatch; // GPU-only
    (void)outputs;  // result goes to the bound output image
    m_lastResult = FUSE_XESS_RESULT_SUCCESS;
    if (!m_runtime) {
        return UpscaleStatus::BackendUnavailable;
    }
    if (!m_gpuBound) {
        return UpscaleStatus::BackendUnavailable; // no native images for this frame
    }
    m_gpuBound = false; // one bind per frame
    const UpscaleResolution& r = inputs.resolution;
    if (!r.valid()) {
        return UpscaleStatus::InvalidInputs;
    }
    if (!caps().supports_ratio(std::max(r.scaleX(), r.scaleY()))) {
        return UpscaleStatus::UnsupportedRatio;
    }
    if (m_gpu.color.image == 0u || m_gpu.velocity.image == 0u || m_gpu.depth.image == 0u || m_gpu.output.image == 0u ||
        m_gpu.command_buffer == nullptr) {
        return UpscaleStatus::InvalidInputs;
    }
    if (!ensure_context()) {
        return UpscaleStatus::BackendUnavailable;
    }
    if (m_lastRenderW != r.render_width || m_lastRenderH != r.render_height) {
        if (m_lastRenderW != 0u) {
            invalidate_history(HistoryResetReason::ResolutionChange);
        }
        m_lastRenderW = r.render_width;
        m_lastRenderH = r.render_height;
    } else if (inputs.reset_history) {
        invalidate_history(HistoryResetReason::CameraCut);
    }
    m_mapped = map_upscale_inputs(inputs, m_gpu, m_pendingReset);
    const XessFunctions& fn = m_runtime->fn();
    const FuseXessQuality quality = to_xess_quality(m_quality);
    if (!m_initialised || m_initOutput.x != r.display_width || m_initOutput.y != r.display_height || m_initQuality != quality ||
        m_initFlags != m_mapped.init_flags) {
        if (fn.vk_build_pipelines) {
            m_lastResult = fn.vk_build_pipelines(m_ctx, m_device.pipeline_cache, true, m_mapped.init_flags);
            if (!succeeded(m_lastResult)) {
                return to_upscale_status(m_lastResult);
            }
        }
        const bool reinit = m_initialised || m_inits > 0u;
        const bool outputChanged = m_initOutput.x != r.display_width || m_initOutput.y != r.display_height;
        FuseXessVkInitParams ip{};
        ip.output_resolution = FuseXess2d{r.display_width, r.display_height};
        ip.quality_setting = quality;
        ip.init_flags = m_mapped.init_flags;
        ip.creation_node_mask = 1u;
        ip.visible_node_mask = 1u;
        ip.pipeline_cache = m_device.pipeline_cache;
        m_lastResult = fn.vk_init(m_ctx, &ip);
        if (!succeeded(m_lastResult)) {
            m_initialised = false;
            return to_upscale_status(m_lastResult);
        }
        if (fn.set_jitter_scale) {
            fn.set_jitter_scale(m_ctx, 1.f, 1.f);
        }
        if (fn.set_exposure_multiplier) {
            fn.set_exposure_multiplier(m_ctx, 1.f); // exposure travels per frame in exposure_scale
        }
        m_initialised = true;
        m_initOutput = ip.output_resolution;
        m_initQuality = quality;
        m_initFlags = m_mapped.init_flags;
        m_velocityScale[0] = m_velocityScale[1] = 0.f; // re-send after init
        m_mapped.execute.reset_history = 1u;
        ++m_inits;
        if (reinit) {
            invalidate_history(outputChanged ? HistoryResetReason::ResolutionChange : HistoryResetReason::QualityModeChange);
        }
    }
    if (m_velocityScale[0] != m_mapped.velocity_scale[0] || m_velocityScale[1] != m_mapped.velocity_scale[1]) {
        m_lastResult = fn.set_velocity_scale(m_ctx, m_mapped.velocity_scale[0], m_mapped.velocity_scale[1]);
        if (!succeeded(m_lastResult)) {
            return to_upscale_status(m_lastResult);
        }
        m_velocityScale[0] = m_mapped.velocity_scale[0];
        m_velocityScale[1] = m_mapped.velocity_scale[1];
    }
    m_lastResult = fn.vk_execute(m_ctx, m_gpu.command_buffer, &m_mapped.execute);
    if (!succeeded(m_lastResult)) {
        return to_upscale_status(m_lastResult);
    }
    m_pendingReset = false;
    ++m_accumulated;
    return UpscaleStatus::Ok;
}

// ---- registration ----------------------------------------------------------------------------------------------------

XessStatus register_xess_backend(upscale::UpscalerRegistry& registry, std::shared_ptr<XessRuntime> runtime,
                                 const XessDeviceBinding& device, std::string* detail) {
    if (!runtime) {
        if (detail) {
            *detail = "no runtime";
        }
        return XessStatus::NotConfigured;
    }
    const XessStatus s = runtime->probe_device(device, detail);
    if (s != XessStatus::Available) {
        return s;
    }
    {
        std::lock_guard<std::mutex> lock(g_bindingMutex);
        g_runtime = std::move(runtime);
        g_device = device;
    }
    if (!registry.register_backend(xess_caps(), &make_xess)) {
        if (detail) {
            *detail = "\"xess\" already registered";
        }
    }
    return XessStatus::Available;
}

void unregister_xess_backend(upscale::UpscalerRegistry& registry) {
    registry.unregister_backend(kXessName);
    std::lock_guard<std::mutex> lock(g_bindingMutex);
    g_runtime.reset();
    g_device = XessDeviceBinding{};
}

XessLoadResult probe_and_register_xess_backend(const XessConfig& project, const XessDeviceBinding& device) {
    XessLoadResult r = XessLoader::probe(project);
    if (r.status != XessStatus::Available || !r.runtime) {
        return r;
    }
    std::string detail;
    const XessStatus s = register_xess_backend(upscale::UpscalerRegistry::instance(), std::move(r.runtime), device, &detail);
    r.status = s;
    r.detail += " (" + detail + ")";
    return r;
}

UpscalerChoice select_upscaler_with_fallback(const upscale::UpscalerRegistry& registry, std::string_view preferred,
                                             const upscale::UpscalerRequirements& req) {
    UpscalerChoice c;
    if (const upscale::UpscalerCaps* p = registry.find(preferred); p && upscale::caps_satisfy(*p, req)) {
        c.caps = p;
        c.reason = "preferred backend";
        return c;
    }
    c.fell_back = true;
    if (const upscale::UpscalerCaps* s = registry.select(req)) {
        c.caps = s;
        c.reason = "preferred backend unavailable: another backend for the same API";
        return c;
    }
    if (req.api != upscale::UpscalerApi::Cpu) {
        upscale::UpscalerRequirements cpu = req;
        cpu.api = upscale::UpscalerApi::Cpu;
        if (const upscale::UpscalerCaps* s = registry.select(cpu)) {
            c.caps = s;
            c.reason = "preferred backend unavailable: in-tree CPU path";
            return c;
        }
    }
    c.reason = "no backend satisfies the requirements";
    return c;
}

} // namespace fuse::renderer::xess
