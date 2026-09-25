#pragma once

// WP-4.4 ILatencyProvider: the renderer's latency-reduction backends behind one interface.
//
//   None            NullLatencyProvider: no pacing, markers ignored (always available).
//   NvReflexPlugin  NVIDIA Reflex through the FUSE NVIDIA runtime provider (docs/nvidia-plugin.md) and its latency
//                   extension ABI (fuse_nv_latency_abi.h). FUSE never links an NVIDIA binary.
//   NvLowLatency2   VK_NV_low_latency2 on the renderer's own device (vkSetLatencySleepModeNV / vkLatencySleepNV /
//                   vkSetLatencyMarkerNV / vkGetLatencyTimingsNV), when the extension was enabled at device creation.
//   AmdAntiLag      AMD Anti-Lag 2 on Vulkan = VK_AMD_anti_lag (vkAntiLagUpdateAMD), resolved at run time when the
//                   extension was enabled. The AMD Anti-Lag 2 SDK itself (MIT) is a D3D11 / D3D12 header over the
//                   driver's DX extension: it has no Vulkan path, so it is not vendored; the Vulkan extension is the
//                   Anti-Lag 2 interface on Vulkan.
//
// Frame protocol (Reflex / Anti-Lag 2 / low_latency2 agree on it):
//   provider.sleep(frame);                              // may block: the provider releases the CPU just in time
//   marker(frame, InputSample); marker(frame, SimulationStart) ... SimulationEnd;
//   marker(frame, RenderSubmitStart) ... RenderSubmitEnd;
//   marker(frame, PresentStart); present; marker(frame, PresentEnd);
// The hardware-facing backends talk to their API through a small dispatch table (C function pointers), filled
// from the real extension / plugin on hardware and by a mock in the CPU gates (fuse_rp_latency_*): everything above
// the table (mode / limiter mapping, frame and present ids, marker mapping, report conversion) is gated on CPU.

#include <fuse/renderer/present/latency/fuse_nv_latency_abi.h>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {
class VulkanDevice;
namespace nvidia {
class NvPlugin;
}
} // namespace fuse::renderer

namespace fuse::renderer::present {

/// Frame markers. Numeric values = VkLatencyMarkerNV = FUSE_NV_LATENCY_MARKER_* (0..7).
enum class LatencyMarker : u8 {
    SimulationStart = 0,
    SimulationEnd = 1,
    RenderSubmitStart = 2,
    RenderSubmitEnd = 3,
    PresentStart = 4,
    PresentEnd = 5,
    InputSample = 6,
    TriggerFlash = 7,
    Count = 8,
};
const char* latency_marker_name(LatencyMarker marker);

enum class LatencyMode : u8 {
    Off = 0,
    LowLatency,      ///< Reflex On / Anti-Lag On / low_latency2 lowLatencyMode
    LowLatencyBoost, ///< Reflex On + Boost / low_latency2 lowLatencyBoost (Anti-Lag: same as LowLatency)
};

enum class LatencyBackend : u8 {
    None = 0,
    NvReflexPlugin,
    NvLowLatency2,
    AmdAntiLag,
    Count,
};
const char* latency_backend_name(LatencyBackend backend);

struct LatencySettings {
    LatencyMode mode = LatencyMode::Off;
    f32 fps_limit = 0.f; ///< frame limiter (0 = none); providers without a limiter ignore it
};

struct LatencyCaps {
    bool low_latency = false; ///< the provider paces the CPU (sleep() can block)
    bool boost = false;
    bool markers = false;     ///< markers reach the driver / runtime
    bool fps_limit = false;
    bool reports = false;     ///< timings() returns driver / runtime frame reports
};

/// One frame of latency timings (microseconds on the provider's clock; 0 = not recorded). Field set of
/// VkLatencyTimingsFrameReportNV / FuseNvLatencyFrameReport.
struct LatencyFrameReport {
    u64 frame_id = 0;
    u64 input_sample_us = 0;
    u64 sim_start_us = 0, sim_end_us = 0;
    u64 render_submit_start_us = 0, render_submit_end_us = 0;
    u64 present_start_us = 0, present_end_us = 0;
    u64 driver_start_us = 0, driver_end_us = 0;
    u64 os_render_queue_start_us = 0, os_render_queue_end_us = 0;
    u64 gpu_render_start_us = 0, gpu_render_end_us = 0;
    /// input -> GPU end (the part of click-to-photon the provider sees; display scan-out is not included).
    u64 input_to_gpu_end_us() const { return gpu_render_end_us > input_sample_us ? gpu_render_end_us - input_sample_us : 0u; }
};

class ILatencyProvider {
public:
    virtual ~ILatencyProvider() = default;
    virtual LatencyBackend backend() const = 0;
    virtual const char* name() const = 0;
    virtual LatencyCaps caps() const = 0;
    /// False when the backend rejects the settings (they are then not applied).
    virtual bool setSettings(const LatencySettings& settings) = 0;
    virtual LatencySettings settings() const = 0;
    /// Frame start (before input sampling). Blocks while the provider paces the CPU.
    virtual void sleep(u64 frameId) = 0;
    virtual void marker(u64 frameId, LatencyMarker marker) = 0;
    /// Copies up to `capacity` recent frame reports (oldest first); returns the count.
    virtual u32 timings(LatencyFrameReport* out, u32 capacity) = 0;
    /// True when this provider paces the CPU with its current settings.
    bool pacing() const { return caps().low_latency && settings().mode != LatencyMode::Off; }
};

// ---- None ---------------------------------------------------------------------------------------------------

class NullLatencyProvider final : public ILatencyProvider {
public:
    LatencyBackend backend() const override { return LatencyBackend::None; }
    const char* name() const override { return "none"; }
    LatencyCaps caps() const override { return LatencyCaps{}; }
    bool setSettings(const LatencySettings& settings) override {
        m_settings = settings;
        return true;
    }
    LatencySettings settings() const override { return m_settings; }
    void sleep(u64) override {}
    void marker(u64, LatencyMarker) override {}
    u32 timings(LatencyFrameReport*, u32) override { return 0u; }

private:
    LatencySettings m_settings{};
};

// ---- Reflex through the NVIDIA runtime provider -------------------------------------------------------------

/// Resolves the latency extension from the provider's entry point: calls `getApi` with the host ABI version and
/// validates the table (ABI major, struct_size covering every function pointer, no null function). False with a
/// reason otherwise.
bool resolve_nv_latency_api(FuseNvPluginGetLatencyApiFn getApi, FuseNvLatencyApi& out, std::string& reason);
/// From a loaded plugin: requires FUSE_NV_FEATURE_REFLEX support and the FUSE_NV_LATENCY_ENTRY_NAME export.
bool resolve_nv_latency_api(const nvidia::NvPlugin& plugin, FuseNvLatencyApi& out, std::string& reason);

class NvReflexPluginLatencyProvider final : public ILatencyProvider {
public:
    /// `api` must come from resolve_nv_latency_api; `context` is the provider's FuseNvContext.
    NvReflexPluginLatencyProvider(const FuseNvLatencyApi& api, FuseNvContext* context);
    LatencyBackend backend() const override { return LatencyBackend::NvReflexPlugin; }
    const char* name() const override { return "nv_reflex_plugin"; }
    LatencyCaps caps() const override;
    bool setSettings(const LatencySettings& settings) override;
    LatencySettings settings() const override { return m_settings; }
    void sleep(u64 frameId) override;
    void marker(u64 frameId, LatencyMarker marker) override;
    u32 timings(LatencyFrameReport* out, u32 capacity) override;
    FuseNvStatus lastStatus() const { return m_lastStatus; }

private:
    static constexpr u32 kReportBatch = 64u;
    FuseNvLatencyApi m_api{};
    FuseNvContext* m_context = nullptr;
    LatencySettings m_settings{};
    FuseNvStatus m_lastStatus = FUSE_NV_OK;
    FuseNvLatencyFrameReport m_scratch[kReportBatch]{};
};

// ---- VK_NV_low_latency2 ---------------------------------------------------------------------------------------

/// The four entry points of VK_NV_low_latency2 as seen by the provider (the Vulkan binding below fills it; the
/// gates use a mock). Return values: VkResult (0 = VK_SUCCESS).
struct LowLatency2Dispatch {
    void* user = nullptr;
    /// vkSetLatencySleepModeNV(swapchain, {lowLatencyMode, lowLatencyBoost, minimumIntervalUs}).
    i32 (*set_sleep_mode)(void* user, bool lowLatency, bool boost, u32 minimumIntervalUs) = nullptr;
    /// vkLatencySleepNV(swapchain, {semaphore, value}) and the host wait for the timeline value.
    i32 (*sleep)(void* user, u64 signalValue) = nullptr;
    /// vkSetLatencyMarkerNV(swapchain, {presentID, marker}).
    void (*set_marker)(void* user, u64 presentId, u32 marker) = nullptr;
    /// vkGetLatencyTimingsNV: up to `capacity` reports (presentID in LatencyFrameReport::frame_id).
    u32 (*get_timings)(void* user, LatencyFrameReport* out, u32 capacity) = nullptr;
    bool valid() const { return set_sleep_mode != nullptr && sleep != nullptr && set_marker != nullptr && get_timings != nullptr; }
};

class NvLowLatency2Provider final : public ILatencyProvider {
public:
    explicit NvLowLatency2Provider(const LowLatency2Dispatch& dispatch);
    LatencyBackend backend() const override { return LatencyBackend::NvLowLatency2; }
    const char* name() const override { return "vk_nv_low_latency2"; }
    LatencyCaps caps() const override;
    bool setSettings(const LatencySettings& settings) override;
    LatencySettings settings() const override { return m_settings; }
    void sleep(u64 frameId) override;
    void marker(u64 frameId, LatencyMarker marker) override;
    u32 timings(LatencyFrameReport* out, u32 capacity) override;
    /// VK_KHR_present_id value this provider uses for `frameId` (the presenter must chain VkPresentIdKHR with it and
    /// VkLatencySubmissionPresentIdNV on the frame's queue submits). presentID must be non-zero and increasing.
    static u64 present_id(u64 frameId) { return frameId + 1u; }
    u64 lastSleepValue() const { return m_sleepValue; }

private:
    LowLatency2Dispatch m_dispatch{};
    LatencySettings m_settings{};
    u64 m_sleepValue = 0; ///< timeline value of the last vkLatencySleepNV
};

// ---- AMD Anti-Lag 2 (VK_AMD_anti_lag) ---------------------------------------------------------------------------

/// vkAntiLagUpdateAMD arguments (VkAntiLagDataAMD + optional VkAntiLagPresentationInfoAMD).
struct AntiLagUpdate {
    u32 mode = 0;                ///< VkAntiLagModeAMD: 0 DRIVER_CONTROL, 1 ON, 2 OFF
    u32 max_fps = 0;             ///< 0 = no limit
    bool has_presentation = false;
    u32 stage = 0;               ///< VkAntiLagStageAMD: 0 INPUT, 1 PRESENT
    u64 frame_index = 0;
};
inline constexpr u32 kAntiLagModeDriverControl = 0u, kAntiLagModeOn = 1u, kAntiLagModeOff = 2u;
inline constexpr u32 kAntiLagStageInput = 0u, kAntiLagStagePresent = 1u;

struct AntiLagDispatch {
    void* user = nullptr;
    void (*update)(void* user, const AntiLagUpdate& update) = nullptr;
    bool valid() const { return update != nullptr; }
};

class AmdAntiLagProvider final : public ILatencyProvider {
public:
    explicit AmdAntiLagProvider(const AntiLagDispatch& dispatch);
    LatencyBackend backend() const override { return LatencyBackend::AmdAntiLag; }
    const char* name() const override { return "vk_amd_anti_lag"; }
    LatencyCaps caps() const override;
    bool setSettings(const LatencySettings& settings) override;
    LatencySettings settings() const override { return m_settings; }
    /// vkAntiLagUpdateAMD(mode, maxFPS, {INPUT, frameIndex}): the driver's sleep point, right before input sampling.
    void sleep(u64 frameId) override;
    /// PresentStart -> vkAntiLagUpdateAMD(mode, maxFPS, {PRESENT, frameIndex}); other markers are not used.
    void marker(u64 frameId, LatencyMarker marker) override;
    u32 timings(LatencyFrameReport*, u32) override { return 0u; }

private:
    AntiLagUpdate makeUpdate(u32 stage, u64 frameId) const;
    AntiLagDispatch m_dispatch{};
    LatencySettings m_settings{};
};

// ---- Vulkan bindings (FUSE_VULKAN_BACKEND; unavailable elsewhere) ------------------------------------------------

/// Owns the resolved VK_NV_low_latency2 entry points and the timeline semaphore vkLatencySleepNV signals.
class VkLowLatency2Binding {
public:
    VkLowLatency2Binding() = default;
    ~VkLowLatency2Binding();
    VkLowLatency2Binding(const VkLowLatency2Binding&) = delete;
    VkLowLatency2Binding& operator=(const VkLowLatency2Binding&) = delete;
    /// False (with a reason) unless the device enabled VK_NV_low_latency2 and every entry point resolves.
    /// `swapchain` = VkSwapchainKHR the latency state belongs to.
    bool init(const VulkanDevice& device, void* swapchain, std::string& reason);
    void destroy();
    LowLatency2Dispatch dispatch();

private:
    void* m_device = nullptr;    ///< VkDevice
    void* m_swapchain = nullptr; ///< VkSwapchainKHR
    void* m_semaphore = nullptr; ///< VkSemaphore (timeline)
    void* m_fn[5] = {};          ///< set sleep mode, sleep, set marker, get timings, wait semaphores
};

/// Owns the resolved vkAntiLagUpdateAMD.
class VkAntiLagBinding {
public:
    /// False (with a reason) unless the device enabled VK_AMD_anti_lag and vkAntiLagUpdateAMD resolves.
    bool init(const VulkanDevice& device, std::string& reason);
    AntiLagDispatch dispatch();

private:
    void* m_device = nullptr;
    void* m_update = nullptr;
};

// ---- Selection ----------------------------------------------------------------------------------------------------

enum class LatencyPreference : u8 { Auto = 0, None, NvReflexPlugin, NvLowLatency2, AmdAntiLag };

struct LatencyAvailability {
    bool nv_reflex_plugin = false; ///< plugin loaded, REFLEX supported, latency extension resolved
    bool nv_low_latency2 = false;  ///< VK_NV_low_latency2 enabled and bound to the swapchain
    bool amd_anti_lag = false;     ///< VK_AMD_anti_lag enabled
};

/// Auto order: Reflex plugin (Streamline PCL / DLSS-G integration) > VK_NV_low_latency2 > VK_AMD_anti_lag > None.
/// An explicit preference that is unavailable yields None; `reason` says why.
LatencyBackend select_latency_backend(LatencyPreference preference, const LatencyAvailability& availability,
                                      const char** reason = nullptr);

/// What create_latency_provider may bind to. Null / empty members are simply unavailable.
struct LatencyProviderSources {
    const nvidia::NvPlugin* plugin = nullptr;
    const VulkanDevice* device = nullptr;
    void* swapchain = nullptr; ///< VkSwapchainKHR (VK_NV_low_latency2 state is per swapchain)
};

/// Owns the provider and whatever binding it runs on.
struct LatencyProviderHandle {
    // Bindings first: the provider (declared last) is destroyed before the entry points it calls.
    std::unique_ptr<VkLowLatency2Binding> lowLatency2;
    std::unique_ptr<VkAntiLagBinding> antiLag;
    std::unique_ptr<ILatencyProvider> provider; ///< never null after create_latency_provider (None at worst)
    std::string reason; ///< why this backend (and why others were not available)
};

LatencyProviderHandle create_latency_provider(LatencyPreference preference, const LatencyProviderSources& sources);

} // namespace fuse::renderer::present
