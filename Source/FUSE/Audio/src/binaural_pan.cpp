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

HrtfIrPreflight preflight_hrtf_ir(const HrtfIrStub& ir) {
    HrtfIrPreflight result;
    result.null_samples = ir.samples == nullptr;
    result.zero_length = ir.length == 0;
    return result;
}

HrtfIrPreflight preflight_hrtf_ir(const float* samples, u32 length) {
    return preflight_hrtf_ir(HrtfIrStub{samples, length});
}

bool can_use_hrtf_ir_preflight(const HrtfIrStub& ir) {
    return preflight_hrtf_ir(ir).has_valid_ir();
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

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir,
                                             const Vec3& rel_listener) {
    HrtfPanPathPreflight result;
    result.ir_preflight = preflight_hrtf_ir(ir);
    result.hrtf_disabled = !hrtf_enabled;
    result.co_located = is_co_located_hrtf_source(rel_listener);
    result.empty_ir = result.ir_preflight.is_empty();
    result.path = resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener);
    return result;
}

HrtfPanPathPreflight preflight_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, make_empty_hrtf_ir(), rel_listener);
}

bool can_apply_hrtf_spatial_pan_preflight(bool hrtf_enabled, const HrtfIrStub& ir,
                                          const Vec3& rel_listener) {
    return preflight_hrtf_pan_path(hrtf_enabled, ir, rel_listener).can_apply_spatial_pan();
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
    return compute_binaural_pan_gains_for_path(
        resolve_hrtf_pan_path(hrtf_enabled, HrtfIrStub{}, rel_listener), rel_listener, params);
}

BinauralPanGains compute_binaural_pan_gains_guarded(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener,
                                                    const BinauralPanParams& params) {
    return compute_binaural_pan_gains_for_path(resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener),
                                               rel_listener, params);
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

HrtfAttenuationCouplingPreflight preflight_hrtf_attenuation_coupling(HrtfPanPath path,
                                                                     float distance_attenuation,
                                                                     float occlusion_gain) {
    HrtfAttenuationCouplingPreflight result;
    result.bypass_path = is_hrtf_pan_path_bypass(path);
    result.clamped_distance_attenuation = clamp_hrtf_attenuation(distance_attenuation);
    result.clamped_occlusion_gain = clamp_hrtf_attenuation(occlusion_gain);
    result.unity_attenuation =
        is_unity_hrtf_attenuation(distance_attenuation, occlusion_gain);
    result.will_narrow = should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain);
    return result;
}

bool should_narrow_hrtf_spatial_image_preflight(HrtfPanPath path, float distance_attenuation,
                                                float occlusion_gain) {
    return preflight_hrtf_attenuation_coupling(path, distance_attenuation, occlusion_gain)
        .should_narrow_spatial_image();
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

void apply_hrtf_distance_factor(BinauralPanGains& gains, float distance_attenuation,
                                const BinauralPanParams& params) {
    apply_spatial_blend(gains, compute_hrtf_distance_factor(distance_attenuation, params));
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
    apply_spatial_blend(gains,
                        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling,
                                                   params));
}

void apply_hrtf_attenuation_coupling_for_path(BinauralPanGains& gains, HrtfPanPath path,
                                              float distance_attenuation, float occlusion_gain,
                                              const HrtfAttenuationCoupling& coupling,
                                              const BinauralPanParams& params) {
    if (!should_narrow_hrtf_spatial_image(path, distance_attenuation, occlusion_gain)) {
        return;
    }
    apply_hrtf_attenuation_coupling(gains, distance_attenuation, occlusion_gain, coupling, params);
}

BinauralPanGains compute_binaural_pan_gains_coupled(bool hrtf_enabled, const Vec3& rel_listener,
                                                    float distance_attenuation,
                                                    float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling,
                                                    const BinauralPanParams& params) {
    return compute_binaural_pan_gains_coupled_for_path(
        resolve_hrtf_pan_path(hrtf_enabled, rel_listener), rel_listener, distance_attenuation,
        occlusion_gain, coupling, params);
}

BinauralPanGains compute_binaural_pan_gains_coupled(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener,
                                                    float distance_attenuation, float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling,
                                                    const BinauralPanParams& params) {
    return compute_binaural_pan_gains_coupled_for_path(
        resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener), rel_listener, distance_attenuation,
        occlusion_gain, coupling, params);
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

} // namespace fuse::audio
