// Denoiser abstraction + registry (WP-6.4b): denoiser.hpp.
#include <fuse/renderer/denoise/denoiser.hpp>

#include <algorithm>

namespace fuse::renderer::denoise {

const char* denoiser_method_name(DenoiserMethod m) {
    switch (m) {
        case DenoiserMethod::Svgf: return "svgf";
        case DenoiserMethod::ASvgf: return "a-svgf";
        case DenoiserMethod::Sigma: return "sigma";
        case DenoiserMethod::Reblur: return "reblur";
        case DenoiserMethod::Relax: return "relax";
    }
    return "unknown";
}

bool denoiser_method_accepts(DenoiserMethod method, DenoiseSignal signal) {
    switch (method) {
        case DenoiserMethod::Svgf:
        case DenoiserMethod::ASvgf: return true;
        case DenoiserMethod::Sigma: return signal == DenoiseSignal::Shadow;
        case DenoiserMethod::Reblur:
        case DenoiserMethod::Relax: return signal == DenoiseSignal::Reflection || signal == DenoiseSignal::Gi;
    }
    return false;
}

// ---- SVGF adapter ------------------------------------------------------------------------------------------

const DenoiserCaps& svgf_denoiser_caps() {
    static const DenoiserCaps kCaps = [] {
        DenoiserCaps c{};
        c.name = kSvgfDenoiserName;
        c.display_name = "FUSE SVGF / A-SVGF";
        c.license = "MIT";
        c.methods = denoiser_method_bit(DenoiserMethod::Svgf) | denoiser_method_bit(DenoiserMethod::ASvgf);
        c.signals = kAllDenoiseSignals;
        c.gpu_only = true;
        c.in_tree = true;
        return c;
    }();
    return kCaps;
}

SvgfDenoiserAdapter::SvgfDenoiserAdapter() { m_svgf.setSettings(to_svgf_settings(m_settings)); }

const DenoiserCaps& SvgfDenoiserAdapter::caps() const { return svgf_denoiser_caps(); }

SvgfSettings SvgfDenoiserAdapter::to_svgf_settings(const DenoiserSettings& settings) {
    SvgfSettings s = svgf_preset(settings.signal);
    s.gradients = settings.method == DenoiserMethod::ASvgf;
    s.maxHistory = static_cast<f32>(std::max(1u, settings.max_history_frames));
    if (settings.disocclusion_depth > 0.f) {
        s.reprojDepth = settings.disocclusion_depth;
    }
    return s;
}

bool SvgfDenoiserAdapter::configure(const DenoiserSettings& settings) {
    if (!caps().supports(settings.method) || !caps().supports(settings.signal)) {
        return false;
    }
    m_settings = settings;
    m_svgf.setSettings(to_svgf_settings(settings));
    return true;
}

namespace {

std::unique_ptr<IDenoiser> make_svgf(const DenoiserCreateInfo& info) {
    auto d = std::make_unique<SvgfDenoiserAdapter>();
    if (!d->init(info.svgf)) {
        return nullptr;
    }
    return d;
}

bool entry_serves(const DenoiserCaps& c, DenoiserMethod m, const DenoiserRequirements& req) {
    if (!c.supports(m) || !c.supports(req.signal) || !denoiser_method_accepts(m, req.signal)) {
        return false;
    }
    if (c.needs_native_frame && !req.have_native_frame) {
        return false;
    }
    if (c.needs_hit_distance(m) && !req.have_hit_distance) {
        return false;
    }
    if (m == DenoiserMethod::ASvgf && !req.have_gradients) {
        return false;
    }
    return true;
}

} // namespace

// ---- registry ------------------------------------------------------------------------------------------------

DenoiserRegistry& DenoiserRegistry::instance() {
    static DenoiserRegistry registry = [] {
        DenoiserRegistry r;
        r.register_builtin_denoisers();
        return r;
    }();
    return registry;
}

void DenoiserRegistry::register_builtin_denoisers() { register_backend(svgf_denoiser_caps(), &make_svgf); }

bool DenoiserRegistry::register_backend(const DenoiserCaps& caps, DenoiserFactory factory) {
    if (caps.name == nullptr || *caps.name == '\0' || factory == nullptr || find(caps.name) != nullptr) {
        return false;
    }
    m_entries.push_back({caps, factory});
    return true;
}

bool DenoiserRegistry::unregister_backend(std::string_view name) {
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [&](const DenoiserBackendEntry& e) { return name == e.caps.name; });
    if (it == m_entries.end()) {
        return false;
    }
    m_entries.erase(it);
    return true;
}

const DenoiserCaps* DenoiserRegistry::find(std::string_view name) const {
    for (const DenoiserBackendEntry& e : m_entries) {
        if (name == e.caps.name) {
            return &e.caps;
        }
    }
    return nullptr;
}

std::unique_ptr<IDenoiser> DenoiserRegistry::create(std::string_view name, const DenoiserCreateInfo& info) const {
    for (const DenoiserBackendEntry& e : m_entries) {
        if (name == e.caps.name) {
            return e.factory(info);
        }
    }
    return nullptr;
}

DenoiserSelection DenoiserRegistry::select(const DenoiserRequirements& req) const {
    DenoiserSelection sel;
    for (const DenoiserBackendEntry& e : m_entries) {
        if (entry_serves(e.caps, req.method, req)) {
            sel.caps = &e.caps;
            sel.method = req.method;
            sel.reason = "preferred method available";
            return sel;
        }
    }
    if (!req.allow_fallback) {
        sel.reason = "no backend serves the preferred method and fallback is disabled";
        return sel;
    }
    const DenoiserMethod fb =
        (req.method == DenoiserMethod::ASvgf && req.have_gradients) ? DenoiserMethod::ASvgf : DenoiserMethod::Svgf;
    for (const DenoiserBackendEntry& e : m_entries) {
        if (e.caps.in_tree && entry_serves(e.caps, fb, req)) {
            sel.caps = &e.caps;
            sel.method = fb;
            sel.fallback = true;
            sel.reason = denoiser_method_accepts(req.method, req.signal)
                             ? "preferred method unavailable (plugin not loaded or inputs missing): in-tree fallback"
                             : "preferred method cannot denoise this signal: in-tree fallback";
            return sel;
        }
    }
    sel.reason = "no backend registered for this signal";
    return sel;
}

} // namespace fuse::renderer::denoise
