#include <fuse/ssfx/ssgi.hpp>

#include <fuse/ssfx/ssr.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fuse::ssfx {
namespace {

constexpr f32 kTwoPi = 6.28318530717958647692f;

SsrParams traceParams(const SsgiParams& params) {
    SsrParams trace{};
    trace.max_steps = params.max_steps;
    trace.stride_px = params.stride_px;
    trace.thickness = params.thickness;
    trace.max_distance = params.max_distance;
    trace.fade_screen_edge = 0.f;
    trace.refine_steps = params.refine_steps;
    return trace;
}

/// Integer hash (lowbias32) -> uniform [0, 1).
f32 hashUnit(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return static_cast<f32>(v >> 8) * (1.f / 16777216.f);
}

} // namespace

math::Vec3 ssgiSampleDirection(const math::Vec3& n, u32 x, u32 y, u32 i, u32 j, u32 sampleSqrt) {
    const u32 k = std::max(1u, sampleSqrt);
    const u32 seed = (x * 73856093U) ^ (y * 19349663U) ^ ((i * k + j) * 83492791U);
    const f32 u1 = (static_cast<f32>(i) + hashUnit(seed)) / static_cast<f32>(k);
    const f32 u2 = (static_cast<f32>(j) + hashUnit(seed ^ 0x9e3779b9U)) / static_cast<f32>(k);
    const math::Vec3 helper = std::fabs(n.x) < 0.9f ? math::Vec3{1.f, 0.f, 0.f} : math::Vec3{0.f, 1.f, 0.f};
    const math::Vec3 tangent = math::cross(helper, n).normalized();
    const math::Vec3 bitangent = math::cross(n, tangent);
    const f32 r = std::sqrt(u1);
    const f32 phi = kTwoPi * u2;
    const f32 lz = std::sqrt(std::max(0.f, 1.f - u1));
    return tangent * (r * std::cos(phi)) + bitangent * (r * std::sin(phi)) + n * lz;
}

SsgiParams clampSsgiParams(const SsgiParams& raw) {
    SsgiParams p = raw;
    p.sample_sqrt = std::max(1u, std::min(64u, raw.sample_sqrt));
    p.bounces = std::min(8u, raw.bounces);
    p.max_steps = std::max(1u, std::min(4096u, raw.max_steps));
    p.stride_px = std::max(0.25f, raw.stride_px);
    p.refine_steps = std::min(32u, raw.refine_steps);
    p.thickness = std::max(0.f, raw.thickness);
    p.max_distance = std::max(1e-3f, raw.max_distance);
    p.intensity = std::max(0.f, raw.intensity);
    return p;
}

math::Vec3 ssgiPixelGather(const SsfxGBufferView& view, const math::Vec3* radiance, const SsgiParams& rawParams,
                           u32 x, u32 y) {
    if (!view.valid() || radiance == nullptr || x >= view.camera.width || y >= view.camera.height ||
        view.depthAt(x, y) <= 0.f) {
        return {};
    }
    const SsgiParams params = clampSsgiParams(rawParams);
    const SsrParams trace = traceParams(params);
    const math::Vec3 p = view.positionAt(x, y);
    math::Vec3 n = view.normalAt(x, y).normalized();
    if (n.dot(p) > 0.f) {
        n = n * -1.f;
    }

    math::Vec3 sum{};
    const u32 total = params.sample_sqrt * params.sample_sqrt;
    for (u32 i = 0u; i < params.sample_sqrt; ++i) {
        for (u32 j = 0u; j < params.sample_sqrt; ++j) {
            // Cosine-weighted directions: the estimator of E / pi is the plain mean of the hit radiance.
            const math::Vec3 dir = ssgiSampleDirection(n, x, y, i, j, params.sample_sqrt);
            const SsrHit hit = ssrTraceRay(view, radiance, trace, x, y, dir);
            if (!hit.hit) {
                continue;
            }
            const u32 hx = std::min(view.camera.width - 1u, static_cast<u32>(std::max(0.f, hit.px)));
            const u32 hy = std::min(view.camera.height - 1u, static_cast<u32>(std::max(0.f, hit.py)));
            // Only the front of the hit surface emits towards the ray origin.
            math::Vec3 hitN = view.normalAt(hx, hy).normalized();
            if (hitN.dot(view.positionAt(hx, hy)) > 0.f) {
                hitN = hitN * -1.f;
            }
            if (hitN.dot(dir) >= 0.f) {
                continue;
            }
            sum = sum + hit.color;
        }
    }
    return sum * (1.f / static_cast<f32>(total));
}

bool computeSsgiCpu(const SsfxGBufferView& view, const math::Vec3* directRadiance, const math::Vec3* albedo,
                    const SsgiParams& rawParams, math::Vec3* indirectOut) {
    if (!view.valid() || directRadiance == nullptr || indirectOut == nullptr) {
        return false;
    }
    const SsgiParams params = clampSsgiParams(rawParams);
    const u32 count = view.camera.width * view.camera.height;
    std::fill(indirectOut, indirectOut + count, math::Vec3{});
    if (params.bounces == 0u) {
        return true;
    }

    std::vector<math::Vec3> radiance(directRadiance, directRadiance + count);
    std::vector<math::Vec3> indirect(count);
    for (u32 bounce = 0u; bounce < params.bounces; ++bounce) {
        for (u32 y = 0u; y < view.camera.height; ++y) {
            for (u32 x = 0u; x < view.camera.width; ++x) {
                const u32 idx = view.index(x, y);
                const math::Vec3 gathered = ssgiPixelGather(view, radiance.data(), params, x, y);
                const math::Vec3 a = albedo != nullptr ? albedo[idx] : math::Vec3{1.f, 1.f, 1.f};
                indirect[idx] = math::Vec3{a.x * gathered.x, a.y * gathered.y, a.z * gathered.z} * params.intensity;
            }
        }
        for (u32 i = 0u; i < count; ++i) {
            radiance[i] = directRadiance[i] + indirect[i];
        }
    }
    std::copy(indirect.begin(), indirect.end(), indirectOut);
    return true;
}

} // namespace fuse::ssfx
