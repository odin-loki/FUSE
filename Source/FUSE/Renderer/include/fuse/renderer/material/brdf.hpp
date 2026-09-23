#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// CPU reference of shaders/common/brdf.glsl (B5.3): GGX D, height-correlated Smith visibility,
/// Schlick Fresnel, Lambert diffuse, metallic workflow with F0 = mix(0.04, albedo, metallic).
/// Roughness is perceptual; alpha = roughness^2.
struct Brdf {
    static constexpr f32 kPi = 3.14159265358979f;
    static constexpr f32 kDielectricF0 = 0.04f;
    /// Perceptual roughness floor — keeps D finite for mirror-like inputs.
    static constexpr f32 kMinRoughness = 0.045f;

    static f32 clampRoughness(f32 roughness);

    /// GGX / Trowbridge-Reitz normal distribution.
    static f32 dGgx(f32 NoH, f32 roughness);
    /// Height-correlated Smith visibility V = G2 / (4 NoV NoL) — already contains the
    /// Cook-Torrance denominator, so specular = D * V * F.
    static f32 visSmithGgxCorrelated(f32 NoV, f32 NoL, f32 roughness);
    static fuse::math::Vec3 fSchlick(f32 VoH, const fuse::math::Vec3& f0);
    static fuse::math::Vec3 specularF0(const fuse::math::Vec3& albedo, f32 metallic);

    /// BRDF value f(V, L) (no cosine, no light), in 1/sr.
    static fuse::math::Vec3 evaluate(const fuse::math::Vec3& albedo,
                                     f32 roughness,
                                     f32 metallic,
                                     const fuse::math::Vec3& N,
                                     const fuse::math::Vec3& V,
                                     const fuse::math::Vec3& L);

    /// Outgoing radiance for a unit-irradiance directional light: f(V, L) * NoL.
    /// Mirrors `brdf_evaluate` with light_color = 1 and light_intensity = 1.
    static fuse::math::Vec3 shade(const fuse::math::Vec3& albedo,
                                  f32 roughness,
                                  f32 metallic,
                                  const fuse::math::Vec3& N,
                                  const fuse::math::Vec3& V,
                                  const fuse::math::Vec3& L);
};

} // namespace fuse::renderer
