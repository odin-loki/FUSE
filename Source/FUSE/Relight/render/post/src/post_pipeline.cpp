// FUSE Relight RL-5.7: post and upscale wiring (see post_pipeline.hpp).
#include <fuse/relight/render/post/post_pipeline.hpp>

#include <fuse/relight/render/pathtrace/pt_gpu.hpp>
#include <fuse/relight/render/post/composite.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_upscaler.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fuse::relight::render::post {

namespace up = fuse::renderer::upscale;
namespace look = fuse::renderer::look;
namespace pt = fuse::relight::render::pathtrace;

namespace {

bool isTemporalRequest(const std::string& name) {
    return name == "dlss" || name == "dlss_rr" || name == "xess" || name == up::kFsr3Name ||
           name == up::kNativeTaauName;
}

const char* kindName(PostUpscaleKind k) {
    switch (k) {
    case PostUpscaleKind::Temporal: return "temporal";
    case PostUpscaleKind::Spatial: return "spatial";
    default: return "none";
    }
}

u8 toByte(float v) { return static_cast<u8>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); }

} // namespace

PostPipeline::PostPipeline() { m_registry.register_builtin_backends(); }

PostPipeline::~PostPipeline() { unbindFsr3(); }

bool PostPipeline::bindFsr3(const renderer::fsr3::Fsr3BackendBinding& binding) {
    unbindFsr3();
    m_fsr3Bound = renderer::fsr3::register_fsr3_backend(m_registry, binding);
    return m_fsr3Bound;
}

void PostPipeline::unbindFsr3() {
    if (m_fsr3Bound) {
        m_upscaler.reset();
        renderer::fsr3::unregister_fsr3_backend(m_registry);
        m_fsr3Bound = false;
        m_configured = false;
    }
}

const up::UpscalerCaps* PostPipeline::findCaps(const std::string& name, bool& global) const {
    global = false;
    if (const up::UpscalerCaps* c = m_registry.find(name)) {
        return c;
    }
    if (const up::UpscalerCaps* c = up::UpscalerRegistry::instance().find(name)) {
        global = true;
        return c;
    }
    return nullptr;
}

bool PostPipeline::select(const PostConfig& config, u32 dw, u32 dh) {
    m_sel = PostSelection{};
    m_sel.requested = config.upscaler.empty() ? std::string("none") : config.upscaler;
    m_sel.displayWidth = dw;
    m_sel.displayHeight = dh;
    m_upscaler.reset();
    float scale = std::clamp(config.resolutionScale, 0.01f, 1.f);
    if (m_sel.requested == "none") {
        m_sel.backend = "none";
        m_sel.kind = PostUpscaleKind::None;
        m_sel.renderWidth = dw;
        m_sel.renderHeight = dh;
        m_sel.mipBias = textureMipBias(dw, dw, config.nativeMipBias, config.upscalingMipBias);
        return true;
    }
    bool global = false;
    const up::UpscalerCaps* caps = findCaps(m_sel.requested, global);
    std::string name = m_sel.requested;
    if (caps == nullptr || caps->stub) {
        const std::string fb = isTemporalRequest(m_sel.requested) || caps == nullptr ? up::kNativeTaauName : up::kFsr1Name;
        if (caps != nullptr) {
            m_sel.reason = m_sel.requested + ": backend is a stub";
        } else if (m_sel.requested == up::kFsr3Name) {
            m_sel.reason = "fsr3: not registered (no Vulkan device bound, or FUSE_UPSCALER_FSR3 off)";
        } else if (m_sel.requested == "dlss" || m_sel.requested == "dlss_rr") {
            m_sel.reason = m_sel.requested + ": not registered (plugins/nvidia provider not loaded)";
        } else if (m_sel.requested == "xess") {
            m_sel.reason = "xess: not registered (plugins/intel_xess provider not loaded)";
        } else {
            m_sel.reason = m_sel.requested + ": unknown upscaler";
        }
        m_sel.reason += "; using " + fb;
        m_sel.fallback = true;
        name = fb;
        caps = findCaps(name, global);
        if (caps == nullptr) {
            m_error = "no upscaler backend available";
            return false;
        }
    }
    if (caps->kind == up::UpscalerKind::Sharpen) {
        scale = 1.f;
    }
    u32 rw = dw, rh = dh;
    renderExtentForScale(dw, dh, scale, rw, rh);
    const float ratio = float(dw) / float(rw);
    if (!caps->supports_ratio(ratio) || !caps->supports_ratio(float(dh) / float(rh))) {
        const float s = 1.f / std::clamp(ratio, caps->min_ratio, caps->max_ratio);
        renderExtentForScale(dw, dh, s, rw, rh);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%sratio %.3f outside [%.2f, %.2f]: clamped", m_sel.reason.empty() ? "" : "; ",
                      double(ratio), double(caps->min_ratio), double(caps->max_ratio));
        m_sel.reason += buf;
    }
    m_upscaler = global ? up::UpscalerRegistry::instance().create(name) : m_registry.create(name);
    if (m_upscaler == nullptr) {
        m_error = "upscaler backend could not be created";
        return false;
    }
    m_sel.backend = name;
    m_sel.kind = caps->kind == up::UpscalerKind::Temporal ? PostUpscaleKind::Temporal : PostUpscaleKind::Spatial;
    m_sel.renderWidth = rw;
    m_sel.renderHeight = rh;
    m_sel.mipBias = textureMipBias(rw, dw, config.nativeMipBias, config.upscalingMipBias);
    return true;
}

bool PostPipeline::configure(const PostConfig& config, u32 dw, u32 dh) {
    m_configured = false;
    m_error = "";
    if (dw == 0u || dh == 0u) {
        m_error = "empty output";
        return false;
    }
    m_config = config;
    if (!select(config, dw, dh)) {
        return false;
    }
    const u32 rw = m_sel.renderWidth, rh = m_sel.renderHeight;
    const std::size_t rn = std::size_t(rw) * rh, dn = std::size_t(dw) * dh;
    const bool spatial = m_sel.kind == PostUpscaleKind::Spatial;
    // The Look (and the local tone map) runs at the display resolution after a temporal upscaler / native, at the
    // render resolution before a spatial one.
    const u32 lw = spatial ? rw : dw, lh = spatial ? rh : dh;
    m_color.assign(rn, math::Vec3{});
    m_depth.assign(rn, 0.f);
    m_motion.assign(rn, math::Vec2{});
    m_display.assign(m_sel.kind == PostUpscaleKind::Temporal ? dn : 0u, math::Vec3{});
    m_ltm.assign(std::size_t(lw) * lh, math::Vec3{});
    m_encoded.assign(std::size_t(lw) * lh, math::Vec3{});
    m_rgbaIn.assign(spatial ? rn : 0u, math::Vec4{});
    m_rgbaOut.assign(spatial ? dn : 0u, math::Vec4{});
    if (config.toneMapping == ToneMappingMode::Local) {
        if (!m_localToneMap.init(lw, lh, config.localToneMap)) {
            m_error = "local tone mapper";
            return false;
        }
    } else {
        m_localToneMap = LocalToneMapper{};
    }
    look::LookChainConfig lc;
    lc.width = lw;
    lc.height = lh;
    lc.encoding = look::LookOutputEncoding::Srgb;
    lc.backend = kernel::Backend::CpuReference;
    if (!m_look.init(lc)) {
        m_error = "look chain";
        return false;
    }
    m_graph.clear();
    m_graph.push(look::LookEffect::Exposure);
    m_graph.push(look::LookEffect::ToneMap);
    m_graph.push(look::LookEffect::OutputTransform);
    if (!m_graph.validate().ok() || !validateLocalToneMapPlacement(m_graph)) {
        m_error = "look graph";
        return false;
    }
    m_resolved = look::look_resolve(look::look_default_params());
    m_resolved.exposure.auto_enabled = false;
    if (!m_lut.valid()) {
        m_lut = look::Lut3D::identity(look::kLutMinSize);
    }
    m_stats = PostFrameStats{};
    m_resetHistory = true;
    m_configured = true;
    return true;
}

bool PostPipeline::runLook(const math::Vec3* hdr, u32 width, u32 height, math::Vec3* encoded) {
    const math::Vec3* src = hdr;
    if (m_config.toneMapping == ToneMappingMode::Local && m_localToneMap.ready()) {
        m_localToneMap.process(hdr, m_ltm.data());
        src = m_ltm.data();
    }
    (void)width;
    (void)height;
    look::LookChainInput in;
    in.hdr = src;
    in.frame_seed = 0;
    return m_look.process(in, m_graph, m_resolved, m_lut, encoded);
}

void PostPipeline::pack(const math::Vec3* encoded, const PostTarget& t) {
    const std::size_t n = std::size_t(t.width) * t.height;
    for (std::size_t i = 0; i < n; ++i) {
        float c[3] = {encoded[i].x, encoded[i].y, encoded[i].z};
        for (float& v : c) {
            if (!std::isfinite(v)) {
                ++m_stats.nonFinite;
                v = 0.f;
            }
        }
        t.pixels[i * 4u + 0u] = toByte(t.bgra ? c[2] : c[0]);
        t.pixels[i * 4u + 1u] = toByte(c[1]);
        t.pixels[i * 4u + 2u] = toByte(t.bgra ? c[0] : c[2]);
        t.pixels[i * 4u + 3u] = 255u;
    }
}

void PostPipeline::packRgba(const math::Vec4* encoded, const PostTarget& t) {
    const std::size_t n = std::size_t(t.width) * t.height;
    for (std::size_t i = 0; i < n; ++i) {
        float c[3] = {encoded[i].x, encoded[i].y, encoded[i].z};
        for (float& v : c) {
            if (!std::isfinite(v)) {
                ++m_stats.nonFinite;
                v = 0.f;
            }
        }
        t.pixels[i * 4u + 0u] = toByte(t.bgra ? c[2] : c[0]);
        t.pixels[i * 4u + 1u] = toByte(c[1]);
        t.pixels[i * 4u + 2u] = toByte(t.bgra ? c[0] : c[2]);
        t.pixels[i * 4u + 3u] = 255u;
    }
}

bool PostPipeline::processHdr(const math::Vec3* color, const float* depth, const math::Vec2* motion,
                              const PostTarget& target) {
    if (!m_configured || color == nullptr || target.pixels == nullptr || target.width != m_sel.displayWidth ||
        target.height != m_sel.displayHeight) {
        m_error = m_configured ? "post target / input mismatch" : "post pipeline not configured";
        return false;
    }
    const u32 rw = m_sel.renderWidth, rh = m_sel.renderHeight, dw = m_sel.displayWidth, dh = m_sel.displayHeight;
    const std::size_t rn = std::size_t(rw) * rh;
    m_stats.nonFinite = 0;
    m_stats.status = up::UpscaleStatus::Ok;
    ++m_stats.frames;
    up::UpscaleDispatch dispatch;
    dispatch.backend = kernel::Backend::CpuReference; // deterministic and allocation-free on the calling thread
    switch (m_sel.kind) {
    case PostUpscaleKind::None:
        m_stats.lookOk = runLook(color, dw, dh, m_encoded.data());
        pack(m_encoded.data(), target);
        break;
    case PostUpscaleKind::Temporal: {
        if (depth == nullptr) {
            std::fill(m_depth.begin(), m_depth.end(), 0.f);
            depth = m_depth.data();
        }
        if (motion == nullptr) {
            std::fill(m_motion.begin(), m_motion.end(), math::Vec2{});
            motion = m_motion.data();
        }
        up::UpscaleInputs in;
        in.resolution = up::make_resolution({rw, rh}, {dw, dh});
        in.color = color;
        in.depth = depth;
        in.motion = motion;
        in.jitter_px = math::Vec2(0.f, 0.f);
        in.jitter_phase = 0;
        in.jitter_phase_count = 1;
        in.exposure = 1.f;
        in.mip_bias = m_sel.mipBias;
        in.frame_index = m_stats.frames - 1u;
        in.reset_history = m_resetHistory;
        in.camera.aspect = float(rw) / float(rh);
        in.previous_camera = in.camera;
        auto* t = static_cast<up::ITemporalUpscaler*>(m_upscaler.get());
        up::TemporalUpscaleOutputs out;
        out.color = up::RgbImage{m_display.data(), dw, dh};
        m_stats.status = t->evaluate(dispatch, in, out);
        m_resetHistory = false;
        if (m_stats.status != up::UpscaleStatus::Ok) {
            m_error = up::upscale_status_name(m_stats.status);
            return false;
        }
        for (math::Vec3& v : m_display) {
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
                ++m_stats.nonFinite;
                v = math::Vec3{};
            }
        }
        m_stats.lookOk = runLook(m_display.data(), dw, dh, m_encoded.data());
        pack(m_encoded.data(), target);
        break;
    }
    case PostUpscaleKind::Spatial: {
        m_stats.lookOk = runLook(color, rw, rh, m_encoded.data());
        for (std::size_t i = 0; i < rn; ++i) {
            m_rgbaIn[i] = math::Vec4(m_encoded[i], 1.f);
        }
        auto* s = static_cast<up::ISpatialUpscaler*>(m_upscaler.get());
        up::SpatialUpscaleInputs in;
        in.color = up::ConstRgbaImage{m_rgbaIn.data(), rw, rh};
        in.sharpness = m_config.sharpness;
        up::UpscaleOutputs out;
        out.color = up::RgbaImage{m_rgbaOut.data(), dw, dh};
        m_stats.status = s->evaluate(dispatch, in, out);
        if (m_stats.status != up::UpscaleStatus::Ok) {
            m_error = up::upscale_status_name(m_stats.status);
            return false;
        }
        packRgba(m_rgbaOut.data(), target);
        break;
    }
    }
    if (!m_stats.lookOk) {
        m_error = "look chain failed";
        return false;
    }
    return true;
}

bool PostPipeline::process(const PtPostInput& input, const PostTarget& target) {
    if (!m_configured || input.outputs == nullptr || input.renderWidth != m_sel.renderWidth ||
        input.renderHeight != m_sel.renderHeight) {
        m_error = m_configured ? "path tracer frame size differs from the post render extent"
                               : "post pipeline not configured";
        return false;
    }
    const auto* base = static_cast<const u8*>(input.outputs);
    auto section = [&](u32 s) { return reinterpret_cast<const float*>(base + u64(s) * input.stride); };
    const std::size_t n = std::size_t(input.renderWidth) * input.renderHeight;
    m_stats.accumulatedInput = input.accumulated && m_config.useAccumulation;
    m_stats.compositeMaxRel = -1.0;
    if (m_stats.accumulatedInput) {
        const float* acc = section(pt::kPtOutAccum);
        for (std::size_t i = 0; i < n; ++i) {
            const float inv = 1.f / std::max(acc[i * 4u + 3u], 1.f);
            m_color[i] = math::Vec3(acc[i * 4u] * inv, acc[i * 4u + 1u] * inv, acc[i * 4u + 2u] * inv);
        }
    } else {
        CompositeInputs ci;
        ci.emissive = section(pt::kPtOutEmissive);
        ci.diffuse = section(pt::kPtOutDiffuse);
        ci.specular = section(pt::kPtOutSpecular);
        ci.albedoD = section(pt::kPtOutAlbedoD);
        ci.albedoS = section(pt::kPtOutAlbedoS);
        ci.denoisedDiffuse = input.denoisedDiffuse;
        ci.denoisedSpecular = input.denoisedSpecular;
        ci.count = n;
        compositeRadiance(ci, m_color.data());
        const float* rad = section(pt::kPtOutRadiance);
        double worst = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const float c[3] = {m_color[i].x, m_color[i].y, m_color[i].z};
            for (u32 k = 0; k < 3u; ++k) {
                const double r = rad[i * 4u + k];
                worst = std::max(worst, std::fabs(double(c[k]) - r) / std::max(std::fabs(r), 1e-3));
            }
        }
        m_stats.compositeMaxRel = worst;
    }
    if (input.exposure != 1.f) {
        for (math::Vec3& c : m_color) {
            c = c * input.exposure;
        }
    }
    const float* depth = section(pt::kPtOutDepth);
    const float* motion = section(pt::kPtOutMotion);
    for (std::size_t i = 0; i < n; ++i) {
        m_depth[i] = depth[i];
        m_motion[i] = math::Vec2(-motion[i * 2u], -motion[i * 2u + 1u]); // previous - current -> current - previous
    }
    return processHdr(m_color.data(), m_depth.data(), m_motion.data(), target);
}

const char* PostPipeline::recordJson() const {
    std::snprintf(m_json, sizeof(m_json),
                  ",\"post\":{\"requested\":\"%s\",\"upscaler\":\"%s\",\"kind\":\"%s\",\"fallback\":%s,\"reason\":\"%s\","
                  "\"render\":[%u,%u],\"display\":[%u,%u],\"mip_bias\":%.6g,\"tonemapping\":\"%s\",\"ltm_levels\":%u,"
                  "\"status\":\"%s\",\"nonfinite\":%u,\"composite_max_rel\":%.3g,\"accumulated_input\":%s,\"frames\":%u,\"error\":\"%s\"}",
                  m_sel.requested.c_str(), m_sel.backend.c_str(), kindName(m_sel.kind), m_sel.fallback ? "true" : "false",
                  m_sel.reason.c_str(), m_sel.renderWidth, m_sel.renderHeight, m_sel.displayWidth, m_sel.displayHeight,
                  double(m_sel.mipBias), m_config.toneMapping == ToneMappingMode::Local ? "local" : "global",
                  m_localToneMap.levels(), up::upscale_status_name(m_stats.status), m_stats.nonFinite, m_stats.compositeMaxRel,
                  m_stats.accumulatedInput ? "true" : "false", m_stats.frames, m_error);
    return m_json;
}

} // namespace fuse::relight::render::post
