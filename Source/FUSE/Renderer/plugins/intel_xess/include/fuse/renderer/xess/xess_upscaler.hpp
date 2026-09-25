#pragma once

// WP-4.3: Intel XeSS Super Resolution as an optional temporal upscaler ("xess") behind upscale::ITemporalUpscaler.
//
// FUSE never links or ships XeSS. XessLoader opens the user's libxess (libxess.dll / libxess.so) from a configured
// directory (project setting, else env FUSE_XESS_SDK_DIR / FUSE_XESS_LIB) and resolves the entry points declared in
// xess_api.h (written from Intel's public API documentation; no Intel header is vendored). Every failure is a status
// with a reason, never a crash: without the runtime, a supported GPU or driver, "xess" is simply not registered and
// the registry falls back to the in-tree / other backends (select_upscaler_with_fallback).
//
//   XessLoader::probe(project)                FUSE_ENABLE_XESS_PLUGIN=ON only
//   XessLoader::load(config) / load_file()    any build (the CI mock gates drive this with a mock libxess)
//   register_xess_backend(registry, runtime, device)   creates + destroys one context on `device` first
//
// Like the NVIDIA backends, "xess" is GPU-only (caps.apis = Vulkan): bind the frame's native Vulkan images with
// bind_gpu_frame() before each evaluate(), which records xessVKExecute into XessGpuFrame::command_buffer.
//
// Parameter conventions (FUSE -> XeSS; UpscaleInputs conventions in upscale_inputs.hpp):
//   velocity   the velocity texture is FUSE's UV motion (cur - prev, render resolution, unjittered) bound as is;
//              XeSS wants render-pixel velocity pointing to the previous position, so xessSetVelocityScale(-render_w,
//              -render_h). Low-res, unjittered MVs: no HIGH_RES_MV / JITTERED_MV / USE_NDC_VELOCITY flag.
//   jitter     XeSS's jitterOffset is the projection (content) offset, the negated FUSE sample offset:
//              jitter_offset = -jitter_px (same sign as FSR's, fsr3_jitter_offset); jitter scale (1, 1).
//   depth      the hardware depth buffer; INVERTED_DEPTH when reverse-Z (XessGpuFrame::depth_reversed_z).
//   exposure   UpscaleInputs::exposure -> exposure_scale (or an exposure texture: EXPOSURE_SCALE_TEXTURE).
//   reactive   an optional responsive-pixel mask image -> RESPONSIVE_PIXEL_MASK.
//   quality    by ratio: NativeAA -> AA (1.0x), UltraQuality 1.3x -> ULTRA_QUALITY_PLUS, Quality 1.5x -> ULTRA_QUALITY,
//              Balanced 1.7x -> QUALITY, Performance 2.0x -> BALANCED, UltraPerformance 3.0x -> ULTRA_PERFORMANCE.
// Output resolution, quality or flag changes re-initialise the context (xessVKInit); a render-size change or
// reset_history resets the history. Steady-state evaluate() makes no heap allocation.

#include <fuse/renderer/upscale/upscaler.hpp>
#include <fuse/renderer/xess/xess_api.h>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace fuse::renderer::xess {

inline constexpr const char* kXessName = "xess";

enum class XessStatus : u8 {
    Available,
    DisabledAtBuild,    ///< FUSE_ENABLE_XESS_PLUGIN=OFF: automatic probing compiled out
    NotConfigured,      ///< no directory in project settings or FUSE_XESS_SDK_DIR
    LibraryNotFound,
    LoadFailed,
    EntryPointMissing,  ///< a required xess* export is absent (the detail names it)
    VersionUnsupported, ///< xessGetVersion failed or reports a major outside kMinMajor..kMaxMajor
    DeviceUnsupported,  ///< context creation: XESS_RESULT_ERROR_UNSUPPORTED_DEVICE
    DriverUnsupported,  ///< context creation: XESS_RESULT_ERROR_UNSUPPORTED_DRIVER
    ContextFailed,      ///< context creation failed otherwise / no device given
};

[[nodiscard]] std::string_view xess_status_name(XessStatus s) noexcept;
[[nodiscard]] const char* xess_result_name(FuseXessResult r) noexcept;

/// Native Vulkan device the contexts are created on.
struct XessDeviceBinding {
    void* instance = nullptr;        ///< VkInstance
    void* physical_device = nullptr; ///< VkPhysicalDevice
    void* device = nullptr;          ///< VkDevice
    u64 pipeline_cache = 0;          ///< VkPipelineCache (optional)
};

struct XessConfig {
    std::string sdk_dir;      ///< env FUSE_XESS_SDK_DIR
    std::string library_name; ///< env FUSE_XESS_LIB; default default_xess_library_name()
};

[[nodiscard]] XessConfig resolve_xess_config(const XessConfig& project);
/// "libxess.dll" on Windows, "libxess.so" elsewhere.
[[nodiscard]] std::string_view default_xess_library_name() noexcept;

/// Resolved entry points (optional ones may be null).
struct XessFunctions {
    PFN_fuseXessGetVersion get_version = nullptr;
    PFN_fuseXessDestroyContext destroy_context = nullptr;
    PFN_fuseXessVkCreateContext vk_create_context = nullptr;
    PFN_fuseXessVkInit vk_init = nullptr;
    PFN_fuseXessVkExecute vk_execute = nullptr;
    PFN_fuseXessSetVelocityScale set_velocity_scale = nullptr;
    // optional
    PFN_fuseXessVkBuildPipelines vk_build_pipelines = nullptr;
    PFN_fuseXessGetInputResolution get_input_resolution = nullptr;
    PFN_fuseXessSetJitterScale set_jitter_scale = nullptr;
    PFN_fuseXessSetExposureMultiplier set_exposure_multiplier = nullptr;
};

/// A loaded libxess. Owns the library handle.
class XessRuntime {
public:
    static constexpr u16 kMinMajor = 1;
    static constexpr u16 kMaxMajor = 2;

    ~XessRuntime();
    XessRuntime(const XessRuntime&) = delete;
    XessRuntime& operator=(const XessRuntime&) = delete;

    [[nodiscard]] const XessFunctions& fn() const noexcept { return m_fn; }
    [[nodiscard]] const FuseXessVersion& version() const noexcept { return m_version; }
    [[nodiscard]] const std::string& library_path() const noexcept { return m_path; }
    [[nodiscard]] void* symbol(const char* name) const noexcept;

    /// Creates and destroys one context on `device`: Available, DeviceUnsupported, DriverUnsupported or ContextFailed.
    XessStatus probe_device(const XessDeviceBinding& device, std::string* detail = nullptr) const;

private:
    friend struct XessLoader;
    XessRuntime() = default;
    void* m_lib = nullptr; // fuse-lint-allow(ownership): OS library handle, closed in ~XessRuntime
    XessFunctions m_fn{};
    FuseXessVersion m_version{};
    std::string m_path;
};

struct XessLoadResult {
    XessStatus status = XessStatus::NotConfigured;
    std::unique_ptr<XessRuntime> runtime; ///< non-null only when Available
    std::string detail;
};

struct XessLoader {
    [[nodiscard]] static XessLoadResult load(const XessConfig& config);
    [[nodiscard]] static XessLoadResult load_file(const std::string& library_path);
    /// resolve_xess_config + load; DisabledAtBuild without touching the file system when FUSE_ENABLE_XESS_PLUGIN=OFF.
    [[nodiscard]] static XessLoadResult probe(const XessConfig& project);
    [[nodiscard]] static bool enabled_at_build() noexcept;
};

// ---- mapping ---------------------------------------------------------------------------------------------------

[[nodiscard]] FuseXessQuality to_xess_quality(upscale::QualityMode mode) noexcept;
/// Per-axis ratio XeSS documents for a quality setting (0 when unknown).
[[nodiscard]] f32 xess_quality_ratio(FuseXessQuality q) noexcept;
[[nodiscard]] upscale::UpscaleStatus to_upscale_status(FuseXessResult r) noexcept;

/// Native images of one frame (image == 0: not bound). width / height 0 = the contract's extent.
struct XessGpuFrame {
    FuseXessVkImageViewInfo color{};      ///< render res, jittered linear HDR
    FuseXessVkImageViewInfo velocity{};   ///< render res, FUSE UV motion
    FuseXessVkImageViewInfo depth{};      ///< render res, hardware depth
    FuseXessVkImageViewInfo exposure{};   ///< optional 1x1 exposure scale texture
    FuseXessVkImageViewInfo responsive{}; ///< optional render-res responsive (reactive) mask
    FuseXessVkImageViewInfo output{};     ///< display res
    void* command_buffer = nullptr;       ///< VkCommandBuffer
    bool depth_reversed_z = true;
    bool hdr = true;
    bool debug_flip_jitter_sign = false;  ///< HW bring-up aid: pass +jitter_px instead of -jitter_px
};

struct XessMappedFrame {
    FuseXessVkExecuteParams execute{};
    u32 init_flags = 0;
    f32 velocity_scale[2] = {0.f, 0.f};
};

/// UpscaleInputs + native images -> xessVKExecute parameters, init flags and velocity scale (pure; gates pin fields).
[[nodiscard]] XessMappedFrame map_upscale_inputs(const UpscaleInputs& in, const XessGpuFrame& gpu, bool reset) noexcept;

// ---- "xess" -------------------------------------------------------------------------------------------------------

[[nodiscard]] const upscale::UpscalerCaps& xess_caps();

class XessUpscaler final : public upscale::ITemporalUpscaler {
public:
    XessUpscaler(std::shared_ptr<XessRuntime> runtime, const XessDeviceBinding& device);
    ~XessUpscaler() override;
    XessUpscaler(const XessUpscaler&) = delete;
    XessUpscaler& operator=(const XessUpscaler&) = delete;

    const upscale::UpscalerCaps& caps() const override { return xess_caps(); }
    /// xessGetInputResolution once a context exists, else render_extent (same ratios by construction).
    upscale::Extent2D render_size(upscale::Extent2D display, upscale::QualityMode mode) const override;
    upscale::UpscaleStatus evaluate(const upscale::UpscaleDispatch& dispatch, const UpscaleInputs& inputs,
                                    const upscale::TemporalUpscaleOutputs& outputs) override;
    const upscale::IJitterProvider& jitter_provider() const override { return m_jitter; }
    void invalidate_history(upscale::HistoryResetReason reason) override;
    u32 history_generation() const override { return m_generation; }
    upscale::HistoryResetReason last_reset_reason() const override { return m_lastReason; }
    u32 accumulated_frames() const override { return m_accumulated; }

    void bind_gpu_frame(const XessGpuFrame& frame);
    void set_quality(upscale::QualityMode mode) { m_quality = mode; }
    [[nodiscard]] FuseXessResult last_result() const { return m_lastResult; }
    [[nodiscard]] u32 context_inits() const { return m_inits; }
    [[nodiscard]] const XessMappedFrame& last_mapped() const { return m_mapped; }

private:
    bool ensure_context();

    std::shared_ptr<XessRuntime> m_runtime;
    XessDeviceBinding m_device{};
    FuseXessContext m_ctx = nullptr; // fuse-lint-allow(ownership): runtime-owned, xessDestroyContext
    upscale::HaltonJitterProvider m_jitter{};
    XessGpuFrame m_gpu{};
    bool m_gpuBound = false;
    bool m_pendingReset = true;
    bool m_initialised = false;
    upscale::QualityMode m_quality = upscale::QualityMode::Quality;
    FuseXess2d m_initOutput{0u, 0u};
    FuseXessQuality m_initQuality = 0;
    u32 m_initFlags = 0;
    f32 m_velocityScale[2] = {0.f, 0.f};
    u32 m_inits = 0;
    u32 m_generation = 0;
    u32 m_accumulated = 0;
    u32 m_lastRenderW = 0, m_lastRenderH = 0;
    upscale::HistoryResetReason m_lastReason = upscale::HistoryResetReason::None;
    FuseXessResult m_lastResult = FUSE_XESS_RESULT_SUCCESS;
    XessMappedFrame m_mapped{};
};

/// Registers "xess" when `runtime` creates a context on `device` (XessRuntime::probe_device); factories bind to the
/// runtime and device process-wide until unregister_xess_backend(). Returns the probe status.
XessStatus register_xess_backend(upscale::UpscalerRegistry& registry, std::shared_ptr<XessRuntime> runtime,
                                 const XessDeviceBinding& device, std::string* detail = nullptr);
void unregister_xess_backend(upscale::UpscalerRegistry& registry);

/// Start-up convenience: XessLoader::probe(project) then register_xess_backend(UpscalerRegistry::instance()).
XessLoadResult probe_and_register_xess_backend(const XessConfig& project, const XessDeviceBinding& device);

/// Upscaler choice with explicit fallback: `preferred` when registered and satisfying `req`; otherwise
/// registry.select(req) (another backend for the same API); otherwise, when req.api is not Cpu, the in-tree CPU path
/// registry.select(req with api = Cpu) (native_taau). `fell_back` / `reason` say which step answered.
struct UpscalerChoice {
    const upscale::UpscalerCaps* caps = nullptr;
    bool fell_back = false;
    const char* reason = "";
};
[[nodiscard]] UpscalerChoice select_upscaler_with_fallback(const upscale::UpscalerRegistry& registry, std::string_view preferred,
                                                           const upscale::UpscalerRequirements& req);

} // namespace fuse::renderer::xess
