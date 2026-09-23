#include <fuse/ssfx/hbao.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/ssfx/hbao_kernel.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::ssfx {
namespace {

constexpr f32 kTwoPi = 2.f * hbao_kernel::kPi;

} // namespace

HbaoParams clampHbaoParams(const HbaoParams& raw) {
    return hbao_kernel::clamp_params(raw);
}

f32 hbaoPixelVisibility(const SsfxGBufferView& view, const HbaoParams& rawParams, u32 x, u32 y) {
    return hbao_kernel::pixel_visibility(view, hbao_kernel::clamp_params(rawParams), x, y);
}

bool computeHbaoCpu(const SsfxGBufferView& view, const HbaoParams& params, f32* visibilityOut,
                    kernel::Backend backend) {
    if (!view.valid() || visibilityOut == nullptr) {
        return false;
    }
    return kernel::launch(backend, hbao_kernel::make_launch(view), hbao_kernel::Kernel{},
                          hbao_kernel::make_params(view, params, visibilityOut))
        .ok;
}

f32 ssaoHemisphereReferenceVisibility(const SsfxGBufferView& view, u32 x, u32 y, f32 radius, u32 sampleSqrt,
                                      u32 marchSteps) {
    if (!view.valid() || x >= view.camera.width || y >= view.camera.height || view.depthAt(x, y) <= 0.f ||
        sampleSqrt == 0u || marchSteps == 0u || radius <= 0.f) {
        return 1.f;
    }
    const SsfxCamera& cam = view.camera;
    const math::Vec3 p = view.positionAt(x, y);
    const math::Vec3 n = hbao_kernel::facing_normal(view, x, y, p);
    math::Vec3 tangent{};
    math::Vec3 bitangent{};
    hbao_kernel::tangent_basis(n, tangent, bitangent);

    u32 visible = 0u;
    const u32 total = sampleSqrt * sampleSqrt;
    for (u32 i = 0u; i < sampleSqrt; ++i) {
        for (u32 j = 0u; j < sampleSqrt; ++j) {
            // Stratified cosine-weighted direction (Malley's method).
            const f32 u1 = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(sampleSqrt);
            const f32 u2 = (static_cast<f32>(j) + 0.5f) / static_cast<f32>(sampleSqrt);
            const f32 r = std::sqrt(u1);
            const f32 phi = kTwoPi * u2;
            const f32 lz = std::sqrt(std::max(0.f, 1.f - u1));
            const math::Vec3 dir = tangent * (r * std::cos(phi)) + bitangent * (r * std::sin(phi)) + n * lz;

            bool occluded = false;
            for (u32 k = 1u; k <= marchSteps; ++k) {
                const math::Vec3 sample = p + dir * (radius * static_cast<f32>(k) / static_cast<f32>(marchSteps));
                f32 sx = 0.f;
                f32 sy = 0.f;
                if (!cam.project(sample, sx, sy) || !cam.inside(sx, sy)) {
                    break;
                }
                const f32 sceneZ = view.sampleDepthBilinear(sx, sy);
                if (sceneZ <= 0.f) {
                    continue;
                }
                if (sample.z > sceneZ * (1.f + 1e-4f) + 1e-5f) {
                    occluded = true;
                    break;
                }
            }
            if (!occluded) {
                ++visible;
            }
        }
    }
    return static_cast<f32>(visible) / static_cast<f32>(total);
}

} // namespace fuse::ssfx
