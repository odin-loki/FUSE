#pragma once

#include <fuse/audio/audio_components.hpp>
#include <fuse/audio/math.hpp>
#include <fuse/types.hpp>

namespace fuse::audio {

/// Stub HRTF impulse-response descriptor — empty IR falls back to ILD/ITD pan.
struct HrtfIrStub {
    const float* samples = nullptr;
    u32 length = 0;
};

/// True when an HRTF IR stub has non-null, non-empty sample data.
bool has_hrtf_ir(const HrtfIrStub& ir);

/// Alias for `has_hrtf_ir` — convolution path is available when true.
bool should_use_hrtf_ir(const HrtfIrStub& ir);

/// True when the IR stub is empty (null samples or zero length).
bool is_empty_hrtf_ir(const HrtfIrStub& ir);

/// Default-constructed empty IR stub.
HrtfIrStub make_empty_hrtf_ir();

/// HRTF pan routing — empty IR uses ILD/ITD stub; convolution deferred until IR wired.
enum class HrtfPanPath {
    Bypass,
    IldItdStub,
    Convolution,
};

/// Select pan path from HRTF enable flag, IR stub, and listener-local offset.
HrtfPanPath resolve_hrtf_pan_path(bool hrtf_enabled, const HrtfIrStub& ir, const Vec3& rel_listener);

/// Resolve pan path with an implicit empty IR stub.
HrtfPanPath resolve_hrtf_pan_path(bool hrtf_enabled, const Vec3& rel_listener);

/// True when the path produces spatial pan (not centre bypass).
bool is_hrtf_pan_path_spatial(HrtfPanPath path);

/// True when the path selects convolution (IR wired; stub gains until delay-line lands).
bool hrtf_pan_path_uses_convolution(HrtfPanPath path);

/// True when HRTF pan should run (enabled and source is not co-located).
bool should_apply_hrtf_pan(bool hrtf_enabled, const Vec3& rel_listener);

/// Stereo pan law used for ILD stub gains.
enum class PanLaw {
    EqualPower,
    Linear,
};

/// Per-channel gains from a pan position in [-1, 1] (left .. right).
struct PanLawGains {
    float left = 0.5f;
    float right = 0.5f;
};

/// Binaural pan stub parameters — ILD pan law + Woodworth ITD placeholder.
struct BinauralPanParams {
    PanLaw pan_law = PanLaw::EqualPower;
    float max_itd_seconds = 0.0007f;
    float max_ild_pan = 1.f;
    float elevation_rolloff = 0.15f;
    float min_spatial_blend = 0.25f;
};

/// Azimuth/elevation in listener-local space (radians).
struct BinauralPanAngles {
    float azimuth = 0.f;
    float elevation = 0.f;
};

/// Per-ear gain stubs plus ITD placeholder for future delay-line wiring.
struct BinauralPanGains {
    float left = 1.f;
    float right = 1.f;
    float itd_seconds = 0.f;
};

/// Azimuth/elevation from a listener-local offset (+Z forward, +X right).
BinauralPanAngles compute_binaural_angles(const Vec3& rel_listener);

/// Transform a world-space offset through `basis`, then compute angles.
BinauralPanAngles compute_binaural_angles(const Vec3& world_relative, const ListenerBasis& basis);

/// Clamp pan position to [-1, 1].
float clamp_pan_position(float pan);

/// Map azimuth (radians) to a pan position in [-1, 1] for ILD stub sampling.
float compute_pan_position_from_azimuth(float azimuth, float max_ild_pan = 1.f);

/// Woodworth ITD stub from listener-local azimuth (radians).
float compute_itd_from_azimuth(float azimuth, const BinauralPanParams& params = {});

/// Symmetric elevation rolloff factor applied to both ears.
float compute_elevation_factor(float elevation, const BinauralPanParams& params = {});

/// Sample L/R gains for a pan law at the endpoints and interior.
PanLawGains sample_pan_law(float pan, PanLaw law);

/// ILD stub via selected pan law; ITD stub scales with sin(azimuth).
BinauralPanGains compute_binaural_pan_gains(const BinauralPanAngles& angles,
                                             const BinauralPanParams& params = {});

/// Convenience — angles and gains from listener-local offset.
BinauralPanGains compute_binaural_pan_gains(const Vec3& rel_listener,
                                             const BinauralPanParams& params = {});

/// Convenience — world-space offset through `basis`, then compute gains.
BinauralPanGains compute_binaural_pan_gains(const Vec3& world_relative, const ListenerBasis& basis,
                                             const BinauralPanParams& params = {});

/// Build a safe listener basis from an audio listener component.
ListenerBasis compute_listener_basis(const AudioListener& listener);

/// Azimuth/elevation from listener and source world positions.
BinauralPanAngles compute_binaural_angles(const AudioListener& listener, const Vec3& source_position);

/// Binaural gains from listener and source world positions.
BinauralPanGains compute_binaural_pan_gains(const AudioListener& listener, const Vec3& source_position,
                                             const BinauralPanParams& params = {});

/// Mono centre fallback when HRTF pan is bypassed.
BinauralPanGains make_centre_binaural_pan_gains();

/// L/R pan spread in [0, 1] — 0 when centre-panned.
float compute_pan_spread(const BinauralPanGains& gains);

/// True when left and right gains are within \p epsilon of centre pan.
bool is_centre_panned(const BinauralPanGains& gains, float epsilon = 1e-3f);

/// One-shot binaural gains with empty-HRTF guards (disabled / co-located → centre).
BinauralPanGains compute_binaural_pan_gains_guarded(bool hrtf_enabled, const Vec3& rel_listener,
                                                    const BinauralPanParams& params = {});

/// IR-aware guarded pan — empty IR keeps ILD/ITD stub; bypass only when disabled/co-located.
BinauralPanGains compute_binaural_pan_gains_guarded(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener,
                                                    const BinauralPanParams& params = {});

/// Gains for a resolved HRTF pan path (bypass → centre mono).
BinauralPanGains compute_binaural_pan_gains_for_path(HrtfPanPath path, const Vec3& rel_listener,
                                                     const BinauralPanParams& params = {});

/// Resolve path then compute gains (pan path + empty-IR guards in one call).
BinauralPanGains compute_binaural_pan_gains_resolved(bool hrtf_enabled, const HrtfIrStub& ir,
                                                     const Vec3& rel_listener,
                                                     const BinauralPanParams& params = {});

/// Linear interpolation between two binaural pan gain states.
BinauralPanGains lerp_binaural_pan_gains(const BinauralPanGains& from, const BinauralPanGains& to,
                                         float t);

/// Clamp per-ear gains to [0, 1].
void clamp_binaural_pan_gains(BinauralPanGains& gains);

/// Narrow or widen L/R spread — blend 1 preserves image, 0 collapses to mono centre.
void apply_spatial_blend(BinauralPanGains& gains, float blend);

/// Attenuation coupling — blends distance and occlusion into spatial image narrowing.
struct HrtfAttenuationCoupling {
    float occlusion_weight = 0.5f;
};

/// Combined spatial blend from distance attenuation and occlusion LF gain.
float compute_hrtf_spatial_blend(float distance_attenuation, float occlusion_gain,
                                 const HrtfAttenuationCoupling& coupling = {},
                                 const BinauralPanParams& params = {});

/// Occlusion-only spatial blend factor (no distance attenuation).
float compute_hrtf_occlusion_blend(float occlusion_gain,
                                   const BinauralPanParams& params = {});

/// Apply occlusion-only spatial narrowing.
void apply_hrtf_occlusion_coupling(BinauralPanGains& gains, float occlusion_gain,
                                   const BinauralPanParams& params = {});

/// Apply distance + occlusion coupling to narrow the binaural image toward mono centre.
void apply_hrtf_attenuation_coupling(BinauralPanGains& gains, float distance_attenuation,
                                     float occlusion_gain,
                                     const HrtfAttenuationCoupling& coupling = {},
                                     const BinauralPanParams& params = {});

/// Narrow binaural image at distance — full separation when attenuation is unity.
float compute_hrtf_distance_factor(float distance_attenuation,
                                   const BinauralPanParams& params = {});

/// Apply distance-based spatial narrowing to L/R gains (mono centre preserved).
void apply_hrtf_distance_factor(BinauralPanGains& gains, float distance_attenuation,
                                const BinauralPanParams& params = {});

/// One-shot guarded pan with distance + occlusion attenuation coupling.
BinauralPanGains compute_binaural_pan_gains_coupled(bool hrtf_enabled, const Vec3& rel_listener,
                                                    float distance_attenuation, float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling = {},
                                                    const BinauralPanParams& params = {});

/// IR-aware one-shot guarded pan with attenuation coupling.
BinauralPanGains compute_binaural_pan_gains_coupled(bool hrtf_enabled, const HrtfIrStub& ir,
                                                    const Vec3& rel_listener,
                                                    float distance_attenuation, float occlusion_gain,
                                                    const HrtfAttenuationCoupling& coupling = {},
                                                    const BinauralPanParams& params = {});

/// Resolve path, compute gains, and apply attenuation coupling in one call.
BinauralPanGains compute_binaural_pan_gains_resolved_coupled(bool hrtf_enabled,
                                                             const HrtfIrStub& ir,
                                                             const Vec3& rel_listener,
                                                             float distance_attenuation,
                                                             float occlusion_gain,
                                                             const HrtfAttenuationCoupling& coupling = {},
                                                             const BinauralPanParams& params = {});

} // namespace fuse::audio
