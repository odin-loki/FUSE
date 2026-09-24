// WP-4.4 latency providers (see include/fuse/renderer/present/latency/latency_provider.hpp). Vulkan-free except the
// VK_NV_low_latency2 / VK_AMD_anti_lag bindings in vk_latency_bindings.cpp.
#include <fuse/renderer/present/latency/latency_provider.hpp>

#include <fuse/renderer/nvidia/nv_plugin_loader.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace fuse::renderer::present {

namespace {
u32 minimumIntervalUs(f32 fpsLimit) {
    if (!(fpsLimit > 0.f) || !std::isfinite(fpsLimit)) {
        return 0u;
    }
    const f64 us = 1.0e6 / static_cast<f64>(fpsLimit);
    return us >= 4.0e9 ? 0u : static_cast<u32>(std::lround(us));
}

bool validSettings(const LatencySettings& s) {
    return std::isfinite(s.fps_limit) && s.fps_limit >= 0.f && s.mode <= LatencyMode::LowLatencyBoost;
}

LatencyFrameReport fromAbi(const FuseNvLatencyFrameReport& r) {
    LatencyFrameReport o{};
    o.frame_id = r.frame_id;
    o.input_sample_us = r.input_sample_us;
    o.sim_start_us = r.sim_start_us;
    o.sim_end_us = r.sim_end_us;
    o.render_submit_start_us = r.render_submit_start_us;
    o.render_submit_end_us = r.render_submit_end_us;
    o.present_start_us = r.present_start_us;
    o.present_end_us = r.present_end_us;
    o.driver_start_us = r.driver_start_us;
    o.driver_end_us = r.driver_end_us;
    o.os_render_queue_start_us = r.os_render_queue_start_us;
    o.os_render_queue_end_us = r.os_render_queue_end_us;
    o.gpu_render_start_us = r.gpu_render_start_us;
    o.gpu_render_end_us = r.gpu_render_end_us;
    return o;
}
} // namespace

const char* latency_marker_name(LatencyMarker marker) {
    switch (marker) {
    case LatencyMarker::SimulationStart: return "simulation_start";
    case LatencyMarker::SimulationEnd: return "simulation_end";
    case LatencyMarker::RenderSubmitStart: return "render_submit_start";
    case LatencyMarker::RenderSubmitEnd: return "render_submit_end";
    case LatencyMarker::PresentStart: return "present_start";
    case LatencyMarker::PresentEnd: return "present_end";
    case LatencyMarker::InputSample: return "input_sample";
    case LatencyMarker::TriggerFlash: return "trigger_flash";
    default: return "?";
    }
}

const char* latency_backend_name(LatencyBackend backend) {
    switch (backend) {
    case LatencyBackend::None: return "none";
    case LatencyBackend::NvReflexPlugin: return "nv_reflex_plugin";
    case LatencyBackend::NvLowLatency2: return "vk_nv_low_latency2";
    case LatencyBackend::AmdAntiLag: return "vk_amd_anti_lag";
    default: return "?";
    }
}

// ---- Reflex through the plugin ---------------------------------------------------------------------------------

bool resolve_nv_latency_api(FuseNvPluginGetLatencyApiFn getApi, FuseNvLatencyApi& out, std::string& reason) {
    out = FuseNvLatencyApi{};
    if (getApi == nullptr) {
        reason = "provider exports no " FUSE_NV_LATENCY_ENTRY_NAME " (latency extension absent)";
        return false;
    }
    FuseNvLatencyApi api{};
    api.struct_size = sizeof(FuseNvLatencyApi);
    const FuseNvStatus status = getApi(FUSE_NV_LATENCY_ABI_VERSION, &api);
    if (status == FUSE_NV_ERR_ABI_MISMATCH || FUSE_NV_PLUGIN_ABI_MAJOR_OF(api.abi_version) != FUSE_NV_LATENCY_ABI_MAJOR) {
        reason = "latency extension ABI major mismatch";
        return false;
    }
    if (status != FUSE_NV_OK) {
        reason = "latency extension entry point failed";
        return false;
    }
    // Every function pointer this host calls must lie inside the provider's struct_size.
    if (api.struct_size < offsetof(FuseNvLatencyApi, get_reports) + sizeof(api.get_reports)) {
        reason = "latency extension table truncated (struct_size too small)";
        return false;
    }
    if (api.set_options == nullptr || api.sleep == nullptr || api.set_marker == nullptr || api.get_reports == nullptr) {
        reason = "latency extension table has null entry points";
        return false;
    }
    out = api;
    reason = "ok";
    return true;
}

bool resolve_nv_latency_api(const nvidia::NvPlugin& plugin, FuseNvLatencyApi& out, std::string& reason) {
    out = FuseNvLatencyApi{};
    if (!plugin.supports(FUSE_NV_FEATURE_REFLEX)) {
        reason = "NVIDIA provider does not support Reflex on this adapter";
        return false;
    }
    void* symbol = plugin.symbol(FUSE_NV_LATENCY_ENTRY_NAME);
    return resolve_nv_latency_api(reinterpret_cast<FuseNvPluginGetLatencyApiFn>(symbol), out, reason);
}

NvReflexPluginLatencyProvider::NvReflexPluginLatencyProvider(const FuseNvLatencyApi& api, FuseNvContext* context)
    : m_api(api), m_context(context) {}

LatencyCaps NvReflexPluginLatencyProvider::caps() const {
    LatencyCaps c{};
    c.low_latency = true;
    c.boost = true;
    c.markers = true;
    c.fps_limit = true;
    c.reports = true;
    return c;
}

bool NvReflexPluginLatencyProvider::setSettings(const LatencySettings& settings) {
    if (!validSettings(settings)) {
        return false;
    }
    FuseNvLatencyOptions o{};
    o.struct_size = sizeof(FuseNvLatencyOptions);
    o.low_latency = settings.mode != LatencyMode::Off ? 1u : 0u;
    o.boost = settings.mode == LatencyMode::LowLatencyBoost ? 1u : 0u;
    o.minimum_interval_us = minimumIntervalUs(settings.fps_limit);
    o.use_markers_to_optimize = o.low_latency;
    m_lastStatus = m_api.set_options(m_context, &o);
    if (m_lastStatus != FUSE_NV_OK) {
        return false;
    }
    m_settings = settings;
    return true;
}

void NvReflexPluginLatencyProvider::sleep(u64 frameId) { m_lastStatus = m_api.sleep(m_context, frameId); }

void NvReflexPluginLatencyProvider::marker(u64 frameId, LatencyMarker marker) {
    if (marker >= LatencyMarker::Count) {
        return;
    }
    m_lastStatus = m_api.set_marker(m_context, frameId, static_cast<FuseNvLatencyMarker>(marker));
}

u32 NvReflexPluginLatencyProvider::timings(LatencyFrameReport* out, u32 capacity) {
    if (out == nullptr || capacity == 0u) {
        return 0u;
    }
    const u32 n = std::min(capacity, kReportBatch);
    for (u32 i = 0; i < n; ++i) {
        m_scratch[i] = FuseNvLatencyFrameReport{};
        m_scratch[i].struct_size = sizeof(FuseNvLatencyFrameReport);
    }
    u32 count = 0;
    m_lastStatus = m_api.get_reports(m_context, m_scratch, n, &count);
    if (m_lastStatus != FUSE_NV_OK) {
        return 0u;
    }
    count = std::min(count, n);
    for (u32 i = 0; i < count; ++i) {
        out[i] = fromAbi(m_scratch[i]);
    }
    return count;
}

// ---- VK_NV_low_latency2 -----------------------------------------------------------------------------------------

NvLowLatency2Provider::NvLowLatency2Provider(const LowLatency2Dispatch& dispatch) : m_dispatch(dispatch) {}

LatencyCaps NvLowLatency2Provider::caps() const {
    LatencyCaps c{};
    c.low_latency = true;
    c.boost = true;
    c.markers = true;
    c.fps_limit = true;
    c.reports = true;
    return c;
}

bool NvLowLatency2Provider::setSettings(const LatencySettings& settings) {
    if (!validSettings(settings) || !m_dispatch.valid()) {
        return false;
    }
    const bool low = settings.mode != LatencyMode::Off;
    const bool boost = settings.mode == LatencyMode::LowLatencyBoost;
    if (m_dispatch.set_sleep_mode(m_dispatch.user, low, boost, minimumIntervalUs(settings.fps_limit)) != 0) {
        return false;
    }
    m_settings = settings;
    return true;
}

void NvLowLatency2Provider::sleep(u64) {
    if (!m_dispatch.valid()) {
        return;
    }
    // vkLatencySleepNV signals the timeline semaphore when the CPU may start: one new value per frame.
    ++m_sleepValue;
    m_dispatch.sleep(m_dispatch.user, m_sleepValue);
}

void NvLowLatency2Provider::marker(u64 frameId, LatencyMarker marker) {
    if (!m_dispatch.valid() || marker >= LatencyMarker::Count) {
        return;
    }
    m_dispatch.set_marker(m_dispatch.user, present_id(frameId), static_cast<u32>(marker));
}

u32 NvLowLatency2Provider::timings(LatencyFrameReport* out, u32 capacity) {
    if (!m_dispatch.valid() || out == nullptr || capacity == 0u) {
        return 0u;
    }
    const u32 n = std::min(capacity, m_dispatch.get_timings(m_dispatch.user, out, capacity));
    for (u32 i = 0; i < n; ++i) {
        // presentID -> frame id
        out[i].frame_id = out[i].frame_id > 0u ? out[i].frame_id - 1u : 0u;
    }
    return n;
}

// ---- VK_AMD_anti_lag ---------------------------------------------------------------------------------------------

AmdAntiLagProvider::AmdAntiLagProvider(const AntiLagDispatch& dispatch) : m_dispatch(dispatch) {}

LatencyCaps AmdAntiLagProvider::caps() const {
    LatencyCaps c{};
    c.low_latency = true;
    c.boost = false;
    c.markers = false; // only the INPUT / PRESENT stages
    c.fps_limit = true;
    c.reports = false;
    return c;
}

bool AmdAntiLagProvider::setSettings(const LatencySettings& settings) {
    if (!validSettings(settings) || !m_dispatch.valid()) {
        return false;
    }
    m_settings = settings;
    // Apply the mode now (no presentation info), like the SDK's ffxAntiLag2Update with the new mode.
    AntiLagUpdate u = makeUpdate(kAntiLagStageInput, 0u);
    u.has_presentation = false;
    m_dispatch.update(m_dispatch.user, u);
    return true;
}

AntiLagUpdate AmdAntiLagProvider::makeUpdate(u32 stage, u64 frameId) const {
    AntiLagUpdate u{};
    u.mode = m_settings.mode == LatencyMode::Off ? kAntiLagModeOff : kAntiLagModeOn;
    const f32 fps = m_settings.fps_limit;
    u.max_fps = (fps > 0.f && std::isfinite(fps)) ? static_cast<u32>(std::lround(fps)) : 0u;
    u.has_presentation = true;
    u.stage = stage;
    u.frame_index = frameId;
    return u;
}

void AmdAntiLagProvider::sleep(u64 frameId) {
    if (m_dispatch.valid()) {
        m_dispatch.update(m_dispatch.user, makeUpdate(kAntiLagStageInput, frameId));
    }
}

void AmdAntiLagProvider::marker(u64 frameId, LatencyMarker marker) {
    if (m_dispatch.valid() && marker == LatencyMarker::PresentStart) {
        m_dispatch.update(m_dispatch.user, makeUpdate(kAntiLagStagePresent, frameId));
    }
}

// ---- Selection --------------------------------------------------------------------------------------------------

LatencyBackend select_latency_backend(LatencyPreference preference, const LatencyAvailability& a, const char** reason) {
    const char* why = "ok";
    LatencyBackend chosen = LatencyBackend::None;
    switch (preference) {
    case LatencyPreference::None:
        why = "latency provider disabled";
        break;
    case LatencyPreference::NvReflexPlugin:
        chosen = a.nv_reflex_plugin ? LatencyBackend::NvReflexPlugin : LatencyBackend::None;
        why = a.nv_reflex_plugin ? "Reflex through the NVIDIA provider" : "Reflex requested but the NVIDIA provider / latency extension is unavailable";
        break;
    case LatencyPreference::NvLowLatency2:
        chosen = a.nv_low_latency2 ? LatencyBackend::NvLowLatency2 : LatencyBackend::None;
        why = a.nv_low_latency2 ? "VK_NV_low_latency2" : "VK_NV_low_latency2 requested but not enabled on the device";
        break;
    case LatencyPreference::AmdAntiLag:
        chosen = a.amd_anti_lag ? LatencyBackend::AmdAntiLag : LatencyBackend::None;
        why = a.amd_anti_lag ? "VK_AMD_anti_lag" : "VK_AMD_anti_lag requested but not enabled on the device";
        break;
    case LatencyPreference::Auto:
    default:
        if (a.nv_reflex_plugin) {
            chosen = LatencyBackend::NvReflexPlugin;
            why = "auto: Reflex through the NVIDIA provider";
        } else if (a.nv_low_latency2) {
            chosen = LatencyBackend::NvLowLatency2;
            why = "auto: VK_NV_low_latency2";
        } else if (a.amd_anti_lag) {
            chosen = LatencyBackend::AmdAntiLag;
            why = "auto: VK_AMD_anti_lag";
        } else {
            why = "auto: no latency backend available";
        }
        break;
    }
    if (reason != nullptr) {
        *reason = why;
    }
    return chosen;
}

LatencyProviderHandle create_latency_provider(LatencyPreference preference, const LatencyProviderSources& sources) {
    LatencyProviderHandle h{};
    LatencyAvailability a{};
    std::string notes;
    FuseNvLatencyApi nvApi{};
    if (sources.plugin != nullptr) {
        std::string r;
        a.nv_reflex_plugin = resolve_nv_latency_api(*sources.plugin, nvApi, r);
        notes += "reflex_plugin: " + r + "; ";
    } else {
        notes += "reflex_plugin: no NVIDIA provider loaded; ";
    }
    if (sources.device != nullptr) {
        auto ll2 = std::make_unique<VkLowLatency2Binding>();
        std::string r;
        a.nv_low_latency2 = ll2->init(*sources.device, sources.swapchain, r);
        notes += "low_latency2: " + r + "; ";
        if (a.nv_low_latency2) {
            h.lowLatency2 = std::move(ll2);
        }
        auto al = std::make_unique<VkAntiLagBinding>();
        a.amd_anti_lag = al->init(*sources.device, r);
        notes += "anti_lag: " + r + "; ";
        if (a.amd_anti_lag) {
            h.antiLag = std::move(al);
        }
    } else {
        notes += "low_latency2 / anti_lag: no device; ";
    }
    const char* why = nullptr;
    const LatencyBackend chosen = select_latency_backend(preference, a, &why);
    switch (chosen) {
    case LatencyBackend::NvReflexPlugin:
        h.provider = std::make_unique<NvReflexPluginLatencyProvider>(nvApi, sources.plugin->context());
        break;
    case LatencyBackend::NvLowLatency2:
        h.provider = std::make_unique<NvLowLatency2Provider>(h.lowLatency2->dispatch());
        break;
    case LatencyBackend::AmdAntiLag:
        h.provider = std::make_unique<AmdAntiLagProvider>(h.antiLag->dispatch());
        break;
    default:
        h.provider = std::make_unique<NullLatencyProvider>();
        break;
    }
    if (chosen != LatencyBackend::NvLowLatency2) {
        h.lowLatency2.reset();
    }
    if (chosen != LatencyBackend::AmdAntiLag) {
        h.antiLag.reset();
    }
    h.reason = std::string(why) + " [" + notes + "]";
    return h;
}

} // namespace fuse::renderer::present
