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

HrtfIrRejectReason hrtf_ir_reject_reason(const HrtfIrStub& ir) {
    if (has_hrtf_ir(ir)) {
        return HrtfIrRejectReason::None;
    }
    if (is_nonnull_zero_length_hrtf_ir(ir)) {
        return HrtfIrRejectReason::MalformedIr;
    }
    if (ir.samples == nullptr && ir.length > 0) {
        return HrtfIrRejectReason::NullSamples;
    }
    if (ir.length == 0) {
        return HrtfIrRejectReason::ZeroLength;
    }
    if (ir.samples == nullptr) {
        return HrtfIrRejectReason::NullSamples;
    }
    return HrtfIrRejectReason::ZeroLength;
}

bool hrtf_ir_rejects_for_reason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtf_ir_reject_reason(ir) == expected;
}

HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir) {
    HrtfIrPreflight preflight{};
    preflight.reason = hrtf_ir_reject_reason(ir);
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

HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const Vec3& rel_listener) {
    if (!hrtf_enabled) {
        return HrtfPanPathRejectReason::HrtfDisabled;
    }
    if (is_co_located_hrtf_source(rel_listener)) {
        return HrtfPanPathRejectReason::CoLocated;
    }
    return HrtfPanPathRejectReason::None;
}

bool hrtf_pan_path_rejects_for_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      HrtfPanPathRejectReason expected) {
    return hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener) == expected;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    HrtfPanPathPreflight preflight{};
    preflight.reason = hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);
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

HrtfAttenuationCouplingRejectReason hrtf_attenuation_coupling_reject_reason(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain) {
    if (is_hrtf_pan_path_bypass(path)) {
        return HrtfAttenuationCouplingRejectReason::BypassPath;
    }
    if (is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain)) {
        return HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    }
    return HrtfAttenuationCouplingRejectReason::None;
}

bool hrtf_attenuation_coupling_rejects_for_reason(HrtfPanPath path, float distance_attenuation,
                                                  float occlusion_gain,
                                                  HrtfAttenuationCouplingRejectReason expected) {
    return hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain)
        == expected;
}

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(
    HrtfPanPath path, float distance_attenuation, float occlusion_gain,
    const HrtfAttenuationCoupling& coupling, const BinauralPanParams& params) {
    HrtfAttenuationCouplingPreflight preflight{};
    preflight.reason =
        hrtf_attenuation_coupling_reject_reason(path, distance_attenuation, occlusion_gain);
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

namespace {

HrtfBinauralRejectReason map_hrtf_ir_reject_reason(HrtfIrRejectReason reason) {
    switch (reason) {
    case HrtfIrRejectReason::None:
        return HrtfBinauralRejectReason::None;
    case HrtfIrRejectReason::NullSamples:
        return HrtfBinauralRejectReason::NullSamples;
    case HrtfIrRejectReason::ZeroLength:
        return HrtfBinauralRejectReason::ZeroLength;
    case HrtfIrRejectReason::MalformedIr:
        return HrtfBinauralRejectReason::MalformedIr;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason map_hrtf_pan_path_reject_reason(HrtfPanPathRejectReason reason) {
    switch (reason) {
    case HrtfPanPathRejectReason::None:
        return HrtfBinauralRejectReason::None;
    case HrtfPanPathRejectReason::HrtfDisabled:
        return HrtfBinauralRejectReason::HrtfDisabled;
    case HrtfPanPathRejectReason::CoLocated:
        return HrtfBinauralRejectReason::CoLocated;
    }
    return HrtfBinauralRejectReason::None;
}

HrtfBinauralRejectReason map_hrtf_attenuation_coupling_reject_reason(
    HrtfAttenuationCouplingRejectReason reason) {
    switch (reason) {
    case HrtfAttenuationCouplingRejectReason::None:
        return HrtfBinauralRejectReason::None;
    case HrtfAttenuationCouplingRejectReason::BypassPath:
        return HrtfBinauralRejectReason::BypassPath;
    case HrtfAttenuationCouplingRejectReason::UnityAttenuation:
        return HrtfBinauralRejectReason::UnityAttenuation;
    }
    return HrtfBinauralRejectReason::None;
}

void populate_hrtf_binaural_reject_reasons(HrtfBinauralPreflight& preflight) {
    preflight.reason = map_hrtf_pan_path_reject_reason(preflight.panPath.reason);
    if (preflight.reason != HrtfBinauralRejectReason::None) {
        preflight.convolutionReason = preflight.reason;
    } else {
        preflight.convolutionReason = map_hrtf_ir_reject_reason(preflight.ir.reason);
    }
    preflight.narrowingReason =
        map_hrtf_attenuation_coupling_reject_reason(preflight.attenuationCoupling.reason);
}

} // namespace

const char* hrtf_binaural_reject_reason_name(HrtfBinauralRejectReason reason) {
    switch (reason) {
    case HrtfBinauralRejectReason::None:
        return "None";
    case HrtfBinauralRejectReason::HrtfDisabled:
        return "HrtfDisabled";
    case HrtfBinauralRejectReason::CoLocated:
        return "CoLocated";
    case HrtfBinauralRejectReason::NullSamples:
        return "NullSamples";
    case HrtfBinauralRejectReason::ZeroLength:
        return "ZeroLength";
    case HrtfBinauralRejectReason::MalformedIr:
        return "MalformedIr";
    case HrtfBinauralRejectReason::BypassPath:
        return "BypassPath";
    case HrtfBinauralRejectReason::UnityAttenuation:
        return "UnityAttenuation";
    }
    return "Unknown";
}

HrtfBinauralRejectReason hrtf_binaural_reject_reason(const HrtfBinauralPreflight& preflight) {
    return map_hrtf_pan_path_reject_reason(preflight.panPath.reason);
}

HrtfBinauralRejectReason hrtf_binaural_convolution_reject_reason(
    const HrtfBinauralPreflight& preflight) {
    const HrtfBinauralRejectReason pan_reason = map_hrtf_pan_path_reject_reason(preflight.panPath.reason);
    if (pan_reason != HrtfBinauralRejectReason::None) {
        return pan_reason;
    }
    return map_hrtf_ir_reject_reason(preflight.ir.reason);
}

HrtfBinauralRejectReason hrtf_binaural_narrowing_reject_reason(
    const HrtfBinauralPreflight& preflight) {
    return map_hrtf_attenuation_coupling_reject_reason(preflight.attenuationCoupling.reason);
}

bool hrtf_binaural_rejects_for_reason(const HrtfBinauralPreflight& preflight,
                                      HrtfBinauralRejectReason expected) {
    return hrtf_binaural_reject_reason(preflight) == expected;
}

bool hrtf_binaural_rejects_for_convolution_reason(const HrtfBinauralPreflight& preflight,
                                                  HrtfBinauralRejectReason expected) {
    return hrtf_binaural_convolution_reject_reason(preflight) == expected;
}

bool hrtf_binaural_rejects_for_narrowing_reason(const HrtfBinauralPreflight& preflight,
                                                HrtfBinauralRejectReason expected) {
    return hrtf_binaural_narrowing_reject_reason(preflight) == expected;
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
    populate_hrtf_binaural_reject_reasons(preflight);
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

} // namespace fuse::audio

// --- deepen additive from deepen-b72-hrtf-pan-guards-e91a ---
bool should_skip_hrtf_attenuation_coupling(float distance_attenuation, float occlusion_gain,
    if (should_skip_hrtf_attenuation_coupling(distance_attenuation, occlusion_gain, coupling,
    if (!should_skip_hrtf_pan_path(path)
        && !should_skip_hrtf_attenuation_coupling(distance_attenuation, occlusion_gain, coupling,

// --- deepen additive from deepen-hrtf-pan-empty-ir-guards-91f9 ---
    if (should_skip_hrtf_pan(hrtf_enabled, rel_listener)) {
    if (!should_skip_hrtf_convolution(ir)) {
    return !should_skip_hrtf_attenuation_coupling(path);
    if (should_skip_hrtf_attenuation_coupling(path)) {

// --- deepen additive from deepen-b72-hrtf-pan-empty-ir-guards-3df6 ---
    if (should_skip_hrtf_spatial_pan(path)) {

// --- deepen additive from deepen-hrtf-guards-71c7 ---
bool should_skip_hrtf_ir_convolution(const HrtfIrStub& ir) {
    return !should_skip_hrtf_ir_convolution(ir);
const char* hrtf_pan_reject_reason_label(HrtfPanRejectReason reason) {
    case HrtfPanRejectReason::None:
    case HrtfPanRejectReason::Disabled:
    case HrtfPanRejectReason::CoLocated:
bool HrtfPanPreflight::can_apply_spatial_pan() const {
bool HrtfPanPreflight::skip_convolution() const {
bool HrtfPanPreflight::ready_for_stub_mix() const {
    return reject_reason == HrtfPanRejectReason::None || path == HrtfPanPath::Bypass;
bool HrtfAttenuationCouplingPreflight::can_narrow_image() const {
bool HrtfAttenuationCouplingPreflight::ready_for_coupling() const {
bool try_resolve_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
                               HrtfPanPath& out_path, HrtfPanRejectReason& out_reason) {
    out_reason = HrtfPanRejectReason::None;
            out_reason = HrtfPanRejectReason::Disabled;
            out_reason = HrtfPanRejectReason::CoLocated;
HrtfPanPreflight preflight_hrtf_pan(bool hrtf_enabled, const HrtfIrStub& ir,
    HrtfPanPreflight preflight;
HrtfPanPreflight preflight_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    HrtfPanRejectReason reason = HrtfPanRejectReason::None;
bool should_skip_hrtf_attenuation_coupling_mapping(float distance_attenuation,
    HrtfAttenuationCouplingPreflight preflight;
        should_skip_hrtf_attenuation_coupling_mapping(distance_attenuation, occlusion_gain);
HrtfAttenuationCouplingPreflight preflight_hrtf_coupled_pan(bool hrtf_enabled,
    const HrtfPanPreflight pan_preflight = preflight_hrtf_pan(hrtf_enabled, ir, rel_listener);
    if (should_skip_hrtf_attenuation_coupling_mapping(distance_attenuation, occlusion_gain)) {

// --- deepen additive from hrtf-preflight-guards-9323 ---
bool HrtfIrPreflight::can_use_convolution() const {
bool HrtfIrPreflight::should_fallback_to_ild_itd() const {
bool HrtfIrPreflight::ready_for_stub() const {
bool HrtfPanPathPreflight::can_apply_spatial_pan() const {
bool HrtfPanPathPreflight::should_bypass() const {
bool HrtfPanPathPreflight::ready_for_stub() const {
    preflight.skip_spatial_pan = should_skip_hrtf_spatial_pan(preflight.path);
bool HrtfAttenuationCouplingPreflight::can_apply_coupling() const {
bool HrtfAttenuationCouplingPreflight::ready_for_stub() const {
    preflight.skip_coupling = should_skip_hrtf_attenuation_coupling(path);
bool HrtfSpatialPanPreflight::can_apply_spatial_pan() const {
bool HrtfSpatialPanPreflight::can_apply_attenuation_coupling() const {
bool HrtfSpatialPanPreflight::ready_for_stub() const {
HrtfSpatialPanPreflight preflight_hrtf_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,
    HrtfSpatialPanPreflight preflight{};

// --- deepen additive from hrtf-preflight-guards-4c1e ---
    HrtfIrPreflight result;
    HrtfPanPathPreflight result;
    HrtfAttenuationCouplingPreflight result;

// --- deepen additive from hrtf-preflight-guards-1a3b ---
    HrtfIrPreflight preflight;
    HrtfPanPathPreflight preflight;
    const HrtfPanPathPreflight pan_preflight =

// --- deepen additive from hrtf-preflight-guards-69c4 ---
HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(HrtfPanPath path,

// --- deepen additive from hrtf-preflight-guards-3b8d ---
    HrtfSpatialPanPreflight result;

// --- deepen additive from hrtf-preflight-guards-3b23 ---
HrtfIrPreflight preflight_hrtf_ir(const float* samples, u32 length) {

// --- deepen additive from deepen-hrtf-preflights-44d8 ---
    return !should_skip_hrtf_spatial_pan(path);
bool should_skip_hrtf_attenuation_coupling_for_inputs(HrtfPanPath path, float distance_attenuation,
    if (should_skip_hrtf_attenuation_coupling_for_inputs(path, distance_attenuation,

// --- deepen additive from deepen-hrtf-preflight-guards-1cf6 ---
    return should_skip_hrtf_convolution(ir);
    preflight.canConvolution = !should_skip_hrtf_convolution(ir);
bool HrtfPanPathPreflight::can_spatial_pan() const {
bool HrtfPanPathPreflight::should_skip_pan() const {
    return should_skip_hrtf_pan_path(path);
bool HrtfAttenuationCouplingPreflight::can_couple() const {
bool HrtfAttenuationCouplingPreflight::should_skip_coupling() const {
    preflight.bypassPath = should_skip_hrtf_attenuation_coupling(path);

// --- deepen additive from deepen-b72-hrtf-preflights-201d ---
    preflight.bypassed = should_skip_hrtf_pan_path(preflight.path);

// --- deepen additive from deepen-hrtf-preflights-ccde ---
    HrtfIrPreflight out;
    HrtfPanPathPreflight out;
    HrtfAttenuationCouplingPreflight out;
    out.bypass_pan_path = should_skip_hrtf_pan_path(path);
    const HrtfPanPathPreflight pan = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);

// --- deepen additive from deepen-b72-hrtf-preflights-815c ---
const char* hrtf_ir_reject_reason_label(HrtfIrRejectReason reason) {
    case HrtfIrRejectReason::EmptyIr:
bool try_preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason& outReason) {
        outReason = HrtfIrRejectReason::None;
        outReason = HrtfIrRejectReason::ZeroLength;
        outReason = HrtfIrRejectReason::NullSamples;
    outReason = HrtfIrRejectReason::EmptyIr;
bool preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    HrtfIrRejectReason localReason = HrtfIrRejectReason::None;
const char* hrtf_pan_path_reject_reason_label(HrtfPanPathRejectReason reason) {
    case HrtfPanPathRejectReason::Disabled:
bool try_preflight_hrtf_spatial_pan(bool hrtf_enabled, const Vec3& rel_listener,
                                   HrtfPanPath& outPath, HrtfPanPathRejectReason& outReason) {
        outReason = HrtfPanPathRejectReason::Disabled;
        outReason = HrtfPanPathRejectReason::CoLocated;
    outReason = HrtfPanPathRejectReason::None;
                                HrtfPanPath* out_path, HrtfPanPathRejectReason* reason) {
    HrtfPanPathRejectReason localReason = HrtfPanPathRejectReason::None;
                                HrtfPanPathRejectReason* reason) {
            *reason = HrtfPanPathRejectReason::Disabled;
            *reason = HrtfPanPathRejectReason::CoLocated;
        *reason = HrtfPanPathRejectReason::None;
const char* hrtf_attenuation_coupling_reject_reason_label(HrtfAttenuationCouplingRejectReason reason) {
bool try_preflight_hrtf_attenuation_coupling(HrtfPanPath path, float distance_attenuation,
                                             HrtfAttenuationCouplingRejectReason& outReason) {
        outReason = HrtfAttenuationCouplingRejectReason::BypassPath;
        outReason = HrtfAttenuationCouplingRejectReason::UnityAttenuation;
    outReason = HrtfAttenuationCouplingRejectReason::None;
                                         HrtfAttenuationCouplingRejectReason* reason) {
    HrtfAttenuationCouplingRejectReason localReason = HrtfAttenuationCouplingRejectReason::None;

// --- deepen additive from deepen-b72-hrtf-preflights-eec6 ---
bool try_preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrPreflightRejectReason* reason) {
            *reason = HrtfIrPreflightRejectReason::NullSamples;
            *reason = HrtfIrPreflightRejectReason::ZeroLength;
        *reason = HrtfIrPreflightRejectReason::None;
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
                                 HrtfPanPreflightRejectReason* reason) {
            *reason = HrtfPanPreflightRejectReason::Disabled;
            *reason = HrtfPanPreflightRejectReason::CoLocated;
        *reason = HrtfPanPreflightRejectReason::None;
                                             HrtfAttenuationCouplingPreflightRejectReason* reason) {
            *reason = HrtfAttenuationCouplingPreflightRejectReason::BypassPath;
            *reason = HrtfAttenuationCouplingPreflightRejectReason::UnityAttenuation;
        *reason = HrtfAttenuationCouplingPreflightRejectReason::None;

// --- deepen additive from deepen-b72-hrtf-pan-empty-ir-guards-e7fc ---
    const HrtfIrRejectReason reason = hrtf_ir_reject_reason(ir);
    if (reason == HrtfIrRejectReason::None) {
bool should_skip_hrtf_attenuation_coupling_apply(HrtfPanPath path, float distance_attenuation,
    const HrtfAttenuationCouplingPreflight preflight =

// --- deepen additive from deepen-hrtf-preflights-981f ---
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrStub& ir) {
HrtfIrPreflight make_hrtf_ir_preflight(const HrtfIrStub& ir) {
bool preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
    const HrtfIrPreflight preflight = make_hrtf_ir_preflight(ir);
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled,
HrtfPanPathPreflight make_hrtf_pan_path_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
    const HrtfPanPathPreflight preflight =
    case HrtfAttenuationCouplingRejectReason::PanBypassed:
HrtfAttenuationCouplingRejectReason classify_hrtf_attenuation_coupling_reject(
        return HrtfAttenuationCouplingRejectReason::PanBypassed;
HrtfAttenuationCouplingPreflight make_hrtf_attenuation_coupling_preflight(
    preflight.bypass_path = should_skip_hrtf_attenuation_coupling(path);
        && should_skip_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling, params);
    preflight.skipped = preflight.reject_reason != HrtfAttenuationCouplingRejectReason::None;
                                        HrtfAttenuationCouplingRejectReason* reason,

// --- deepen additive from deepen-hrtf-b7-2-guards-9316 ---
        preflight.reject = HrtfIrPreflightReject::NullSamples;
        preflight.reject = HrtfIrPreflightReject::ZeroLength;
    preflight.reject = HrtfIrPreflightReject::None;
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflightReject* reject) {
    const HrtfIrPreflight preflight = preflight_hrtf_ir(ir);
        preflight.reject = HrtfPanPathPreflightReject::Disabled;
        preflight.reject = HrtfPanPathPreflightReject::CoLocated;
    preflight.reject = HrtfPanPathPreflightReject::None;
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                 const Vec3& rel_listener, HrtfPanPathPreflightReject* reject) {
    const HrtfPanPathPreflight preflight = preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
        preflight.reject = HrtfAttenuationCouplingPreflightReject::BypassPath;
        preflight.reject = HrtfAttenuationCouplingPreflightReject::UnityAttenuation;
    preflight.reject = HrtfAttenuationCouplingPreflightReject::None;
                                             HrtfAttenuationCouplingPreflightReject* reject,

// --- deepen additive from deepen-b72-hrtf-preflights-bc2e ---
HrtfIrPreflight preflight_hrtf_ir_stub(const HrtfIrStub& ir) {
HrtfPanPathPreflight preflight_hrtf_pan_path_guarded(bool hrtf_enabled, const HrtfIrStub& ir,
HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling_for_path(

// --- deepen additive from deepen-b72-hrtf-preflight-guards-df8b ---
    case HrtfIrRejectReason::Empty:
    case HrtfIrRejectReason::Malformed:
        return HrtfIrRejectReason::Malformed;
        return HrtfIrRejectReason::Empty;
    const HrtfIrRejectReason reject = classify_hrtf_ir_reject(ir);
    return reject == HrtfIrRejectReason::None;
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const Vec3& rel_listener) {
        return HrtfPanPathRejectReason::Disabled;
    const HrtfPanPathRejectReason reject = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
    return reject == HrtfPanPathRejectReason::None;
    const HrtfAttenuationCouplingRejectReason reject =
    return reject == HrtfAttenuationCouplingRejectReason::None;
HrtfBinauralPanPreflight preflight_hrtf_binaural_pan(bool hrtf_enabled, const HrtfIrStub& ir,
    HrtfBinauralPanPreflight preflight{};

// --- deepen additive from deepen-hrtf-preflights-bb6d ---
        preflight.reason = HrtfIrRejectReason::ZeroLength;
        preflight.reason = HrtfIrRejectReason::NullSamples;
        preflight.reject_reason = HrtfPanPathRejectReason::Disabled;
        preflight.reject_reason = HrtfPanPathRejectReason::CoLocated;

// --- deepen additive from deepen-hrtf-preflights-0f68 ---
EmptyHrtfIrPreflight preflightEmptyHrtfIr(const HrtfIrStub& ir) {
    EmptyHrtfIrPreflight preflight;
    return preflightEmptyHrtfIr(ir).hasIr();
HrtfPanPathPreflight preflightHrtfPanPath(bool hrtf_enabled, const HrtfIrStub& ir,
HrtfPanPathPreflight preflightHrtfPanPath(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflightHrtfPanPath(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
HrtfAttenuationCouplingPreflight preflightHrtfAttenuationCoupling(

// --- deepen additive from deepen-b72-hrtf-preflight-guards-f1c9 ---
HrtfGuardedPanPreflight preflight_hrtf_guarded_pan(bool hrtf_enabled, const HrtfIrStub& ir,
    HrtfGuardedPanPreflight preflight;

// --- deepen additive from deepen-b7-2-hrtf-composite-preflights-84ae ---
HrtfBinauralPreflight preflight_hrtf_binaural_for_path(HrtfPanPath path, const HrtfIrStub& ir,
    preflight.panPath.skipped = should_skip_hrtf_pan_path(path);
HrtfBinauralPreflight preflight_hrtf_binaural_for_path(HrtfPanPath path, float distance_attenuation,
bool can_apply_binaural_hrtf_pan(const HrtfBinauralPreflight& preflight) {
bool can_convolve_binaural_hrtf(const HrtfBinauralPreflight& preflight) {
bool can_narrow_binaural_hrtf_spatial_image(const HrtfBinauralPreflight& preflight) {

// --- deepen additive from deepen-b72-hrtf-preflights-9877 ---
    const HrtfPanPathPreflight& preflight, const Vec3& rel_listener,
bool is_consistent_hrtf_binaural_preflight(const HrtfBinauralPreflight& preflight) {
bool has_empty_hrtf_ir(const HrtfBinauralPreflight& preflight) {
bool should_apply_hrtf_attenuation_coupling(const HrtfBinauralPreflight& preflight) {
bool should_skip_hrtf_attenuation_coupling(const HrtfBinauralPreflight& preflight) {

// --- deepen additive from deepen-fuse-b7-2-hrtf-guards-708b ---
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& preflight) {
bool should_skip_hrtf_ir_preflight(const HrtfIrStub& ir) {
    return classify_hrtf_ir_reject(ir) != HrtfIrRejectReason::None;
bool try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
bool should_skip_hrtf_pan_path_preflight(bool hrtf_enabled, const Vec3& rel_listener) {
    return classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener) != HrtfPanPathRejectReason::None;
    HrtfAttenuationCouplingPreflight& preflight, const HrtfAttenuationCoupling& coupling,
bool should_skip_hrtf_attenuation_coupling_preflight(HrtfPanPath path, float distance_attenuation,
const char* hrtf_binaural_reject_reason_label(HrtfBinauralRejectReason reason) {
    case HrtfBinauralRejectReason::PanBypass:
    case HrtfBinauralRejectReason::IrFallback:
    case HrtfBinauralRejectReason::AttenuationSkipped:
HrtfBinauralRejectReason classify_hrtf_binaural_reject(const HrtfBinauralPreflight& preflight) {
        return HrtfBinauralRejectReason::PanBypass;
        return HrtfBinauralRejectReason::IrFallback;
        return HrtfBinauralRejectReason::AttenuationSkipped;
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
bool should_skip_hrtf_binaural_preflight(bool hrtf_enabled, const Vec3& rel_listener) {
    return should_skip_hrtf_pan_path_preflight(hrtf_enabled, rel_listener);

// --- deepen additive from deepen-b72-hrtf-preflight-reasons-53ba ---
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
                                  HrtfPanPathRejectReason& reason) {
                                              HrtfAttenuationCouplingRejectReason& reason,
    case HrtfBinauralRejectReason::EmptyIr:
HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const HrtfIrStub& ir,
    const HrtfPanPathRejectReason pan_reject =
    if (pan_reject == HrtfPanPathRejectReason::HrtfDisabled) {
    if (pan_reject == HrtfPanPathRejectReason::CoLocated) {
HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(
    const HrtfAttenuationCouplingRejectReason coupling_reject =
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::BypassPath) {
    if (coupling_reject == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {
                              HrtfBinauralRejectReason* reason,
    const HrtfBinauralRejectReason reject =
    return reject == HrtfBinauralRejectReason::None;
                              float occlusion_gain, HrtfBinauralRejectReason* reason,
                                  HrtfBinauralRejectReason& reason,
bool try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,

// --- deepen additive from b7-2-hrtf-reject-reasons-1115 ---
    return HrtfIrRejectReason::EmptyIr;
    preflight.skipped = preflight.reason != HrtfAttenuationCouplingRejectReason::None;
HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const Vec3& rel_listener) {
    const HrtfPanPathRejectReason pan_reason = hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);

// --- deepen additive from b72-hrtf-reject-reason-preflights-62b4 ---
    case HrtfIrRejectReason::MalformedStub:
        return HrtfIrRejectReason::MalformedStub;
HrtfBinauralRejectReason hrtf_binaural_reject_reason(HrtfPanPathRejectReason pan_reason) {
    return preflight.rejectReason() != HrtfBinauralRejectReason::None;

// --- deepen additive from deepen-hrtf-reject-reason-preflights-1e2a ---
    return preflight.reason != HrtfBinauralRejectReason::None;

// --- deepen additive from deepen-hrtf-reject-reason-preflights-83b7 ---
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& preflight, HrtfIrRejectReason& reason) {
    preflight.convolveRejectReason = preflight_hrtf_ir(ir).rejectReason;
                                 HrtfPanPathPreflight& preflight, HrtfPanPathRejectReason& reason) {
    HrtfAttenuationCouplingPreflight& preflight, HrtfAttenuationCouplingRejectReason& reason,
                                                  HrtfAttenuationCouplingRejectReason expected,
HrtfBinauralRejectReason classify_hrtf_binaural_reject(bool hrtf_enabled, const Vec3& rel_listener) {
                                 HrtfBinauralPreflight& preflight, HrtfBinauralRejectReason& reason,

// --- deepen additive from deepen-hrtf-reject-reason-preflights-cb75 ---
const char* hrtfIrRejectReasonName(HrtfIrRejectReason reason) {
HrtfIrRejectReason hrtfIrRejectReason(const HrtfIrStub& ir) {
bool hrtfIrRejectsForReason(const HrtfIrStub& ir, HrtfIrRejectReason expected) {
    return hrtfIrRejectReason(ir) == expected;
    preflight.reason = hrtfIrRejectReason(ir);
const char* hrtfPanPathRejectReasonName(HrtfPanPathRejectReason reason) {
HrtfPanPathRejectReason hrtfPanPathRejectReason(bool hrtf_enabled, const Vec3& rel_listener) {
    return hrtfPanPathRejectReason(hrtf_enabled, rel_listener) == expected;
    preflight.reason = hrtfPanPathRejectReason(hrtf_enabled, rel_listener);
    preflight.skipped = preflight.reason != HrtfPanPathRejectReason::None;
const char* hrtfAttenuationCouplingRejectReasonName(HrtfAttenuationCouplingRejectReason reason) {
HrtfAttenuationCouplingRejectReason hrtfAttenuationCouplingRejectReason(
    return hrtfAttenuationCouplingRejectReason(path, distance_attenuation, occlusion_gain)
        hrtfAttenuationCouplingRejectReason(path, distance_attenuation, occlusion_gain);
const char* hrtfBinauralRejectReasonName(HrtfBinauralRejectReason reason) {
HrtfBinauralRejectReason hrtfBinauralRejectReason(const HrtfBinauralPreflight& preflight) {
HrtfIrRejectReason hrtfBinauralConvolutionRejectReason(const HrtfBinauralPreflight& preflight) {
HrtfAttenuationCouplingRejectReason hrtfBinauralNarrowingRejectReason(
bool hrtfBinauralRejectsForReason(const HrtfBinauralPreflight& preflight,
    return hrtfBinauralRejectReason(preflight) == expected;
    preflight.reason = hrtfBinauralRejectReason(preflight);

// --- deepen additive from deepen-b72-hrtf-reject-reasons-54bb ---
HrtfBinauralRejectReason resolve_hrtf_binaural_reject_reason(
    if (pan_reason == HrtfPanPathRejectReason::HrtfDisabled) {
    if (pan_reason == HrtfPanPathRejectReason::CoLocated) {
    const HrtfIrRejectReason ir_reason = hrtf_ir_reject_reason(ir);
    if (ir_reason == HrtfIrRejectReason::MalformedIr) {
    if (ir_reason == HrtfIrRejectReason::EmptyIr) {
        return HrtfBinauralRejectReason::EmptyIr;
HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,
HrtfBinauralRejectReason hrtf_binaural_reject_reason(bool hrtf_enabled, const Vec3& rel_listener,
                                      float occlusion_gain, HrtfBinauralRejectReason expected,

// --- deepen additive from b72-hrtf-reject-reason-preflights-1e6e ---
    preflight.rejected = preflight.reason != HrtfIrRejectReason::None;
    preflight.rejected = preflight.reason != HrtfPanPathRejectReason::None;
    HrtfAttenuationCouplingRejectReason expected, const HrtfAttenuationCoupling& coupling,
    preflight.rejected = preflight.reason != HrtfAttenuationCouplingRejectReason::None;
HrtfBinauralRejectReason map_pan_path_reject_to_binaural(HrtfPanPathRejectReason reason) {
HrtfBinauralRejectReason map_attenuation_coupling_reject_to_binaural(
    if (pan_reason != HrtfPanPathRejectReason::None) {
    if (hrtf_ir_reject_reason(ir) != HrtfIrRejectReason::None) {
    const HrtfAttenuationCouplingRejectReason coupling_reason =
    if (coupling_reason != HrtfAttenuationCouplingRejectReason::None) {

// --- deepen additive from deepen-b72-hrtf-reject-reasons-db94 ---
HrtfBinauralRejectReason map_ir_reject_to_binaural(HrtfIrRejectReason reason) {
HrtfBinauralRejectReason map_attenuation_reject_to_binaural(
HrtfBinauralRejectReason compute_hrtf_binaural_reject_reason(
    const HrtfIrPreflight& ir, const HrtfPanPathPreflight& pan_path,
    const HrtfAttenuationCouplingPreflight& attenuation) {
    const HrtfBinauralRejectReason pan_reject = map_pan_path_reject_to_binaural(pan_path.reason);
    if (pan_reject != HrtfBinauralRejectReason::None) {
    const HrtfBinauralRejectReason ir_reject = map_ir_reject_to_binaural(ir.reason);
    if (ir_reject != HrtfBinauralRejectReason::None) {
                                      float occlusion_gain, HrtfBinauralRejectReason expected) {

// --- deepen additive from deepen-b72-hrtf-reject-reasons-e7b1 ---
    preflight.hrtfDisabled = preflight.reason == HrtfPanPathRejectReason::HrtfDisabled;
    preflight.coLocated = preflight.reason == HrtfPanPathRejectReason::CoLocated;
        preflight.reason == HrtfAttenuationCouplingRejectReason::BypassPath;
        preflight.reason == HrtfAttenuationCouplingRejectReason::UnityAttenuation;
HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_pan(HrtfPanPathRejectReason reason) {
HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_ir(HrtfIrRejectReason reason) {
HrtfBinauralRejectReason hrtf_binaural_reject_reason_from_coupling(

// --- deepen additive from deepen-b72-hrtf-reject-reasons-5710 ---
    if (ir_reason != HrtfIrRejectReason::None) {
    if (coupling_reason == HrtfAttenuationCouplingRejectReason::BypassPath) {
    if (coupling_reason == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {

// --- deepen additive from deepen-b72-hrtf-reject-reason-preflights-d99c ---
    const HrtfPanPathRejectReason panReason = hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);
    preflight.rejected = preflight.reason != HrtfBinauralRejectReason::None;

// --- deepen additive from deepen-b72-hrtf-reject-reasons-a636 ---
bool hrtf_ir_reject_reason_is_blocking(HrtfIrRejectReason reason) {
    return reason != HrtfIrRejectReason::None;
    case HrtfPanPathRejectReason::EmptyIr:
bool hrtf_pan_path_reject_reason_is_blocking_spatial(HrtfPanPathRejectReason reason) {
    return reason == HrtfPanPathRejectReason::HrtfDisabled
        || reason == HrtfPanPathRejectReason::CoLocated;
bool hrtf_pan_path_reject_reason_is_blocking_convolution(HrtfPanPathRejectReason reason) {
    return reason != HrtfPanPathRejectReason::None;
HrtfPanPathRejectReason classify_hrtf_pan_path_spatial_reject(bool hrtf_enabled,
HrtfPanPathRejectReason classify_hrtf_pan_path_convolution_reject(bool hrtf_enabled,
    const HrtfPanPathRejectReason spatial_reject =
    if (spatial_reject != HrtfPanPathRejectReason::None) {
        return HrtfPanPathRejectReason::EmptyIr;
    preflight.spatialRejectReason = classify_hrtf_pan_path_spatial_reject(hrtf_enabled, rel_listener);
    preflight.convolutionRejectReason =
bool hrtf_attenuation_coupling_reject_reason_is_blocking(HrtfAttenuationCouplingRejectReason reason) {
    return reason != HrtfAttenuationCouplingRejectReason::None;
bool hrtf_binaural_reject_reason_is_blocking_spatial(HrtfBinauralRejectReason reason) {
    return reason == HrtfBinauralRejectReason::HrtfDisabled
        || reason == HrtfBinauralRejectReason::CoLocated;
bool hrtf_binaural_reject_reason_is_blocking_convolution(HrtfBinauralRejectReason reason) {
    return reason != HrtfBinauralRejectReason::None
        && reason != HrtfBinauralRejectReason::BypassPath
        && reason != HrtfBinauralRejectReason::UnityAttenuation;
bool hrtf_binaural_reject_reason_is_blocking_coupling(HrtfBinauralRejectReason reason) {
    return reason == HrtfBinauralRejectReason::BypassPath
        || reason == HrtfBinauralRejectReason::UnityAttenuation;
HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(bool hrtf_enabled,
HrtfBinauralRejectReason classify_hrtf_binaural_convolution_reject(bool hrtf_enabled,
HrtfBinauralRejectReason classify_hrtf_binaural_coupling_reject(HrtfPanPath path,
                                     HrtfBinauralRejectReason* reason) {
                                      float occlusion_gain, HrtfBinauralRejectReason* reason) {
    preflight.spatialRejectReason =
    preflight.couplingRejectReason = classify_hrtf_binaural_coupling_reject(

// --- deepen additive from b72-hrtf-reject-reason-preflights-6c04 ---
bool try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrPreflight& out, HrtfIrRejectReason& reason) {
    preflight.skipped = preflight.should_skip();
                                 HrtfPanPathPreflight& out, HrtfPanPathRejectReason& reason) {
    HrtfAttenuationCouplingPreflight& out, HrtfAttenuationCouplingRejectReason& reason,
                                      const Vec3& rel_listener, HrtfBinauralRejectReason expected) {
bool hrtf_binaural_reject_reason_is_bypass(HrtfBinauralRejectReason reason) {
bool hrtf_binaural_reject_reason_blocks_convolution(HrtfBinauralRejectReason reason) {
        || reason == HrtfBinauralRejectReason::EmptyIr
        || reason == HrtfBinauralRejectReason::MalformedIr;
                                 HrtfBinauralPreflight& out, HrtfBinauralRejectReason& reason,
    return !out.should_skip();

// --- deepen additive from b72-hrtf-reject-reason-preflights-d934 ---
const char* hrtfIrRejectReasonLabel(HrtfIrRejectReason reason) {
const char* hrtfPanPathRejectReasonLabel(HrtfPanPathRejectReason reason) {
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(bool hrtf_enabled, const HrtfIrStub& ir,
                                      const Vec3& rel_listener, HrtfPanPathRejectReason expected) {
const char* hrtfAttenuationCouplingRejectReasonLabel(HrtfAttenuationCouplingRejectReason reason) {
const char* hrtfBinauralRejectReasonLabel(HrtfBinauralRejectReason reason) {

// --- deepen additive from deepen-b72-hrtf-reject-reasons-4c1f ---
    preflight.malformedIr = preflight.reason == HrtfIrRejectReason::MalformedIr;
    preflight.emptyIr = preflight.reason != HrtfIrRejectReason::None;
    preflight.bypassPath = preflight.reason == HrtfAttenuationCouplingRejectReason::BypassPath;
    case HrtfBinauralRejectReason::PanBypassDisabled:
    case HrtfBinauralRejectReason::PanBypassCoLocated:
    case HrtfBinauralRejectReason::ConvolutionEmptyIr:
    case HrtfBinauralRejectReason::ConvolutionMalformedIr:
    case HrtfBinauralRejectReason::NarrowingBypassPath:
    case HrtfBinauralRejectReason::NarrowingUnityAttenuation:
bool is_blocking_hrtf_binaural_reject_reason(HrtfBinauralRejectReason reason) {
    return reason == HrtfBinauralRejectReason::PanBypassDisabled
        || reason == HrtfBinauralRejectReason::PanBypassCoLocated;
    const HrtfPanPathRejectReason panReject =
    if (panReject == HrtfPanPathRejectReason::HrtfDisabled) {
        return HrtfBinauralRejectReason::PanBypassDisabled;
    if (panReject == HrtfPanPathRejectReason::CoLocated) {
        return HrtfBinauralRejectReason::PanBypassCoLocated;
    const HrtfIrRejectReason irReject = classify_hrtf_ir_reject(ir);
    if (irReject == HrtfIrRejectReason::MalformedIr) {
        return HrtfBinauralRejectReason::ConvolutionMalformedIr;
    if (irReject == HrtfIrRejectReason::EmptyIr) {
        return HrtfBinauralRejectReason::ConvolutionEmptyIr;
    const HrtfAttenuationCouplingRejectReason narrowingReject =
    if (narrowingReject == HrtfAttenuationCouplingRejectReason::BypassPath) {
        return HrtfBinauralRejectReason::NarrowingBypassPath;
    if (narrowingReject == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {
        return HrtfBinauralRejectReason::NarrowingUnityAttenuation;

// --- deepen additive from deepen-b7-2-hrtf-reject-preflights-f8d6 ---
HrtfBinauralRejectReason map_pan_path_skip_reject_to_binaural(HrtfPanPathRejectReason reason) {
HrtfBinauralRejectReason map_pan_path_convolution_reject_to_binaural(HrtfPanPathRejectReason reason) {
    case HrtfPanPathRejectReason::MalformedIr:
bool hrtf_ir_preflight_rejects_for_reason(const HrtfIrPreflight& preflight, HrtfIrRejectReason expected) {
HrtfPanPathRejectReason classify_hrtf_pan_path_skip_reject(bool hrtf_enabled, const Vec3& rel_listener) {
    const HrtfPanPathRejectReason skip_reason = classify_hrtf_pan_path_skip_reject(hrtf_enabled, rel_listener);
    if (skip_reason != HrtfPanPathRejectReason::None) {
    const HrtfIrRejectReason ir_reason = classify_hrtf_ir_reject(ir);
        return HrtfPanPathRejectReason::MalformedIr;
bool hrtf_pan_path_preflight_skips_for_reason(const HrtfPanPathPreflight& preflight,
bool hrtf_pan_path_preflight_convolution_rejects_for_reason(const HrtfPanPathPreflight& preflight,
    return preflight.convolutionRejectReason == expected;
    const HrtfAttenuationCouplingPreflight& preflight, HrtfAttenuationCouplingRejectReason expected) {
HrtfBinauralRejectReason classify_hrtf_binaural_skip_reject(bool hrtf_enabled, const Vec3& rel_listener) {
HrtfBinauralRejectReason classify_hrtf_binaural_narrowing_reject(HrtfPanPath path,
    preflight.narrowingRejectReason = classify_hrtf_binaural_narrowing_reject(
bool hrtf_binaural_preflight_skips_for_reason(const HrtfBinauralPreflight& preflight,
bool hrtf_binaural_preflight_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,
bool hrtf_binaural_preflight_narrowing_rejects_for_reason(const HrtfBinauralPreflight& preflight,
    return preflight.narrowingRejectReason == expected;

// --- deepen additive from deepen-b72-hrtf-reject-reasons-2fef ---
bool is_blocking_hrtf_ir_reject_reason(HrtfIrRejectReason reason) {
HrtfIrPreflight try_preflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
HrtfPanPathRejectReason hrtf_pan_path_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,
bool is_blocking_hrtf_pan_path_reject_reason(HrtfPanPathRejectReason reason) {
HrtfPanPathPreflight try_preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
HrtfPanPathPreflight try_preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
bool is_blocking_hrtf_attenuation_coupling_reject_reason(HrtfAttenuationCouplingRejectReason reason) {
HrtfAttenuationCouplingPreflight try_preflight_hrtf_attenuation_coupling(
    HrtfAttenuationCouplingRejectReason& reason, const HrtfAttenuationCoupling& coupling,
bool is_blocking_hrtf_binaural_convolution_reject_reason(HrtfBinauralRejectReason reason) {
        || reason == HrtfBinauralRejectReason::EmptyIr;
HrtfBinauralPreflight try_preflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir,
                                                  float occlusion_gain, HrtfBinauralRejectReason& reason,
HrtfBinauralPreflight try_preflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-aa20 ---
bool hrtf_pan_path_reject_reason_is_blocking(HrtfPanPathRejectReason reason) {
                                             HrtfAttenuationCouplingRejectReason& reason) {
bool hrtf_binaural_reject_reason_is_blocking(HrtfBinauralRejectReason reason) {
    return reason != HrtfBinauralRejectReason::None;
    const HrtfPanPathRejectReason pan_reason = classify_hrtf_pan_path_reject(hrtf_enabled, rel_listener);
                             const BinauralPanParams& params, HrtfBinauralRejectReason* reason) {
                                 const BinauralPanParams& params, HrtfBinauralRejectReason& reason) {

// --- deepen additive from deepen-b72-hrtf-reject-reasons-e7e2 ---
    preflight.panRejectReason =
    if (preflight.panRejectReason != HrtfBinauralRejectReason::None) {
        preflight.convolutionRejectReason = preflight.panRejectReason;
        preflight.convolutionRejectReason = map_ir_reject_to_binaural(preflight.ir.reason);
    if (preflight.attenuationCoupling.reason == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {
        preflight.attenuationRejectReason = HrtfBinauralRejectReason::UnityAttenuation;
        preflight.attenuationRejectReason = HrtfBinauralRejectReason::None;
HrtfBinauralRejectReason classify_hrtf_binaural_pan_reject(bool hrtf_enabled,
    const HrtfBinauralRejectReason pan_reject = classify_hrtf_binaural_pan_reject(hrtf_enabled, rel_listener);
HrtfBinauralRejectReason classify_hrtf_binaural_attenuation_reject(
    if (reject == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {
bool hrtf_binaural_reject_reason_is_pan_blocking(HrtfBinauralRejectReason reason) {
bool hrtf_binaural_reject_reason_is_convolution_blocking(HrtfBinauralRejectReason reason) {

// --- deepen additive from deepen-b7-2-hrtf-guards-219a ---
HrtfBinauralRejectReason map_pan_path_reject(HrtfPanPathRejectReason reason) {
HrtfBinauralRejectReason map_ir_reject(HrtfIrRejectReason reason) {
HrtfBinauralRejectReason map_coupling_reject(HrtfAttenuationCouplingRejectReason reason) {
    preflight.panRejectReason = classify_hrtf_binaural_pan_reject(hrtf_enabled, rel_listener);
    reason = out.panRejectReason;

// --- deepen additive from deepen-b72-hrtf-reject-preflights-e16c ---
    return reason == HrtfIrRejectReason::None;
    return reason == HrtfPanPathRejectReason::None;
    return reason == HrtfAttenuationCouplingRejectReason::None;
HrtfBinauralRejectReason map_coupling_reject_to_binaural(
    if (preflight.spatialPanReject != HrtfBinauralRejectReason::None) {
HrtfBinauralRejectReason classify_hrtf_binaural_spatial_pan_reject(
bool try_preflight_hrtf_binaural_spatial_pan(bool hrtf_enabled, const HrtfIrStub& ir,
    return reason == HrtfBinauralRejectReason::None;
bool try_preflight_hrtf_binaural_convolution(bool hrtf_enabled, const HrtfIrStub& ir,
bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const HrtfIrStub& ir,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-494b ---
HrtfIrRejectReason classify_hrtf_ir_reject(const HrtfIrPreflight& preflight) {
bool hrtf_ir_reject_reason_blocks_convolution(HrtfIrRejectReason reason) {
bool preflight_hrtf_ir_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
HrtfPanPathRejectReason classify_hrtf_pan_path_reject(const HrtfPanPathPreflight& preflight) {
bool hrtf_pan_path_reject_reason_blocks_spatial_pan(HrtfPanPathRejectReason reason) {
                                   const Vec3& rel_listener, HrtfPanPathRejectReason* reason) {
HrtfBinauralConvolutionRejectReason classify_hrtf_binaural_convolution_reject(
        return HrtfBinauralConvolutionRejectReason::HrtfDisabled;
        return HrtfBinauralConvolutionRejectReason::CoLocated;
        return HrtfBinauralConvolutionRejectReason::MalformedIr;
        return HrtfBinauralConvolutionRejectReason::EmptyIr;
    return HrtfBinauralConvolutionRejectReason::None;
HrtfBinauralNarrowingRejectReason classify_hrtf_binaural_narrowing_reject(
        return HrtfBinauralNarrowingRejectReason::BypassPath;
        return HrtfBinauralNarrowingRejectReason::UnityAttenuation;
    return HrtfBinauralNarrowingRejectReason::None;
const char* hrtf_binaural_convolution_reject_reason_label(HrtfBinauralConvolutionRejectReason reason) {
    case HrtfBinauralConvolutionRejectReason::None:
    case HrtfBinauralConvolutionRejectReason::HrtfDisabled:
    case HrtfBinauralConvolutionRejectReason::CoLocated:
    case HrtfBinauralConvolutionRejectReason::MalformedIr:
    case HrtfBinauralConvolutionRejectReason::EmptyIr:
const char* hrtf_binaural_narrowing_reject_reason_label(HrtfBinauralNarrowingRejectReason reason) {
    case HrtfBinauralNarrowingRejectReason::None:
    case HrtfBinauralNarrowingRejectReason::BypassPath:
    case HrtfBinauralNarrowingRejectReason::UnityAttenuation:
bool hrtf_binaural_reject_reason_blocks_spatial_pan(HrtfBinauralRejectReason reason) {

// --- deepen additive from deepen-hrtf-b7-2-guards-64dd ---
    preflight.spatialPanRejectReason =
        preflight.convolutionRejectReason = HrtfBinauralRejectReason::None;
    } else if (preflight.spatialPanRejectReason != HrtfBinauralRejectReason::None) {
        preflight.convolutionRejectReason = preflight.spatialPanRejectReason;
        preflight.convolutionRejectReason = map_ir_reject_to_binaural(preflight.ir.rejectReason);
    return preflight.spatialPanRejectReason;
    return preflight.convolutionRejectReason;
    return preflight.narrowingRejectReason;
    const HrtfBinauralRejectReason reject = preflight.spatialPanRejectReason;

// --- deepen additive from deepen-b7.2-hrtf-reject-reasons-a8a9 ---
bool hrtfIrRejectReasonIsBlocking(HrtfIrRejectReason reason) {
HrtfIrRejectReason classifyHrtfIrReject(const HrtfIrStub& ir) {
bool tryPreflightHrtfIr(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
    reason = classifyHrtfIrReject(ir);
    return !hrtfIrRejectReasonIsBlocking(reason);
    preflight.rejectReason = classifyHrtfIrReject(ir);
bool hrtfPanPathRejectReasonIsBlocking(HrtfPanPathRejectReason reason) {
HrtfPanPathRejectReason classifyHrtfPanPathReject(bool hrtf_enabled, const Vec3& rel_listener) {
bool tryPreflightHrtfPanPath(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    reason = classifyHrtfPanPathReject(hrtf_enabled, rel_listener);
    return !hrtfPanPathRejectReasonIsBlocking(reason);
bool tryPreflightHrtfPanPath(bool hrtf_enabled, const Vec3& rel_listener,
    return tryPreflightHrtfPanPath(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);
    preflight.rejectReason = classifyHrtfPanPathReject(hrtf_enabled, rel_listener);
bool hrtfAttenuationCouplingRejectReasonIsBlocking(HrtfAttenuationCouplingRejectReason reason) {
HrtfAttenuationCouplingRejectReason classifyHrtfAttenuationCouplingReject(
bool tryPreflightHrtfAttenuationCoupling(HrtfPanPath path, float distance_attenuation,
    reason = classifyHrtfAttenuationCouplingReject(path, distance_attenuation, occlusion_gain,
    return !hrtfAttenuationCouplingRejectReasonIsBlocking(reason);
    preflight.rejectReason = classifyHrtfAttenuationCouplingReject(
bool hrtfBinauralRejectReasonIsBypass(HrtfBinauralRejectReason reason) {
bool hrtfBinauralRejectReasonBlocksConvolution(HrtfBinauralRejectReason reason) {
    return reason == HrtfBinauralRejectReason::EmptyIr
        || hrtfBinauralRejectReasonIsBypass(reason);
HrtfBinauralRejectReason classifyHrtfBinauralReject(
    const HrtfPanPathRejectReason pan_reject = classifyHrtfPanPathReject(hrtf_enabled, rel_listener);
    const HrtfIrRejectReason ir_reject = classifyHrtfIrReject(ir);
    if (ir_reject == HrtfIrRejectReason::MalformedIr) {
    if (ir_reject == HrtfIrRejectReason::NullSamples) {
    const HrtfAttenuationCouplingRejectReason atten_reject = classifyHrtfAttenuationCouplingReject(
    if (atten_reject == HrtfAttenuationCouplingRejectReason::UnityAttenuation) {
bool tryPreflightHrtfBinaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
    reason = classifyHrtfBinauralReject(hrtf_enabled, ir, rel_listener, distance_attenuation,
    return !hrtfBinauralRejectReasonIsBypass(reason);
bool tryPreflightHrtfBinaural(bool hrtf_enabled, const Vec3& rel_listener,
    return tryPreflightHrtfBinaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
    preflight.rejectReason = classifyHrtfBinauralReject(

// --- deepen additive from hrtf-b7-2-deepen-guards-653a ---
    preflight.reason = classifyHrtfIrReject(ir);
    const HrtfIrRejectReason reject = classifyHrtfIrReject(ir);
HrtfPanPathRejectReason classifyHrtfPanPathReject(bool hrtf_enabled, const HrtfIrStub& ir,
    if (ir_reject != HrtfIrRejectReason::None) {
    preflight.reason = classifyHrtfPanPathReject(hrtf_enabled, ir, rel_listener);
        classifyHrtfPanPathReject(hrtf_enabled, ir, rel_listener);
    return !hrtfPanPathRejectReasonIsBlocking(reject);
                                 const Vec3& rel_listener, HrtfPanPathRejectReason& reason) {
        classifyHrtfAttenuationCouplingReject(path, distance_attenuation, occlusion_gain);
bool hrtfBinauralRejectReasonIsBlocking(HrtfBinauralRejectReason reason) {
HrtfBinauralRejectReason classifyHrtfBinauralReject(const HrtfBinauralPreflight& preflight) {
    if (preflight.panPath.reason == HrtfPanPathRejectReason::HrtfDisabled) {
    if (preflight.panPath.reason == HrtfPanPathRejectReason::CoLocated) {
    if (preflight.panPath.reason == HrtfPanPathRejectReason::MalformedIr) {
    if (preflight.panPath.reason == HrtfPanPathRejectReason::EmptyIr) {
    preflight.reason = classifyHrtfBinauralReject(preflight);
    return !hrtfBinauralRejectReasonIsBlocking(preflight.reason);

// --- deepen additive from deepen-b72-hrtf-reject-reasons-a304 ---
    HrtfAttenuationCouplingRejectReason* reason, const HrtfAttenuationCoupling& coupling,
    const HrtfAttenuationCouplingPreflight preflight = preflight_hrtf_attenuation_coupling(
HrtfIrRejectReason classify_hrtf_binaural_convolution_reject(const HrtfBinauralPreflight& preflight) {
HrtfAttenuationCouplingRejectReason classify_hrtf_binaural_narrowing_reject(

// --- deepen additive from b72-hrtf-reject-reason-guards-3aaf ---
bool preflight_hrtf_ir_convolution_ready(const HrtfIrPreflight& preflight,
bool preflight_hrtf_ir_convolution_ready(const HrtfIrStub& ir, HrtfIrRejectReason* reason) {
bool try_preflight_hrtf_ir_convolution(const HrtfIrPreflight& preflight, HrtfIrRejectReason& reason) {
bool try_preflight_hrtf_ir_convolution(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
bool preflight_hrtf_pan_spatial_ready(const HrtfPanPathPreflight& preflight,
bool try_preflight_hrtf_pan_spatial(const HrtfPanPathPreflight& preflight,
bool try_preflight_hrtf_pan_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
bool try_preflight_hrtf_pan_spatial(bool hrtf_enabled, const Vec3& rel_listener,
bool should_skip_hrtf_pan_spatial(bool hrtf_enabled, const Vec3& rel_listener) {
    const HrtfAttenuationCouplingPreflight& preflight, HrtfAttenuationCouplingRejectReason* reason) {
bool try_preflight_hrtf_attenuation_narrowing(
    const HrtfAttenuationCouplingPreflight& preflight, HrtfAttenuationCouplingRejectReason& reason) {
bool try_preflight_hrtf_attenuation_narrowing(HrtfPanPath path, float distance_attenuation,
bool should_skip_hrtf_attenuation_narrowing(HrtfPanPath path, float distance_attenuation,
HrtfBinauralRejectReason classify_hrtf_binaural_spatial_reject(const HrtfBinauralPreflight& preflight) {
bool preflight_hrtf_binaural_spatial_ready(const HrtfBinauralPreflight& preflight,
                                           const Vec3& rel_listener, HrtfBinauralRejectReason* reason) {
bool try_preflight_hrtf_binaural_spatial(const HrtfBinauralPreflight& preflight,
bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const Vec3& rel_listener,
bool try_preflight_hrtf_binaural_spatial(bool hrtf_enabled, const HrtfIrStub& ir,
                                         const Vec3& rel_listener, HrtfBinauralRejectReason& reason) {
bool preflight_hrtf_binaural_convolution_ready(const HrtfBinauralPreflight& preflight,
bool try_preflight_hrtf_binaural_convolution(const HrtfBinauralPreflight& preflight,
bool preflight_hrtf_binaural_narrowing_ready(const HrtfBinauralPreflight& preflight,
bool try_preflight_hrtf_binaural_narrowing(const HrtfBinauralPreflight& preflight,
bool try_preflight_hrtf_binaural_narrowing(bool hrtf_enabled, const Vec3& rel_listener,

// --- deepen additive from hrtf-reject-reason-enums-2d4f ---
const char* hrtf_convolution_reject_reason_name(HrtfConvolutionRejectReason reason) {
    case HrtfConvolutionRejectReason::None:
    case HrtfConvolutionRejectReason::HrtfDisabled:
    case HrtfConvolutionRejectReason::CoLocated:
    case HrtfConvolutionRejectReason::MalformedIr:
    case HrtfConvolutionRejectReason::NullSamples:
HrtfConvolutionRejectReason hrtf_convolution_reject_reason(bool hrtf_enabled, const HrtfIrStub& ir,
    const HrtfPanPathRejectReason bypass = hrtf_pan_path_reject_reason(hrtf_enabled, rel_listener);
    if (bypass == HrtfPanPathRejectReason::HrtfDisabled) {
        return HrtfConvolutionRejectReason::HrtfDisabled;
    if (bypass == HrtfPanPathRejectReason::CoLocated) {
        return HrtfConvolutionRejectReason::CoLocated;
    const HrtfIrRejectReason irReason = hrtf_ir_reject_reason(ir);
        return HrtfConvolutionRejectReason::MalformedIr;
        return HrtfConvolutionRejectReason::NullSamples;
        return HrtfConvolutionRejectReason::None;
                                         HrtfConvolutionRejectReason expected) {
HrtfConvolutionRejectReason hrtf_binaural_convolution_reject_reason(
bool hrtf_binaural_convolution_rejects_for_reason(const HrtfBinauralPreflight& preflight,

// --- deepen additive from deepen-b72-hrtf-reject-reasons-0e3e ---
bool tryPreflight_hrtf_ir(const HrtfIrStub& ir, HrtfIrRejectReason& reason) {
bool tryPreflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
bool tryPreflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener,
    return tryPreflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener, reason);
bool tryPreflight_hrtf_attenuation_coupling(
bool tryPreflight_hrtf_binaural(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener,
bool tryPreflight_hrtf_binaural(bool hrtf_enabled, const Vec3& rel_listener,
    return tryPreflight_hrtf_binaural(hrtf_enabled, make_empty_hrtf_ir(), rel_listener,
