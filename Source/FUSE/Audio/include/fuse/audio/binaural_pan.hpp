#pragma once

#include <fuse/audio/math.hpp>
#include <fuse/types.hpp>

namespace fuse::audio {

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

/// Clamp per-ear gains to [0, 1].
void clamp_binaural_pan_gains(BinauralPanGains& gains);

/// Narrow binaural image at distance — full separation when attenuation is unity.
float compute_hrtf_distance_factor(float distance_attenuation,
                                   const BinauralPanParams& params = {});

/// Apply distance-based spatial narrowing to L/R gains (mono centre preserved).
void apply_hrtf_distance_factor(BinauralPanGains& gains, float distance_attenuation,
                                const BinauralPanParams& params = {});

} // namespace fuse::audio
