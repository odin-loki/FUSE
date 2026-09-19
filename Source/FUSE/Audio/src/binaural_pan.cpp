#include <fuse/audio/binaural_pan.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::audio {

namespace {

constexpr float kHrtfCoLocatedEpsilon = 1e-5f;

} // namespace

float hrtf_co_located_epsilon() {
    return kHrtfCoLocatedEpsilon;
}

HrtfIrStub make_empty_hrtf_ir() {
    return HrtfIrStub{};
}

HrtfIrStub make_hrtf_ir_stub(const float* samples, u32 length) {
    if (samples == nullptr || length == 0) {
        return make_empty_hrtf_ir();
    }
    return HrtfIrStub{samples, length};
}

bool has_hrtf_ir(const HrtfIrStub& ir) {
    return ir.samples != nullptr && ir.length > 0;
}

bool is_empty_hrtf_ir(const HrtfIrStub& ir) {
    return !has_hrtf_ir(ir);
}

bool should_use_hrtf_ir(const HrtfIrStub& ir) {
    return has_hrtf_ir(ir);
}

bool should_skip_hrtf_convolution(const HrtfIrStub& ir) {
    return is_empty_hrtf_ir(ir);
}

bool is_nonnull_zero_length_hrtf_ir(const HrtfIrStub& ir) {
    return ir.samples != nullptr && ir.length == 0;
}

HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir) {
    HrtfIrPreflight preflight{};
    preflight.nullSamples = ir.samples == nullptr;
    preflight.zeroLength = ir.length == 0;
    preflight.malformedIr = is_nonnull_zero_length_hrtf_ir(ir);
    preflight.emptyIr = is_empty_hrtf_ir(ir);
    return preflight;
}

bool can_convolve_hrtf_ir(const HrtfIrPreflight& preflight) {
    return preflight.can_convolve();
}

bool should_skip_hrtf_ir_convolution(const HrtfIrPreflight& preflight) {
    return preflight.should_skip_convolution();
}

const char* hrtf_ir_reject_reason_name(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return "None";
    case HrtfIrRejectReason::NullSamples:
        return "NullSamples";
    case HrtfIrRejectReason::ZeroLength:
        return "ZeroLength";
    case HrtfIrRejectReason::MalformedIr:
        return "MalformedIr";
    }
    return "Unknown";
}

HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfIrRejectReason::None;
    }
    if (preflight.malformedIr) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (preflight.nullSamples) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (preflight.zeroLength) {
        return HrtfIrRejectReason::ZeroLength;
    }
    return HrtfIrRejectReason::NullSamples;
}

bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrPreflight preflight = preflight_hrtf_ir(ir);
    if (reason != nullptr) {
        *reason = classify_hrtf_ir_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    return preflight_hrtf_ir_ready(ir, &reason);
}

bool should_skip_hrtf_ir_ready(const HrtfIrStub& ir) {
    return !preflight_hrtf_ir_ready(ir);
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight{};
    preflight.hrtfDisabled = !hrtf_enabled;
    preflight.coLocated = is_co_located_hrtf_source(rel_listener);
    preflight.emptyIr = is_empty_hrtf_ir(ir);
    preflight.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.skipped = should_skip_hrtf_pan_path(preflight.path);
    return preflight;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool can_apply_spatial_hrtf_pan(const HrtfPanPathPreflight& preflight) {
    return preflight.can_spatial_pan();
}

bool can_convolve_hrtf_pan_path(const HrtfPanPathPreflight& preflight) {
    return preflight.can_convolve();
}

bool should_skip_hrtf_pan_path_preflight(const HrtfPanPathPreflight& preflight) {
    return preflight.should_skip();
}

bool uses_ild_itd_stub_hrtf_pan_path(const HrtfPanPathPreflight& preflight) {
    return preflight.uses_ild_itd_stub();
}

const char* hrtf_pan_path_reject_reason_name(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return "None";
    case HrtfPanPathRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfPanPathRejectReason::CoLocated:
        return "CoLocated";
    }
    return "Unknown";
}

HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight) {
    if (!preflight.skipped) {
        return HrtfPanPathRejectReason::None;
    }
    if (preflight.hrtfDisabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (preflight.coLocated) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, HrtfPanPathRejectReason* reason) {
    const HrtfPanPathPreflight preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    if (reason != nullptr) {
        *reason = classify_hrtf_pan_path_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   HrtfPanPathRejectReason* reason) {
    return preflight_hrtf_pan_path_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);
}

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, HrtfPanPathRejectReason& reason) {
    return preflight_hrtf_pan_path_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
                                 HrtfPanPathRejectReason& reason) {
    return preflight_hrtf_pan_path_ready(hrtf_enabled, rel_listener, &reason);
}

bool should_skip_hrtf_pan_path_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                     const Vec3& rel_listener) {
    return !preflight_hrtf_pan_path_ready(hrtf_enabled, ir, rel_listener);
}

bool should_skip_hrtf_pan_path_ready(bool hrtf_enabled, const Vec3& rel_listener) {
    return !preflight_hrtf_pan_path_ready(hrtf_enabled, rel_listener);
}

HrtfPanPath resolve_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                  const Vec3& rel_listener) {
    if (!should_apply_hrtf_pan(hrtf_enabled, rel_listener)) {
        return HrtfPanPath::Bypass;
    }
    if (should_use_hrtf_ir(ir)) {
        return HrtfPanPath::Convolution;
    }
    return HrtfPanPath::IldItdStub;
}

HrtfPanPath resolve_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return resolve_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool is_hrtf_pan_bypassed(HrtfPanPath path) {
    return is_hrtf_pan_path_bypass(path);
}

bool should_skip_hrtf_pan_path(HrtfPanPath path) {
    return is_hrtf_pan_bypassed(path);
}

bool is_spatial_hrtf_pan_path(HrtfPanPath path) {
    return path != HrtfPanPath::Bypass;
}

bool hrtf_pan_path_uses_convolution(HrtfPanPath path) {
    return path == HrtfPanPath::Convolution;
}

bool hrtf_pan_path_uses_ild_itd_stub(HrtfPanPath path) {
    return path == HrtfPanPath::IldItdStub;
}

bool is_hrtf_pan_path_bypass(HrtfPanPath path) {
    return path == HrtfPanPath::Bypass;
}

bool is_bypass_hrtf_pan_path(HrtfPanPath path) {
    return is_hrtf_pan_path_bypass(path);
}

bool should_skip_hrtf_spatial_pan(HrtfPanPath path) {
    return is_hrtf_pan_path_bypass(path);
}

bool is_co_located_hrtf_source(const Vec3& rel_listener) {
    return rel_listener.length() < kHrtfCoLocatedEpsilon;
}

bool should_skip_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return !hrtf_enabled || is_co_located_hrtf_source(rel_listener);
}

bool should_apply_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return !should_skip_hrtf_pan(hrtf_enabled, rel_listener);
}

bool should_bypass_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return should_skip_hrtf_pan(hrtf_enabled, rel_listener);
}

namespace {

float clamp_unit(float value) {
    return std::clamp(value, -1.f, 1.f);
}

PanLawGains sample_equal_power_pan(float pan) {
    PanLawGains gains;
    gains.left = std::sqrt(0.5f * (1.f - pan));
    gains.right = std::sqrt(0.5f * (1.f + pan));
    return gains;
}

PanLawGains sample_linear_pan(float pan) {
    PanLawGains gains;
    gains.left = 0.5f * (1.f - pan);
    gains.right = 0.5f * (1.f + pan);
    return gains;
}

} // namespace

float clamp_pan_position(float pan) {
    return std::clamp(pan, -1.f, 1.f);
}

float compute_pan_position_from_azimuth(float azimuth, float max_ild_pan) {
    return clamp_pan_position(std::sin(azimuth) * max_ild_pan);
}

float compute_itd_from_azimuth(float azimuth, const BinauralPanParams& params) {
    return params.max_itd_seconds * std::sin(azimuth);
}

float compute_elevation_factor(float elevation, const BinauralPanParams& params) {
    return 1.f - params.elevation_rolloff * std::fabs(std::sin(elevation));
}

PanLawGains sample_pan_law(float pan, PanLaw law) {
    const float clamped = clamp_pan_position(pan);
    switch (law) {
    case PanLaw::Linear:
        return sample_linear_pan(clamped);
    case PanLaw::EqualPower:
    default:
        return sample_equal_power_pan(clamped);
    }
}

BinauralPanAngles compute_binaural_angles(const Vec3& rel_listener) {
    BinauralPanAngles angles;
    const float distance = rel_listener.length();
    if (distance < kHrtfCoLocatedEpsilon) {
        return angles;
    }

    angles.azimuth = std::atan2(rel_listener.x, -rel_listener.z);
    angles.elevation = std::asin(clamp_unit(rel_listener.y / distance));
    return angles;
}

BinauralPanAngles compute_binaural_angles(const Vec3& world_relative, const ListenerBasis& basis) {
    return compute_binaural_angles(to_listener_space(world_relative, basis));
}

BinauralPanGains compute_binaural_pan_gains(const BinauralPanAngles& angles,
                                             const BinauralPanParams& params) {
    BinauralPanGains gains;

    const float pan = compute_pan_position_from_azimuth(angles.azimuth, params.max_ild_pan);
    const PanLawGains pan_gains = sample_pan_law(pan, params.pan_law);
    gains.left = pan_gains.left;
    gains.right = pan_gains.right;
    gains.itd_seconds = compute_itd_from_azimuth(angles.azimuth, params);

    const float elevation_factor = compute_elevation_factor(angles.elevation, params);
    gains.left *= elevation_factor;
    gains.right *= elevation_factor;

    clamp_binaural_pan_gains(gains);
    return gains;
}

BinauralPanGains compute_binaural_pan_gains(const Vec3& rel_listener,
                                             const BinauralPanParams& params) {
    return compute_binaural_pan_gains(compute_binaural_angles(rel_listener), params);
}

BinauralPanGains compute_binaural_pan_gains(const Vec3& world_relative, const ListenerBasis& basis,
                                             const BinauralPanParams& params) {
    return compute_binaural_pan_gains(compute_binaural_angles(world_relative, basis), params);
}

ListenerBasis compute_listener_basis(const AudioListener& listener) {
    return make_listener_basis_safe(listener.forward, listener.up);
}

BinauralPanAngles compute_binaural_angles(const AudioListener& listener, const Vec3& source_position) {
    const Vec3 world_relative = source_position - listener.position;
    return compute_binaural_angles(world_relative, compute_listener_basis(listener));
}

BinauralPanGains compute_binaural_pan_gains(const AudioListener& listener, const Vec3& source_position,
                                             const BinauralPanParams& params) {
    return compute_binaural_pan_gains(compute_binaural_angles(listener, source_position), params);
}

BinauralPanGains make_centre_binaural_pan_gains() {
    BinauralPanGains gains;
    gains.left = 0.5f;
    gains.right = 0.5f;
    gains.itd_seconds = 0.f;
    return gains;
}

float compute_pan_spread(const BinauralPanGains& gains) {
    return std::fabs(gains.left - gains.right);
}

bool is_centre_panned(const BinauralPanGains& gains, float epsilon) {
    return compute_pan_spread(gains) <= epsilon;
}

float compute_binaural_pan_energy(const BinauralPanGains& gains) {
    return gains.left * gains.left + gains.right * gains.right;
}

bool has_nonzero_itd(const BinauralPanGains& gains, float epsilon) {
    return std::fabs(gains.itd_seconds) > epsilon;
}

void scale_binaural_pan_gains(BinauralPanGains& gains, float scale) {
    gains.left *= scale;
    gains.right *= scale;
    clamp_binaural_pan_gains(gains);
}

void apply_binaural_pan_to_sample(float mono, const BinauralPanGains& pan, float attenuation,
                                  float& left, float& right) {
    const float scaled = mono * attenuation;
    left += scaled * pan.left;
    right += scaled * pan.right;
}

void apply_centre_binaural_pan_to_sample(float mono, float attenuation, float& left, float& right) {
    const float scaled = mono * attenuation;
    left += scaled;
    right += scaled;
}

BinauralPanGains compute_binaural_pan_gains_guarded(bool hrtf_enabled, const Vec3& rel_listener,
                                                    const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, rel_listener, 1.f, 1.f, {}, params);
    return compute_binaural_pan_gains_from_preflight(preflight, rel_listener, {}, params);
}

BinauralPanGains compute_binaural_pan_gains_guarded(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener,
                                                    const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f, {}, params);
    return compute_binaural_pan_gains_from_preflight(preflight, rel_listener, {}, params);
}

BinauralPanGains compute_binaural_pan_gains_for_path(HrtfPanPath path, const Vec3& rel_listener,
                                                    const BinauralPanParams& params) {
    if (path == HrtfPanPath::Bypass) {
        return make_centre_binaural_pan_gains();
    }
    // Convolution path deferred — ILD/ITD stub until delay-line / IR wiring lands.
    return compute_binaural_pan_gains(rel_listener, params);
}

BinauralPanGains lerp_binaural_pan_gains(const BinauralPanGains& from, const BinauralPanGains& to,
                                         float t) {
    const float blend = std::clamp(t, 0.f, 1.f);
    BinauralPanGains gains;
    gains.left = from.left + (to.left - from.left) * blend;
    gains.right = from.right + (to.right - from.right) * blend;
    gains.itd_seconds = from.itd_seconds + (to.itd_seconds - from.itd_seconds) * blend;
    clamp_binaural_pan_gains(gains);
    return gains;
}

void clamp_binaural_pan_gains(BinauralPanGains& gains) {
    gains.left = std::clamp(gains.left, 0.f, 1.f);
    gains.right = std::clamp(gains.right, 0.f, 1.f);
}

float clamp_hrtf_attenuation(float attenuation) {
    return std::clamp(attenuation, 0.f, 1.f);
}

float clamp_hrtf_attenuation_coupling_weight(float weight) {
    return std::clamp(weight, 0.f, 1.f);
}

bool is_unity_hrtf_attenuation(float distance_attenuation, float occlusion_gain) {
    return clamp_hrtf_attenuation(distance_attenuation) >= 1.f - 1e-5f
        && clamp_hrtf_attenuation(occlusion_gain) >= 1.f - 1e-5f;
}

bool should_apply_hrtf_attenuation_coupling(HrtfPanPath path) {
    return is_spatial_hrtf_pan_path(path);
}

bool should_skip_hrtf_attenuation_coupling(HrtfPanPath path) {
    return is_hrtf_pan_path_bypass(path);
}

bool should_narrow_hrtf_spatial_image(HrtfPanPath path, float distance_attenuation,
                                      float occlusion_gain) {
    return should_apply_hrtf_attenuation_coupling(path)
        && !is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
}

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight{};
    preflight.bypassPath = is_hrtf_pan_path_bypass(path);
    preflight.unityAttenuation =
        is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    preflight.spatialBlend =
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    preflight.skipped = preflight.bypassPath || preflight.unityAttenuation;
    return preflight;
}

bool can_narrow_hrtf_spatial_image(const HrtfAttenuationCouplingPreflight& preflight) {
    return preflight.can_narrow();
}

bool should_skip_hrtf_attenuation_coupling_preflight(const HrtfAttenuationCouplingPreflight& preflight) {
    return preflight.should_skip();
}

const char* hrtf_attenuation_coupling_reject_reason_name(HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return "None";
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return "BypassPath";
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
    const HrtfAttenuationCouplingPreflight& preflight) {
    if (!preflight.skipped) {
        return HrtfAttenuationCouplingRejectReason::None;
    }
    if (preflight.bypassPath) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (preflight.unityAttenuation) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

bool preflight_hrtf_attenuation_coupling_ready(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason* reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    const HrtfAttenuationCouplingPreflight preflight =
        preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain, coupling,
                                          params);
    if (reason != nullptr) {
        *reason = classify_hrtf_attenuation_coupling_reject(preflight);
    }
    return preflight.can_narrow();
}

bool try_preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
    const BinauralPanParams& params) {
    return preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                     &reason, coupling, params);
}

bool should_skip_hrtf_attenuation_coupling_ready(HrtfPanPath path, float distance_attenuation,
                                                 float occlusion_gain,
                                                 const HrtfAttenuationCoupling& coupling,
                                                 const BinauralPanParams& params) {
    return !preflight_hrtf_attenuation_coupling_ready(path, distance_attenuation, occlusion_gain,
                                                      nullptr, coupling, params);
}

float compute_hrtf_distance_factor(float distance_attenuation,
                                   const BinauralPanParams& params) {
    const float atten = clamp_hrtf_attenuation(distance_attenuation);
    return params.min_spatial_blend + (1.f - params.min_spatial_blend) * atten;
}

void apply_spatial_blend(BinauralPanGains& gains, float blend) {
    const float clamped = std::clamp(blend, 0.f, 1.f);
    const float centre = 0.5f * (gains.left + gains.right);
    gains.left = centre + (gains.left - centre) * clamped;
    gains.right = centre + (gains.right - centre) * clamped;
    clamp_binaural_pan_gains(gains);
}

bool is_unity_hrtf_spatial_blend(float blend, float epsilon) {
    return blend >= 1.f - epsilon;
}

void apply_hrtf_spatial_blend_guarded(BinauralPanGains& gains, float blend) {
    if (is_unity_hrtf_spatial_blend(blend)) {
        return;
    }
    apply_spatial_blend(gains, blend);
}

bool should_skip_hrtf_spatial_blend(float distance_attenuation, float occlusion_gain,
                                    const HrtfAttenuationCoupling& coupling,
                                    const BinauralPanParams& params) {
    return is_unity_hrtf_spatial_blend(
        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params));
}

void apply_hrtf_distance_factor(BinauralPanGains& gains, float distance_attenuation,
                                const BinauralPanParams& params) {
    apply_hrtf_spatial_blend_guarded(gains,
                                     compute_hrtf_distance_factor(distance_attenuation, params));
}

float compute_hrtf_spatial_blend(float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    const float distance_blend =
        compute_hrtf_distance_factor(clamp_hrtf_attenuation(distance_attenuation), params);
    const float occlusion_blend =
        compute_hrtf_distance_factor(clamp_hrtf_attenuation(occlusion_gain), params);
    const float weight = clamp_hrtf_attenuation_coupling_weight(coupling.occlusion_weight);
    return distance_blend * (1.f - weight) + occlusion_blend * weight;
}

void apply_hrtf_attenuation_coupling(BinauralPanGains& gains, float distance_attenuation,
                                     float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                     const BinauralPanParams& params) {
    if (should_skip_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params)) {
        return;
    }
    apply_hrtf_spatial_blend_guarded(
        gains, compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params));
}

void apply_hrtf_attenuation_coupling_for_path(BinauralPanGains& gains, HrtfPanPath path,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    if (!can_narrow_hrtf_spatial_image(
            preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain,
                                                coupling, params))) {
        return;
    }
    apply_hrtf_attenuation_coupling(gains, distance_attenuation, occlusion_gain, coupling, params);
}

BinauralPanGains compute_binaural_pan_gains_coupled(bool hrtf_enabled, const Vec3& rel_listener,
                                                    float distance_attenuation,
                                                    float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling,
                                                    const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    return compute_binaural_pan_gains_from_preflight(preflight, rel_listener, coupling, params);
}

BinauralPanGains compute_binaural_pan_gains_coupled(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener,
                                                    float distance_attenuation, float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling,
                                                    const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    return compute_binaural_pan_gains_from_preflight(preflight, rel_listener, coupling, params);
}

BinauralPanGains compute_binaural_pan_gains_coupled_for_path(HrtfPanPath path,
                                                              const Vec3& rel_listener,
                                                              float distance_attenuation,
                                                              float occlusion_gain,
                                                              const HrtfAttenuationCoupling& coupling,
                                                              const BinauralPanParams& params) {
    BinauralPanGains pan = compute_binaural_pan_gains_for_path(path, rel_listener, params);
    apply_hrtf_attenuation_coupling_for_path(pan, path, distance_attenuation, occlusion_gain,
                                             coupling, params);
    return pan;
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                              const Vec3& rel_listener, float distance_attenuation,
                                              float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    HrtfBinauralPreflight preflight{};
    preflight.ir = preflight_hrtf_ir(ir);
    preflight.panPath = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    preflight.attenuationCoupling = preflight_hrtf_attenuation_coupling(
        preflight.panPath.path, distance_attenuation, occlusion_gain, coupling, params);
    return preflight;
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    return preflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                   distance_attenuation, occlusion_gain, coupling, params);
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const AudioListener& listener,
                                              const Vec3& source_position, const HrtfIrStub& ir,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    const Vec3 world_relative = source_position - listener.position;
    const Vec3 rel_listener = to_listener_space(world_relative, compute_listener_basis(listener));
    return preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                  occlusion_gain, coupling, params);
}

HrtfBinauralPreflight preflight_hrtf_binaural(bool hrtf_enabled, const AudioListener& listener,
                                              const Vec3& source_position, float distance_attenuation,
                                              float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    return preflight_hrtf_binaural(hrtf_enabled, listener, source_position, make_empty_hrtf_ir(),
                                   distance_attenuation, occlusion_gain, coupling, params);
}

bool can_apply_hrtf_binaural_pan(const HrtfBinauralPreflight& preflight) {
    return preflight.can_spatial_pan();
}

bool can_convolve_hrtf_binaural(const HrtfBinauralPreflight& preflight) {
    return preflight.can_convolve();
}

bool can_narrow_hrtf_binaural_spatial_image(const HrtfBinauralPreflight& preflight) {
    return preflight.can_narrow_spatial_image();
}

bool should_skip_hrtf_binaural(const HrtfBinauralPreflight& preflight) {
    return preflight.is_bypass();
}

bool should_skip_hrtf_binaural_convolution(const HrtfBinauralPreflight& preflight) {
    return preflight.should_skip_convolution();
}

bool uses_ild_itd_stub_hrtf_binaural(const HrtfBinauralPreflight& preflight) {
    return preflight.uses_ild_itd_stub();
}

BinauralPanGains compute_binaural_pan_gains_from_preflight(const HrtfBinauralPreflight& preflight,
                                                           const Vec3& rel_listener,
                                                           const HrtfAttenuationCoupling& coupling,
                                                           const BinauralPanParams& params) {
    (void)coupling;
    BinauralPanGains pan =
        compute_binaural_pan_gains_for_path(preflight.path(), rel_listener, params);
    if (preflight.can_narrow_spatial_image()) {
        apply_hrtf_spatial_blend_guarded(pan, preflight.attenuationCoupling.spatialBlend);
    }
    return pan;
}

void apply_binaural_pan_to_sample_from_preflight(float mono, const HrtfBinauralPreflight& preflight,
                                                 const Vec3& rel_listener, float attenuation,
                                                 float& left, float& right,
                                                 const HrtfAttenuationCoupling& coupling,
                                                 const BinauralPanParams& params) {
    if (preflight.should_skip()) {
        apply_centre_binaural_pan_to_sample(mono, attenuation, left, right);
        return;
    }
    const BinauralPanGains pan =
        compute_binaural_pan_gains_from_preflight(preflight, rel_listener, coupling, params);
    apply_binaural_pan_to_sample(mono, pan, attenuation, left, right);
}

const char* hrtf_binaural_reject_reason_name(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    }
    return "Unknown";
}

const char* hrtf_convolution_reject_reason_name(HrtfConvolutionRejectReason reason) {
    switch (reason) {
    case HrtfConvolutionRejectReason::None:
        return "None";
    case HrtfConvolutionRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfConvolutionRejectReason::CoLocated:
        return "CoLocated";
    case HrtfConvolutionRejectReason::EmptyIr:
        return "EmptyIr";
    case HrtfConvolutionRejectReason::MalformedIr:
        return "MalformedIr";
    }
    return "Unknown";
}

HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
    if (!preflight.should_skip()) {
        return HrtfBinauralRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfBinauralRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfConvolutionRejectReason classify_hrtf_convolution_reject(
    const HrtfBinauralPreflight& preflight) {
    if (preflight.can_convolve()) {
        return HrtfConvolutionRejectReason::None;
    }
    if (preflight.panPath.hrtfDisabled) {
        return HrtfConvolutionRejectReason::HrtfDisabled;
    }
    if (preflight.panPath.coLocated) {
        return HrtfConvolutionRejectReason::CoLocated;
    }
    if (preflight.ir.malformedIr) {
        return HrtfConvolutionRejectReason::MalformedIr;
    }
    if (preflight.ir.emptyIr) {
        return HrtfConvolutionRejectReason::EmptyIr;
    }
    return HrtfConvolutionRejectReason::EmptyIr;
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                   const Vec3& rel_listener, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const HrtfBinauralPreflight preflight = preflight_hrtf_binaural(
        hrtf_enabled, ir, rel_listener, distance_attenuation, occlusion_gain, coupling, params);
    if (reason != nullptr) {
        *reason = classify_hrtf_binaural_reject(preflight);
    }
    return preflight.can_spatial_pan();
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
                                         distance_attenuation, occlusion_gain, reason, coupling,
                                         params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, const HrtfIrStub& ir,
                                   float distance_attenuation, float occlusion_gain,
                                   HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    const Vec3 world_relative = source_position - listener.position;
    const Vec3 rel_listener = to_listener_space(world_relative, compute_listener_basis(listener));
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, reason, coupling, params);
}

bool preflight_hrtf_binaural_ready(bool hrtf_enabled, const AudioListener& listener,
                                   const Vec3& source_position, float distance_attenuation,
                                   float occlusion_gain, HrtfBinauralRejectReason* reason,
                                   const HrtfAttenuationCoupling& coupling,
                                   const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, listener, source_position,
                                         make_empty_hrtf_ir(), distance_attenuation,
                                         occlusion_gain, reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
                                 float distance_attenuation, float occlusion_gain,
                                 HrtfBinauralRejectReason& reason,
                                 const HrtfAttenuationCoupling& coupling,
                                 const BinauralPanParams& params) {
    return preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                         occlusion_gain, &reason, coupling, params);
}

bool should_skip_hrtf_binaural_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                     const Vec3& rel_listener, float distance_attenuation,
                                     float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                     const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, ir, rel_listener, distance_attenuation,
                                          occlusion_gain, nullptr, coupling, params);
}

bool should_skip_hrtf_binaural_ready(bool hrtf_enabled, const Vec3& rel_listener,
                                     float distance_attenuation, float occlusion_gain,
                                     const HrtfAttenuationCoupling& coupling,
                                     const BinauralPanParams& params) {
    return !preflight_hrtf_binaural_ready(hrtf_enabled, rel_listener, distance_attenuation,
                                          occlusion_gain, nullptr, coupling, params);
}

bool preflight_hrtf_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                      const Vec3& rel_listener,
                                      HrtfConvolutionRejectReason* reason) {
    const HrtfBinauralPreflight preflight =
        preflight_hrtf_binaural(hrtf_enabled, ir, rel_listener, 1.f, 1.f);
    if (reason != nullptr) {
        *reason = classify_hrtf_convolution_reject(preflight);
    }
    return preflight.can_convolve();
}

bool try_preflight_hrtf_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
                                    const Vec3& rel_listener, HrtfConvolutionRejectReason& reason) {
    return preflight_hrtf_convolution_ready(hrtf_enabled, ir, rel_listener, &reason);
}

bool should_skip_hrtf_convolution_ready(bool hrtf_enabled, const HrtfIrStub& ir,
                                        const Vec3& rel_listener) {
    return !preflight_hrtf_convolution_ready(hrtf_enabled, ir, rel_listener);
}

} // namespace fuse::audio
