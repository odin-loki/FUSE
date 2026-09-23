#include <fuse/renderer/material/brdf.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

f32 Brdf::clampRoughness(f32 roughness) {
    return std::clamp(roughness, kMinRoughness, 1.f);
}

f32 Brdf::dGgx(f32 NoH, f32 roughness) {
    const f32 a = clampRoughness(roughness) * clampRoughness(roughness);
    const f32 a2 = a * a;
    const f32 d = (NoH * a2 - NoH) * NoH + 1.f;
    return a2 / (kPi * d * d);
}

f32 Brdf::visSmithGgxCorrelated(f32 NoV, f32 NoL, f32 roughness) {
    const f32 a = clampRoughness(roughness) * clampRoughness(roughness);
    const f32 a2 = a * a;
    const f32 gv = NoL * std::sqrt(NoV * NoV * (1.f - a2) + a2);
    const f32 gl = NoV * std::sqrt(NoL * NoL * (1.f - a2) + a2);
    return 0.5f / std::max(gv + gl, 1e-5f);
}

fuse::math::Vec3 Brdf::fSchlick(f32 VoH, const fuse::math::Vec3& f0) {
    const f32 x = std::clamp(1.f - VoH, 0.f, 1.f);
    const f32 x2 = x * x;
    const f32 w = x2 * x2 * x;
    return {f0.x + (1.f - f0.x) * w, f0.y + (1.f - f0.y) * w, f0.z + (1.f - f0.z) * w};
}

fuse::math::Vec3 Brdf::specularF0(const fuse::math::Vec3& albedo, f32 metallic) {
    const f32 m = std::clamp(metallic, 0.f, 1.f);
    return {kDielectricF0 + (albedo.x - kDielectricF0) * m,
            kDielectricF0 + (albedo.y - kDielectricF0) * m,
            kDielectricF0 + (albedo.z - kDielectricF0) * m};
}

fuse::math::Vec3 Brdf::evaluate(const fuse::math::Vec3& albedo,
                                f32 roughness,
                                f32 metallic,
                                const fuse::math::Vec3& N,
                                const fuse::math::Vec3& V,
                                const fuse::math::Vec3& L) {
    const f32 NoL = N.dot(L);
    if (NoL <= 0.f) {
        return {};
    }
    const fuse::math::Vec3 H = (V + L).normalized();
    const f32 NoV = std::max(N.dot(V), 1e-4f);
    const f32 NoH = std::max(N.dot(H), 0.f);
    const f32 VoH = std::max(V.dot(H), 0.f);
    const f32 m = std::clamp(metallic, 0.f, 1.f);

    const fuse::math::Vec3 F = fSchlick(VoH, specularF0(albedo, m));
    const f32 DV = dGgx(NoH, roughness) * visSmithGgxCorrelated(NoV, NoL, roughness);
    const f32 kd = (1.f - m) / kPi;
    return {F.x * DV + (1.f - F.x) * kd * albedo.x,
            F.y * DV + (1.f - F.y) * kd * albedo.y,
            F.z * DV + (1.f - F.z) * kd * albedo.z};
}

fuse::math::Vec3 Brdf::shade(const fuse::math::Vec3& albedo,
                             f32 roughness,
                             f32 metallic,
                             const fuse::math::Vec3& N,
                             const fuse::math::Vec3& V,
                             const fuse::math::Vec3& L) {
    const f32 NoL = std::max(N.dot(L), 0.f);
    return evaluate(albedo, roughness, metallic, N, V, L) * NoL;
}

} // namespace fuse::renderer
