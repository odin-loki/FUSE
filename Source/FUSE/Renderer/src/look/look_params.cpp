#include <fuse/renderer/look/effect_graph.hpp>
#include <fuse/renderer/look/look_params.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace fuse::renderer::look {

namespace {

constexpr LookEffectInfo kEffects[kLookEffectCount] = {
    {LookEffect::AmbientOcclusion, "ambientOcclusion", LookStage::PreUpscale, LookDomain::SceneHdr,
     LookDomain::SceneHdr, true},
    {LookEffect::DepthOfField, "depthOfField", LookStage::PostUpscaleHdr, LookDomain::SceneHdr, LookDomain::SceneHdr,
     false},
    {LookEffect::MotionBlur, "motionBlur", LookStage::PostUpscaleHdr, LookDomain::SceneHdr, LookDomain::SceneHdr,
     false},
    {LookEffect::Bloom, "bloom", LookStage::PostUpscaleHdr, LookDomain::SceneHdr, LookDomain::SceneHdr, false},
    {LookEffect::LensDirt, "lensDirt", LookStage::PostUpscaleHdr, LookDomain::SceneHdr, LookDomain::SceneHdr, false},
    {LookEffect::LensFlare, "lensFlare", LookStage::PostUpscaleHdr, LookDomain::SceneHdr, LookDomain::SceneHdr,
     false},
    {LookEffect::Exposure, "exposure", LookStage::PostUpscaleHdr, LookDomain::SceneHdr, LookDomain::SceneHdr, false},
    {LookEffect::ToneMap, "toneMap", LookStage::PostUpscaleHdr, LookDomain::SceneHdr, LookDomain::DisplayLinear,
     false},
    {LookEffect::ColorGrade, "colorGrade", LookStage::Display, LookDomain::DisplayLinear, LookDomain::DisplayLinear,
     false},
    {LookEffect::Sharpen, "sharpen", LookStage::Display, LookDomain::DisplayLinear, LookDomain::DisplayLinear, false},
    {LookEffect::ChromaticAberration, "chromaticAberration", LookStage::Display, LookDomain::Any, LookDomain::Any,
     false},
    {LookEffect::Vignette, "vignette", LookStage::Display, LookDomain::Any, LookDomain::Any, false},
    {LookEffect::FilmGrain, "filmGrain", LookStage::Display, LookDomain::DisplayLinear, LookDomain::DisplayLinear,
     false},
    {LookEffect::OutputTransform, "outputTransform", LookStage::Output, LookDomain::DisplayLinear,
     LookDomain::Encoded, false},
};

constexpr LookParamInfo kParams[kLookParamCount] = {
#define FUSE_LOOK_PARAM_INFO(id, effect, key, type, d0, d1, d2, mn, mx, blend)                                  \
    {LookParam::id,                                                                                              \
     LookEffect::effect,                                                                                         \
     key,                                                                                                        \
     LookParamType::type,                                                                                        \
     {static_cast<f32>(d0), static_cast<f32>(d1), static_cast<f32>(d2)},                                         \
     static_cast<f32>(mn),                                                                                       \
     static_cast<f32>(mx),                                                                                       \
     LookBlendMode::blend},
    FUSE_LOOK_PARAM_LIST(FUSE_LOOK_PARAM_INFO)
#undef FUSE_LOOK_PARAM_INFO
};

LookParamBlock makeDefaults() {
    LookParamBlock block{};
    for (u32 i = 0; i < kLookParamCount; ++i) {
        const LookParamInfo& info = kParams[i];
        for (u32 c = 0; c < kLookSlotsPerParam; ++c) {
            block.slots[i * kLookSlotsPerParam + c] = c < look_param_components(info.type) ? info.def[c] : 0.f;
        }
    }
    return block;
}

} // namespace

const LookEffectInfo& look_effect_info(LookEffect effect) {
    const u32 i = static_cast<u32>(effect);
    return kEffects[i < kLookEffectCount ? i : 0u];
}

const char* look_effect_key(LookEffect effect) {
    return look_effect_info(effect).key;
}

bool look_effect_from_key(std::string_view key, LookEffect& out) {
    for (const LookEffectInfo& info : kEffects) {
        if (key == info.key) {
            out = info.effect;
            return true;
        }
    }
    return false;
}

const LookParamInfo& look_param_info(LookParam param) {
    const u32 i = static_cast<u32>(param);
    return kParams[i < kLookParamCount ? i : 0u];
}

bool look_param_find(LookEffect effect, std::string_view key, LookParam& out) {
    for (const LookParamInfo& info : kParams) {
        if (info.effect == effect && key == info.key) {
            out = info.id;
            return true;
        }
    }
    return false;
}

u32 look_param_enum_count(LookParam param) {
    return param == LookParam::TmOperator ? static_cast<u32>(kLookToneMapOperatorNames.size()) : 0u;
}

const char* look_param_enum_name(LookParam param, u32 value) {
    if (param == LookParam::TmOperator && value < kLookToneMapOperatorNames.size()) {
        return kLookToneMapOperatorNames[value];
    }
    return nullptr;
}

bool look_param_enum_from_name(LookParam param, std::string_view name, u32& out) {
    const u32 n = look_param_enum_count(param);
    for (u32 i = 0; i < n; ++i) {
        if (name == look_param_enum_name(param, i)) {
            out = i;
            return true;
        }
    }
    return false;
}

const LookParamBlock& look_default_params() {
    static const LookParamBlock defaults = makeDefaults();
    return defaults;
}

u32 LookParamMask::count() const {
    u32 n = 0;
    for (u64 w : bits) {
        n += static_cast<u32>(std::popcount(w));
    }
    return n;
}

u32 look_clamp_params(LookParamBlock& block) {
    u32 changed = 0;
    for (u32 i = 0; i < kLookParamCount; ++i) {
        const LookParamInfo& info = kParams[i];
        const u32 comps = look_param_components(info.type);
        for (u32 c = 0; c < comps; ++c) {
            f32& slot = block.slots[i * kLookSlotsPerParam + c];
            f32 v = slot;
            if (!std::isfinite(v)) {
                v = info.def[c];
            }
            v = std::clamp(v, info.min, info.max);
            if (info.type == LookParamType::Int || info.type == LookParamType::Enum) {
                v = std::nearbyint(v);
            } else if (info.type == LookParamType::Bool) {
                v = v >= 0.5f ? 1.f : 0.f;
            }
            if (v != slot) {
                slot = v;
                ++changed;
            }
        }
    }
    return changed;
}

LookResolved look_resolve(const LookParamBlock& b) {
    const auto on = [&b](LookParam p) { return b.get(p) >= 0.5f; };
    const auto u = [&b](LookParam p) { return static_cast<u32>(std::max(std::nearbyint(b.get(p)), 0.f)); };
    LookResolved r{};
    r.ambient_occlusion = {on(LookParam::AoEnabled), b.get(LookParam::AoSsaoStrength),
                           b.get(LookParam::AoSsilStrength)};
    r.dof.enabled = on(LookParam::DofEnabled);
    r.dof.near_blur = on(LookParam::DofNearBlur);
    r.dof.focus_distance_m = b.get(LookParam::DofFocusDistanceM);
    r.dof.focal_length_mm = b.get(LookParam::DofFocalLengthMm);
    r.dof.f_stop = b.get(LookParam::DofFStop);
    r.dof.max_coc_radius_px = b.get(LookParam::DofMaxCocRadiusPx);
    r.dof.bokeh_blades = u(LookParam::DofBokehBlades);
    r.motion_blur.enabled = on(LookParam::MbEnabled);
    r.motion_blur.shutter_angle = b.get(LookParam::MbShutterAngle);
    r.motion_blur.max_blur_px = b.get(LookParam::MbMaxBlurPx);
    r.motion_blur.max_samples = u(LookParam::MbMaxSamples);
    r.bloom.enabled = on(LookParam::BloomEnabled);
    r.bloom.threshold = b.get(LookParam::BloomThreshold);
    r.bloom.knee = b.get(LookParam::BloomKnee);
    r.bloom.intensity = b.get(LookParam::BloomIntensity);
    r.bloom.scatter = b.get(LookParam::BloomScatter);
    r.bloom.tint = b.color(LookParam::BloomTint);
    r.lens_dirt.enabled = on(LookParam::DirtEnabled);
    r.lens_dirt.intensity = b.get(LookParam::DirtIntensity);
    r.lens_dirt.tint = b.color(LookParam::DirtTint);
    r.lens_flare.enabled = on(LookParam::FlareEnabled);
    r.lens_flare.intensity = b.get(LookParam::FlareIntensity);
    r.lens_flare.threshold = b.get(LookParam::FlareThreshold);
    r.lens_flare.ghost_count = u(LookParam::FlareGhostCount);
    r.lens_flare.ghost_spacing = b.get(LookParam::FlareGhostSpacing);
    r.lens_flare.halo_radius = b.get(LookParam::FlareHaloRadius);
    r.lens_flare.halo_thickness = b.get(LookParam::FlareHaloThickness);
    r.lens_flare.halo_intensity = b.get(LookParam::FlareHaloIntensity);
    r.lens_flare.chromatic_shift = b.get(LookParam::FlareChromaticShift);
    r.lens_flare.tint = b.color(LookParam::FlareTint);
    r.exposure.bias_ev = b.get(LookParam::ExpBiasEv);
    r.exposure.auto_enabled = on(LookParam::ExpAutoEnabled);
    r.exposure.min_ev = b.get(LookParam::ExpMinEv);
    r.exposure.max_ev = b.get(LookParam::ExpMaxEv);
    r.exposure.adapt_speed_up = b.get(LookParam::ExpAdaptSpeedUp);
    r.exposure.adapt_speed_down = b.get(LookParam::ExpAdaptSpeedDown);
    r.tonemap.op = static_cast<LookToneMapOperator>(std::min(u(LookParam::TmOperator), 3u));
    r.tonemap.calibrate_mid_grey = on(LookParam::TmCalibrateMidGrey);
    r.grade.enabled = on(LookParam::GradeEnabled);
    r.grade.temperature_k = b.get(LookParam::GradeTemperatureK);
    r.grade.tint = b.get(LookParam::GradeTint);
    r.grade.lift = b.color(LookParam::GradeLift);
    r.grade.gamma = b.color(LookParam::GradeGamma);
    r.grade.gain = b.color(LookParam::GradeGain);
    r.grade.contrast = b.get(LookParam::GradeContrast);
    r.grade.contrast_pivot = b.get(LookParam::GradeContrastPivot);
    r.grade.saturation = b.get(LookParam::GradeSaturation);
    r.grade.curve[0] = b.color(LookParam::GradeCurve0);
    r.grade.curve[1] = b.color(LookParam::GradeCurve1);
    r.grade.curve[2] = b.color(LookParam::GradeCurve2);
    r.grade.curve[3] = b.color(LookParam::GradeCurve3);
    r.grade.curve[4] = b.color(LookParam::GradeCurve4);
    r.grade.lut_strength = b.get(LookParam::GradeLutStrength);
    r.sharpen = {on(LookParam::SharpenEnabled), b.get(LookParam::SharpenSharpness)};
    r.chromatic_aberration = {on(LookParam::CaEnabled), b.get(LookParam::CaIntensity)};
    r.vignette.enabled = on(LookParam::VignetteEnabled);
    r.vignette.intensity = b.get(LookParam::VignetteIntensity);
    r.vignette.falloff = b.get(LookParam::VignetteFalloff);
    r.vignette.roundness = b.get(LookParam::VignetteRoundness);
    r.vignette.color = b.color(LookParam::VignetteColor);
    r.film_grain = {on(LookParam::GrainEnabled), b.get(LookParam::GrainIntensity), b.get(LookParam::GrainResponse)};
    r.output = {b.get(LookParam::OutPaperWhiteNits), b.get(LookParam::OutPeakNits)};
    return r;
}

// ---------------------------------------------------------------------------------------------
// Effect graph

const char* look_graph_error_name(LookGraphError error) {
    switch (error) {
    case LookGraphError::None:
        return "none";
    case LookGraphError::Empty:
        return "empty";
    case LookGraphError::DuplicateNode:
        return "duplicate_node";
    case LookGraphError::MissingToneMap:
        return "missing_tone_map";
    case LookGraphError::MissingOutputTransform:
        return "missing_output_transform";
    case LookGraphError::OutputNotLast:
        return "output_not_last";
    case LookGraphError::DomainMismatch:
        return "domain_mismatch";
    case LookGraphError::StageOrder:
        return "stage_order";
    case LookGraphError::LensDirtWithoutBloom:
        return "lens_dirt_without_bloom";
    case LookGraphError::LensFlareWithoutBloom:
        return "lens_flare_without_bloom";
    case LookGraphError::GrainBeforeSharpen:
        return "grain_before_sharpen";
    case LookGraphError::ExposureAfterToneMap:
        return "exposure_after_tone_map";
    }
    return "unknown";
}

LookEffectGraph LookEffectGraph::makeDefault() {
    LookEffectGraph g;
    for (u32 i = 0; i < kLookEffectCount; ++i) {
        g.push(static_cast<LookEffect>(i));
    }
    return g;
}

bool LookEffectGraph::push(LookEffect effect) {
    if (m_count >= m_nodes.size() || static_cast<u32>(effect) >= kLookEffectCount) {
        return false;
    }
    m_nodes[m_count++] = effect;
    return true;
}

u32 LookEffectGraph::indexOf(LookEffect effect) const {
    for (u32 i = 0; i < m_count; ++i) {
        if (m_nodes[i] == effect) {
            return i;
        }
    }
    return m_count;
}

LookGraphValidation LookEffectGraph::validate() const {
    LookGraphValidation v{};
    const auto fail = [&v](LookGraphError e, u32 i) {
        v.error = e;
        v.node_index = i;
        return v;
    };
    if (m_count == 0u) {
        return fail(LookGraphError::Empty, 0);
    }
    bool seen[kLookEffectCount] = {};
    for (u32 i = 0; i < m_count; ++i) {
        const u32 e = static_cast<u32>(m_nodes[i]);
        if (seen[e]) {
            return fail(LookGraphError::DuplicateNode, i);
        }
        seen[e] = true;
    }
    if (!seen[static_cast<u32>(LookEffect::ToneMap)]) {
        return fail(LookGraphError::MissingToneMap, m_count);
    }
    if (!seen[static_cast<u32>(LookEffect::OutputTransform)]) {
        return fail(LookGraphError::MissingOutputTransform, m_count);
    }
    if (m_nodes[m_count - 1u] != LookEffect::OutputTransform) {
        return fail(LookGraphError::OutputNotLast, indexOf(LookEffect::OutputTransform));
    }
    const u32 toneMapAt = indexOf(LookEffect::ToneMap);
    if (contains(LookEffect::Exposure) && indexOf(LookEffect::Exposure) > toneMapAt) {
        return fail(LookGraphError::ExposureAfterToneMap, indexOf(LookEffect::Exposure));
    }
    if (contains(LookEffect::LensDirt) &&
        (!contains(LookEffect::Bloom) || indexOf(LookEffect::Bloom) > indexOf(LookEffect::LensDirt))) {
        return fail(LookGraphError::LensDirtWithoutBloom, indexOf(LookEffect::LensDirt));
    }
    if (contains(LookEffect::LensFlare) &&
        (!contains(LookEffect::Bloom) || indexOf(LookEffect::Bloom) > indexOf(LookEffect::LensFlare))) {
        return fail(LookGraphError::LensFlareWithoutBloom, indexOf(LookEffect::LensFlare));
    }
    if (contains(LookEffect::Sharpen) && contains(LookEffect::FilmGrain) &&
        indexOf(LookEffect::FilmGrain) < indexOf(LookEffect::Sharpen)) {
        return fail(LookGraphError::GrainBeforeSharpen, indexOf(LookEffect::FilmGrain));
    }
    // Domain chain + stage monotonicity.
    LookDomain domain = LookDomain::SceneHdr;
    LookStage stage = LookStage::PreUpscale;
    for (u32 i = 0; i < m_count; ++i) {
        const LookEffectInfo& info = look_effect_info(m_nodes[i]);
        const bool accepts = info.input == LookDomain::Any
                                 ? (domain == LookDomain::SceneHdr || domain == LookDomain::DisplayLinear)
                                 : info.input == domain;
        if (!accepts) {
            return fail(LookGraphError::DomainMismatch, i);
        }
        // "Any" nodes (vignette, CA) may sit in either the HDR or the display section; they do not
        // advance the stage.
        if (info.input != LookDomain::Any) {
            if (static_cast<u8>(info.stage) < static_cast<u8>(stage)) {
                return fail(LookGraphError::StageOrder, i);
            }
            stage = info.stage;
        }
        if (info.output != LookDomain::Any) {
            domain = info.output;
        }
    }
    return v;
}

} // namespace fuse::renderer::look
