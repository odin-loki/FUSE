#include <fuse/ssfx/ssr.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::ssfx {
namespace {

f32 saturate(f32 v) {
    return std::max(0.f, std::min(1.f, v));
}

/// Nearest-pixel depth at continuous coordinates; 0 when off-screen or sky.
f32 depthNearest(const SsfxGBufferView& view, f32 px, f32 py) {
    if (!view.camera.inside(px, py)) {
        return 0.f;
    }
    return view.depthAt(static_cast<u32>(px), static_cast<u32>(py));
}

} // namespace

SsrParams clampSsrParams(const SsrParams& raw) {
    SsrParams p = raw;
    p.max_steps = std::max(1u, std::min(4096u, raw.max_steps));
    p.stride_px = std::max(0.25f, raw.stride_px);
    p.thickness = std::max(0.f, raw.thickness);
    p.max_distance = std::max(1e-3f, raw.max_distance);
    p.fade_screen_edge = std::max(0.f, std::min(0.5f, raw.fade_screen_edge));
    p.refine_steps = std::min(32u, raw.refine_steps);
    return p;
}

math::Vec3 ssrReflect(const math::Vec3& incident, const math::Vec3& n) {
    return incident - n * (2.f * incident.dot(n));
}

SsrHit ssrTracePixel(const SsfxGBufferView& view, const math::Vec3* sceneColor, const SsrParams& params, u32 x,
                     u32 y) {
    if (!view.valid() || sceneColor == nullptr || x >= view.camera.width || y >= view.camera.height ||
        view.depthAt(x, y) <= 0.f) {
        return SsrHit{};
    }
    const math::Vec3 p = view.positionAt(x, y);
    math::Vec3 n = view.normalAt(x, y).normalized();
    const math::Vec3 v = p.normalized();
    if (n.dot(v) > 0.f) {
        n = n * -1.f;
    }
    return ssrTraceRay(view, sceneColor, params, x, y, ssrReflect(v, n));
}

SsrHit ssrTraceRay(const SsfxGBufferView& view, const math::Vec3* sceneColor, const SsrParams& rawParams, u32 x,
                   u32 y, const math::Vec3& direction) {
    SsrHit result{};
    if (!view.valid() || sceneColor == nullptr || x >= view.camera.width || y >= view.camera.height ||
        view.depthAt(x, y) <= 0.f || direction.length() <= 0.f) {
        return result;
    }
    const SsrParams params = clampSsrParams(rawParams);
    const SsfxCamera& cam = view.camera;

    const math::Vec3 p = view.positionAt(x, y);
    const math::Vec3 r = direction.normalized();

    // Clip the ray against the near plane so both endpoints project.
    f32 rayLength = params.max_distance;
    if (r.z < 0.f) {
        const f32 maxLen = (p.z - cam.near_z * 1.01f) / -r.z;
        rayLength = std::min(rayLength, maxLen);
    }
    if (rayLength <= 1e-4f) {
        return result;
    }
    const math::Vec3 e = p + r * rayLength;

    f32 p0x = 0.f;
    f32 p0y = 0.f;
    f32 p1x = 0.f;
    f32 p1y = 0.f;
    if (!cam.project(p, p0x, p0y) || !cam.project(e, p1x, p1y)) {
        return result;
    }
    const f32 k0 = 1.f / p.z;
    const f32 k1 = 1.f / e.z;
    const math::Vec3 q0 = p * k0;
    const math::Vec3 q1 = e * k1;
    const f32 ddx = p1x - p0x;
    const f32 ddy = p1y - p0y;
    const f32 pixelLength = std::max(std::fabs(ddx), std::fabs(ddy));
    if (pixelLength < 1e-3f) {
        return result;
    }
    const f32 stride = std::max(params.stride_px, pixelLength / static_cast<f32>(params.max_steps));
    const u32 stepCount = static_cast<u32>(std::ceil(pixelLength / stride));
    const f32 fStep = stride / pixelLength;

    auto rayZAt = [&](f32 f) { return 1.f / (k0 + (k1 - k0) * f); };

    f32 prevF = 0.f;
    f32 prevZ = p.z;
    bool hit = false;
    f32 hitF = 0.f;
    for (u32 i = 1u; i <= stepCount; ++i) {
        const f32 f = std::min(1.f, static_cast<f32>(i) * fStep);
        const f32 sx = p0x + ddx * f;
        const f32 sy = p0y + ddy * f;
        result.steps_taken = i;
        if (!cam.inside(sx, sy)) {
            break;
        }
        const f32 rayZ = rayZAt(f);
        const f32 sceneZ = depthNearest(view, sx, sy);
        if (sceneZ > 0.f && !(static_cast<u32>(sx) == x && static_cast<u32>(sy) == y)) {
            const f32 zMin = std::min(prevZ, rayZ);
            const f32 zMax = std::max(prevZ, rayZ);
            if (zMax >= sceneZ && zMin <= sceneZ + params.thickness) {
                // Bisection between the last two samples for the first crossing.
                f32 lo = prevF;
                f32 hi = f;
                for (u32 k = 0u; k < params.refine_steps; ++k) {
                    const f32 mid = 0.5f * (lo + hi);
                    const f32 midSceneZ = depthNearest(view, p0x + ddx * mid, p0y + ddy * mid);
                    if (midSceneZ > 0.f && rayZAt(mid) >= midSceneZ) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                hit = true;
                hitF = hi;
                break;
            }
        }
        prevF = f;
        prevZ = rayZ;
    }
    if (!hit) {
        return result;
    }

    result.hit = true;
    result.px = p0x + ddx * hitF;
    result.py = p0y + ddy * hitF;
    const u32 hx = std::min(cam.width - 1u, static_cast<u32>(std::max(0.f, result.px)));
    const u32 hy = std::min(cam.height - 1u, static_cast<u32>(std::max(0.f, result.py)));
    result.color = sceneColor[view.index(hx, hy)];

    const f32 k = k0 + (k1 - k0) * hitF;
    const math::Vec3 hitPos = (q0 + (q1 - q0) * hitF) * (1.f / k);
    result.distance = (hitPos - p).length();

    f32 edgeFade = 1.f;
    if (params.fade_screen_edge > 0.f) {
        const f32 u = result.px / static_cast<f32>(cam.width);
        const f32 vv = result.py / static_cast<f32>(cam.height);
        const f32 edge = std::min(std::min(u, 1.f - u), std::min(vv, 1.f - vv));
        edgeFade = saturate(edge / params.fade_screen_edge);
    }
    const f32 distanceFade = saturate(4.f * (1.f - result.distance / params.max_distance));
    result.confidence = edgeFade * distanceFade;
    return result;
}

bool computeSsrCpu(const SsfxGBufferView& view, const math::Vec3* sceneColor, const SsrParams& params,
                   const f32* reflectiveMask, math::Vec3* colorOut, f32* confidenceOut) {
    if (!view.valid() || sceneColor == nullptr) {
        return false;
    }
    for (u32 y = 0u; y < view.camera.height; ++y) {
        for (u32 x = 0u; x < view.camera.width; ++x) {
            const u32 idx = view.index(x, y);
            SsrHit hit{};
            if (reflectiveMask == nullptr || reflectiveMask[idx] > 0.f) {
                hit = ssrTracePixel(view, sceneColor, params, x, y);
            }
            if (colorOut != nullptr) {
                colorOut[idx] = hit.hit ? hit.color : math::Vec3{};
            }
            if (confidenceOut != nullptr) {
                confidenceOut[idx] = hit.hit ? hit.confidence : 0.f;
            }
        }
    }
    return true;
}

} // namespace fuse::ssfx
