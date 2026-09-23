#include <fuse/renderer/ssfx/hbao.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fuse::renderer {
namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kTwoPi = 2.f * kPi;

f32 saturate(f32 v) {
    return std::max(0.f, std::min(1.f, v));
}

void tangentBasis(const math::Vec3& n, math::Vec3& t, math::Vec3& b) {
    const math::Vec3 helper = std::fabs(n.x) < 0.9f ? math::Vec3{1.f, 0.f, 0.f} : math::Vec3{0.f, 1.f, 0.f};
    t = math::cross(helper, n).normalized();
    b = math::cross(n, t);
}

/// Normal oriented towards the camera (view-space origin).
math::Vec3 facingNormal(const SsfxGBufferView& view, u32 x, u32 y, const math::Vec3& p) {
    math::Vec3 n = view.normalAt(x, y).normalized();
    if (n.dot(p) > 0.f) {
        n = n * -1.f;
    }
    return n;
}

} // namespace

HbaoParams clampHbaoParams(const HbaoParams& raw) {
    HbaoParams p = raw;
    p.radius = std::max(1e-4f, raw.radius);
    p.bias = std::max(0.f, std::min(0.95f, raw.bias));
    p.directions = std::max(1u, std::min(64u, raw.directions));
    p.steps_per_dir = std::max(1u, std::min(256u, raw.steps_per_dir));
    p.strength = std::max(0.f, raw.strength);
    p.max_radius_px = std::max(1.f, raw.max_radius_px);
    return p;
}

f32 hbaoPixelVisibility(const SsfxGBufferView& view, const HbaoParams& rawParams, u32 x, u32 y) {
    if (!view.valid() || x >= view.camera.width || y >= view.camera.height || view.depthAt(x, y) <= 0.f) {
        return 1.f;
    }
    const HbaoParams params = clampHbaoParams(rawParams);
    const SsfxCamera& cam = view.camera;
    const math::Vec3 p = view.positionAt(x, y);
    const math::Vec3 n = facingNormal(view, x, y, p);
    const math::Vec3 viewVec = (p * -1.f).normalized();

    const f32 radiusPx = std::min(params.max_radius_px, params.radius * cam.fx / p.z);
    if (radiusPx < 1.f) {
        return 1.f;
    }
    const u32 steps = std::max(1u, std::min(params.steps_per_dir, static_cast<u32>(radiusPx)));
    const f32 cxp = static_cast<f32>(x) + 0.5f;
    const f32 cyp = static_cast<f32>(y) + 0.5f;

    // Basis perpendicular to the view vector — measures the angle of each view-aligned slice.
    math::Vec3 viewT{};
    math::Vec3 viewB{};
    tangentBasis(viewVec, viewT, viewB);

    // Each slice is two-sided (screen direction and its opposite), so `directions` horizon searches make
    // `directions / 2` slices.
    const u32 sliceCount = std::max(1u, params.directions / 2u);
    struct Slice {
        f32 angle = 0.f;
        f32 visibility = 1.f;
    };
    std::vector<Slice> slices(sliceCount);

    for (u32 sIdx = 0u; sIdx < sliceCount; ++sIdx) {
        const f32 phi = kPi * (static_cast<f32>(sIdx) + 0.5f) / static_cast<f32>(sliceCount);
        const f32 dx = std::cos(phi);
        const f32 dy = std::sin(phi);

        // Slice plane spanned by the view vector and the screen direction (view +Y is screen-down).
        const math::Vec3 direction{dx, dy, 0.f};
        const math::Vec3 ortho = (direction - viewVec * direction.dot(viewVec)).normalized();
        const math::Vec3 axis = math::cross(ortho, viewVec).normalized();
        const math::Vec3 projN = n - axis * n.dot(axis);
        const f32 projNLen = projN.length();
        slices[sIdx].angle = std::atan2(ortho.dot(viewB), ortho.dot(viewT));
        if (projNLen < 1e-6f) {
            slices[sIdx].visibility = 0.f;
            continue;
        }
        const f32 signN = ortho.dot(projN) >= 0.f ? 1.f : -1.f;
        const f32 cosN = saturate(projN.dot(viewVec) / projNLen);
        const f32 gamma = signN * std::acos(cosN);

        // Horizon search on both sides; horizons start at the tangent plane (no occlusion).
        f32 horizonCos[2] = {std::cos(gamma + 0.5f * kPi), std::cos(gamma - 0.5f * kPi)};
        for (u32 side = 0u; side < 2u; ++side) {
            const f32 sdx = side == 0u ? dx : -dx;
            const f32 sdy = side == 0u ? dy : -dy;
            i32 lastX = static_cast<i32>(x);
            i32 lastY = static_cast<i32>(y);
            for (u32 s = 1u; s <= steps; ++s) {
                const f32 t = radiusPx * static_cast<f32>(s) / static_cast<f32>(steps);
                const f32 sx = cxp + sdx * t;
                const f32 sy = cyp + sdy * t;
                if (!cam.inside(sx, sy)) {
                    break;
                }
                const i32 ix = static_cast<i32>(sx);
                const i32 iy = static_cast<i32>(sy);
                if (ix == lastX && iy == lastY) {
                    continue;
                }
                lastX = ix;
                lastY = iy;
                const u32 ux = static_cast<u32>(ix);
                const u32 uy = static_cast<u32>(iy);
                if (view.depthAt(ux, uy) <= 0.f) {
                    continue;
                }
                const math::Vec3 h = view.positionAt(ux, uy) - p;
                const f32 dist = h.length();
                if (dist < 1e-6f || dist > params.radius) {
                    continue;
                }
                const math::Vec3 hn = h * (1.f / dist);
                // Angle bias: ignore samples within asin(bias) of the tangent plane (self-occlusion guard).
                if (hn.dot(n) <= params.bias) {
                    continue;
                }
                horizonCos[side] = std::max(horizonCos[side], hn.dot(viewVec));
            }
        }

        // Exact cosine-weighted visibility of the slice between the two horizons (projected-normal form).
        f32 h1 = std::acos(std::max(-1.f, std::min(1.f, horizonCos[0])));
        f32 h0 = -std::acos(std::max(-1.f, std::min(1.f, horizonCos[1])));
        h0 = gamma + std::max(-0.5f * kPi, std::min(0.5f * kPi, h0 - gamma));
        h1 = gamma + std::max(-0.5f * kPi, std::min(0.5f * kPi, h1 - gamma));
        const f32 sinG = std::sin(gamma);
        const f32 arc0 = 0.25f * (cosN + 2.f * h0 * sinG - std::cos(2.f * h0 - gamma));
        const f32 arc1 = 0.25f * (cosN + 2.f * h1 * sinG - std::cos(2.f * h1 - gamma));
        slices[sIdx].visibility = projNLen * (arc0 + arc1);
    }

    f32 visibility = 0.f;
    if (params.azimuth_weighting && sliceCount > 1u) {
        // Weight each slice by the angle (around the view vector, period pi) it represents.
        for (Slice& slice : slices) {
            slice.angle = std::fmod(slice.angle + 2.f * kPi, kPi);
        }
        std::sort(slices.begin(), slices.end(), [](const Slice& a, const Slice& b) { return a.angle < b.angle; });
        f32 totalWeight = 0.f;
        for (u32 i = 0u; i < sliceCount; ++i) {
            const f32 prev = slices[(i + sliceCount - 1u) % sliceCount].angle;
            const f32 next = slices[(i + 1u) % sliceCount].angle;
            f32 gap = next - prev;
            if (gap <= 0.f) {
                gap += kPi;
            }
            visibility += 0.5f * gap * slices[i].visibility;
            totalWeight += 0.5f * gap;
        }
        visibility = totalWeight > 0.f ? visibility / totalWeight : 1.f;
    } else {
        for (const Slice& slice : slices) {
            visibility += slice.visibility;
        }
        visibility /= static_cast<f32>(sliceCount);
    }

    visibility = saturate(visibility);
    return params.strength == 1.f ? visibility : std::pow(visibility, params.strength);
}

bool computeHbaoCpu(const SsfxGBufferView& view, const HbaoParams& params, f32* visibilityOut) {
    if (!view.valid() || visibilityOut == nullptr) {
        return false;
    }
    for (u32 y = 0u; y < view.camera.height; ++y) {
        for (u32 x = 0u; x < view.camera.width; ++x) {
            visibilityOut[view.index(x, y)] = hbaoPixelVisibility(view, params, x, y);
        }
    }
    return true;
}

f32 ssaoHemisphereReferenceVisibility(const SsfxGBufferView& view, u32 x, u32 y, f32 radius, u32 sampleSqrt,
                                      u32 marchSteps) {
    if (!view.valid() || x >= view.camera.width || y >= view.camera.height || view.depthAt(x, y) <= 0.f ||
        sampleSqrt == 0u || marchSteps == 0u || radius <= 0.f) {
        return 1.f;
    }
    const SsfxCamera& cam = view.camera;
    const math::Vec3 p = view.positionAt(x, y);
    const math::Vec3 n = facingNormal(view, x, y, p);
    math::Vec3 tangent{};
    math::Vec3 bitangent{};
    tangentBasis(n, tangent, bitangent);

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

} // namespace fuse::renderer
