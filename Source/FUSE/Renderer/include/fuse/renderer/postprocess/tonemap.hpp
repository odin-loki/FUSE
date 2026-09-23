#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Tone-map operator identifiers (B5.10 — P5 §5.10).
enum class ToneMapper : u8 {
    ACES = 0,
    Filmic = 1,
    Reinhard = 2,
    Neutral = 3,
};

/// CPU reference tone mapping — mirrors renderer/postprocess/tonemapping.cuh.
///
/// `aces_tonemap` is the per-channel rational ACES filmic fit
///   f(x) = x (2.51 x + 0.03) / (x (2.43 x + 0.59) + 0.14), clamped to [0, 1]
/// (the published Narkowicz 2015 fit of the ACES RRT + sRGB ODT; the P5 sketch calls it the "Hill
/// approximation" but these are the Narkowicz constants). Un-exposed it maps scene 0.18 to ~0.267
/// display-linear, so the grade/stack apply a mid-grey calibration exposure (see
/// `tone_mapper_mid_grey_calibration_ev`) that makes scene 0.18 land on display-linear 0.18.
fuse::math::Vec3 aces_tonemap(const fuse::math::Vec3& x);
fuse::math::Vec3 filmic_tonemap(const fuse::math::Vec3& x);
fuse::math::Vec3 reinhard_tonemap(const fuse::math::Vec3& x);

fuse::math::Vec3 apply_tone_map(const fuse::math::Vec3& hdr, ToneMapper mapper);
const char* tone_mapper_name(ToneMapper mapper);

/// Scene-referred mid grey and its display-referred (linear, pre-OETF) target.
inline constexpr f32 kSceneMidGrey = 0.18f;
inline constexpr f32 kDisplayMidGrey = 0.18f;

/// Exposure bias (EV stops) such that `apply_tone_map(scene_grey * 2^ev, mapper) == display_grey`
/// (monotone bisection on the operator). Neutral returns 0 when scene_grey == display_grey.
/// Returns 0 when the operator cannot reach `display_grey`.
f32 tone_mapper_mid_grey_calibration_ev(ToneMapper mapper, f32 scene_grey = kSceneMidGrey,
                                        f32 display_grey = kDisplayMidGrey);

/// Host-side tone-map pass stub (CUDA kernel deferred).
class ToneMap {
public:
    void setMapper(ToneMapper mapper) { m_mapper = mapper; }
    ToneMapper mapper() const { return m_mapper; }
    bool isReady() const { return m_ready; }
    void init();
    void destroy();

    fuse::math::Vec3 apply(const fuse::math::Vec3& hdr) const;

private:
    ToneMapper m_mapper = ToneMapper::ACES;
    bool m_ready = false;
};

} // namespace fuse::renderer
