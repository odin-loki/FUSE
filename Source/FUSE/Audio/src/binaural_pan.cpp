#include <fuse/audio/binaural_pan.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::audio {

bool has_hrtf_ir(const HrtfIrStub& ir) {
    return ir.samples != nullptr && ir.length > 0;
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

bool should_apply_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener) {
    return hrtf_enabled && rel_listener.length() >= 1e-5f;
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
    if (distance < 1e-5f) {
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

float compute_hrtf_distance_factor(float distance_attenuation,
                                   const BinauralPanParams& params) {
    const float atten = std::clamp(distance_attenuation, 0.f, 1.f);
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
    const float distance_blend = compute_hrtf_distance_factor(distance_attenuation, params);
    const float occlusion_blend = compute_hrtf_distance_factor(std::clamp(occlusion_gain, 0.f, 1.f),
                                                               params);
    const float weight = std::clamp(coupling.occlusion_weight, 0.f, 1.f);
    return distance_blend * (1.f - weight) + occlusion_blend * weight;
}

void apply_hrtf_attenuation_coupling(BinauralPanGains& gains, float distance_attenuation,
                                     float occlusion_gain, const HrtfAttenuationCoupling& coupling,
                                     const BinauralPanParams& params) {
    apply_spatial_blend(gains,
                        compute_hrtf_spatial_blend(distance_attenuation, occlusion_gain, coupling,
                                                   params));
}

BinauralPanGains compute_binaural_pan_gains_coupled(bool hrtf_enabled, const Vec3& rel_listener,
                                                    float distance_attenuation,
                                                    float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling,
                                                    const BinauralPanParams& params) {
    BinauralPanGains pan = compute_binaural_pan_gains_guarded(hrtf_enabled, rel_listener, params);
    if (should_apply_hrtf_pan(hrtf_enabled, rel_listener)) {
        apply_hrtf_attenuation_coupling(pan, distance_attenuation, occlusion_gain, coupling, params);
    }
    return pan;
}

BinauralPanGains compute_binaural_pan_gains_coupled(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener,
                                                    float distance_attenuation, float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling,
                                                    const BinauralPanParams& params) {
    BinauralPanGains pan =
        compute_binaural_pan_gains_guarded(hrtf_enabled, ir, rel_listener, params);
    if (resolve_hrtf_pan_path(hrtf_enabled, ir, rel_listener) != HrtfPanPath::Bypass) {
        apply_hrtf_attenuation_coupling(pan, distance_attenuation, occlusion_gain, coupling, params);
    }
    return pan;
}

} // namespace fuse::audio
