// Upscaler abstraction: quality modes, jitter providers, input validation and the backend registry
// (upscaler.hpp). The built-in backends live in upscaler_backends.cpp.

#include <fuse/renderer/upscale/upscaler.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::upscale {

// Defined in upscaler_backends.cpp.
void register_builtin_upscalers(UpscalerRegistry& registry);

f32 quality_mode_ratio(QualityMode mode) { return fuse::renderer::upscaleRatioForMode(mode); }

const char* quality_mode_name(QualityMode mode) { return fuse::renderer::upscaleQualityModeLabel(mode); }

Extent2D render_extent(Extent2D display, QualityMode mode) {
    const fuse::renderer::UpscaleResolution r =
        fuse::renderer::makeUpscaleResolution(display.width, display.height, mode);
    return {r.render_width, r.render_height};
}

fuse::renderer::UpscaleResolution make_resolution(Extent2D render, Extent2D display) {
    fuse::renderer::UpscaleResolution r{};
    r.render_width = render.width;
    r.render_height = render.height;
    r.display_width = display.width;
    r.display_height = display.height;
    return r;
}

math::Vec2 upscale_ratio(Extent2D render, Extent2D display) {
    if (!render.valid()) {
        return math::Vec2(0.f, 0.f);
    }
    return math::Vec2(static_cast<f32>(display.width) / static_cast<f32>(render.width),
                      static_cast<f32>(display.height) / static_cast<f32>(render.height));
}

f32 mip_lod_bias(Extent2D render, Extent2D display, bool temporal) {
    if (!render.valid() || !display.valid()) {
        return 0.f;
    }
    return fuse::renderer::upscaleTextureMipBias(make_resolution(render, display), temporal ? -1.f : 0.f);
}

const char* upscale_status_name(UpscaleStatus status) {
    switch (status) {
    case UpscaleStatus::Ok:
        return "Ok";
    case UpscaleStatus::InvalidInputs:
        return "InvalidInputs";
    case UpscaleStatus::UnsupportedRatio:
        return "UnsupportedRatio";
    case UpscaleStatus::BackendUnavailable:
        return "BackendUnavailable";
    case UpscaleStatus::LaunchFailed:
        return "LaunchFailed";
    }
    return "Unknown";
}

const char* history_reset_reason_name(HistoryResetReason reason) {
    switch (reason) {
    case HistoryResetReason::None:
        return "None";
    case HistoryResetReason::CameraCut:
        return "CameraCut";
    case HistoryResetReason::Teleport:
        return "Teleport";
    case HistoryResetReason::ResolutionChange:
        return "ResolutionChange";
    case HistoryResetReason::QualityModeChange:
        return "QualityModeChange";
    case HistoryResetReason::Explicit:
        return "Explicit";
    }
    return "Unknown";
}

// ---- Jitter -------------------------------------------------------------------------------------------

math::Vec2 IJitterProvider::offset_ndc(u32 frame_index, Extent2D render, Extent2D display) const {
    return fuse::renderer::upscaleJitterNdc(offset_px(frame_index, render, display), render.width, render.height);
}

u32 HaltonJitterProvider::phase_count(Extent2D render, Extent2D display) const {
    if (!render.valid() || !display.valid()) {
        return kBasePhaseCount;
    }
    return fuse::renderer::upscaleJitterPhaseCount(make_resolution(render, display));
}

math::Vec2 HaltonJitterProvider::offset_px(u32 frame_index, Extent2D render, Extent2D display) const {
    // ffxFsr3GetJitterOffset: Halton(phase + 1) - 0.5 on bases 2 and 3.
    return fuse::renderer::upscaleJitterOffset(frame_index, phase_count(render, display));
}

// ---- Validation ---------------------------------------------------------------------------------------

bool IUpscaler::supports(Extent2D render, Extent2D display) const {
    if (!render.valid() || !display.valid()) {
        return false;
    }
    const math::Vec2 ratio = upscale_ratio(render, display);
    return caps().supports_ratio(ratio.x) && caps().supports_ratio(ratio.y);
}

UpscaleStatus ISpatialUpscaler::validate(const SpatialUpscaleInputs& inputs, const UpscaleOutputs& outputs) const {
    if (!inputs.color.valid() || !outputs.color.valid()) {
        return UpscaleStatus::InvalidInputs;
    }
    return supports(inputs.color.extent(), outputs.color.extent()) ? UpscaleStatus::Ok
                                                                   : UpscaleStatus::UnsupportedRatio;
}

UpscaleStatus ITemporalUpscaler::validate(const UpscaleInputs& inputs, const TemporalUpscaleOutputs& outputs) const {
    if (fuse::renderer::validateUpscaleInputs(inputs) != fuse::renderer::UpscaleInputsError::None ||
        !outputs.color.valid()) {
        return UpscaleStatus::InvalidInputs;
    }
    const UpscalerCaps& c = caps();
    if ((c.needs_reactive_mask && inputs.reactive == nullptr) || (c.needs_exposure && !(inputs.exposure > 0.f))) {
        return UpscaleStatus::InvalidInputs;
    }
    const fuse::renderer::UpscaleResolution& r = inputs.resolution;
    if (outputs.color.width != r.display_width || outputs.color.height != r.display_height) {
        return UpscaleStatus::InvalidInputs;
    }
    return supports({r.render_width, r.render_height}, {r.display_width, r.display_height})
               ? UpscaleStatus::Ok
               : UpscaleStatus::UnsupportedRatio;
}

// ---- Registry -----------------------------------------------------------------------------------------

bool caps_satisfy(const UpscalerCaps& caps, const UpscalerRequirements& req) {
    if (!caps.supports_mode(req.mode) || !caps.supports_api(req.api)) {
        return false;
    }
    if (caps.stub && !req.allow_stub) {
        return false;
    }
    if (caps.temporal ? !req.allow_temporal : !req.allow_spatial) {
        return false;
    }
    if (!caps.supports_ratio(quality_mode_ratio(req.mode))) {
        return false;
    }
    if ((caps.needs_depth && !req.have_depth) || (caps.needs_motion_vectors && !req.have_motion_vectors) ||
        (caps.needs_reactive_mask && !req.have_reactive_mask) || (caps.needs_exposure && !req.have_exposure)) {
        return false;
    }
    return true;
}

UpscalerRegistry& UpscalerRegistry::instance() {
    static UpscalerRegistry registry = [] {
        UpscalerRegistry r;
        r.register_builtin_backends();
        return r;
    }();
    return registry;
}

void UpscalerRegistry::register_builtin_backends() { register_builtin_upscalers(*this); }

bool UpscalerRegistry::register_backend(const UpscalerCaps& caps, UpscalerFactory factory) {
    if (caps.name == nullptr || caps.name[0] == '\0' || factory == nullptr || find(caps.name) != nullptr) {
        return false;
    }
    m_entries.push_back(UpscalerBackendEntry{caps, factory});
    return true;
}

bool UpscalerRegistry::unregister_backend(std::string_view name) {
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [name](const UpscalerBackendEntry& e) { return name == e.caps.name; });
    if (it == m_entries.end()) {
        return false;
    }
    m_entries.erase(it);
    return true;
}

const UpscalerCaps* UpscalerRegistry::find(std::string_view name) const {
    for (const UpscalerBackendEntry& e : m_entries) {
        if (name == e.caps.name) {
            return &e.caps;
        }
    }
    return nullptr;
}

std::unique_ptr<IUpscaler> UpscalerRegistry::create(std::string_view name) const {
    for (const UpscalerBackendEntry& e : m_entries) {
        if (name == e.caps.name) {
            return e.factory();
        }
    }
    return nullptr;
}

const UpscalerCaps* UpscalerRegistry::select(const UpscalerRequirements& req) const {
    for (const UpscalerBackendEntry& e : m_entries) {
        if (caps_satisfy(e.caps, req)) {
            return &e.caps;
        }
    }
    return nullptr;
}

} // namespace fuse::renderer::upscale
