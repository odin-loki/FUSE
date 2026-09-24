// WP-7.2 ReSTIR DI and GI: settings and frame constants (include/fuse/renderer/restir/restir.hpp).
#include <fuse/renderer/restir/restir.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace fuse::renderer::restir {

namespace {
f32 clampf(f32 v, f32 lo, f32 hi) {
    if (!(v >= lo)) {
        return lo;
    }
    return v > hi ? hi : v;
}
} // namespace

bool restirInvert(const f32 in[16], f64 out[16]) {
    f64 a[4][8];
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            a[r][c] = in[c * 4 + r];
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    }
    for (u32 c = 0; c < 4; ++c) {
        u32 piv = c;
        for (u32 r = c + 1; r < 4; ++r) {
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) {
                piv = r;
            }
        }
        if (std::fabs(a[piv][c]) < 1e-30) {
            return false;
        }
        for (u32 k = 0; k < 8; ++k) {
            std::swap(a[c][k], a[piv][k]);
        }
        const f64 d = a[c][c];
        for (u32 k = 0; k < 8; ++k) {
            a[c][k] /= d;
        }
        for (u32 r = 0; r < 4; ++r) {
            if (r != c) {
                const f64 f = a[r][c];
                for (u32 k = 0; k < 8; ++k) {
                    a[r][k] -= f * a[c][k];
                }
            }
        }
    }
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            out[c * 4 + r] = a[r][c + 4];
        }
    }
    return true;
}

RestirSettings restirSanitize(const RestirSettings& in) {
    RestirSettings s = in;
    s.diCandidates = std::clamp(s.diCandidates, 1u, kRestirMaxCandidates);
    s.diSpatialIterations = std::min(s.diSpatialIterations, kRestirMaxSpatial);
    s.giSpatialIterations = std::min(s.giSpatialIterations, kRestirMaxSpatial);
    s.diNeighbors = std::clamp(s.diNeighbors, 1u, kRestirMaxNeighbors);
    s.giNeighbors = std::clamp(s.giNeighbors, 1u, kRestirMaxNeighbors);
    s.diRadius = clampf(s.diRadius, 1.f, 256.f);
    s.giRadius = clampf(s.giRadius, 1.f, 256.f);
    s.diMCap = clampf(s.diMCap, 1.f, 1.0e6f);
    s.giMCap = clampf(s.giMCap, 1.f, 1.0e6f);
    s.giJacobianClamp = s.giJacobianClamp > 0.f ? clampf(s.giJacobianClamp, 1.f, 1.0e6f) : 0.f;
    s.normalThreshold = clampf(s.normalThreshold, -1.f, 1.f);
    s.depthThreshold = clampf(s.depthThreshold, 0.f, 1.0e6f);
    s.normalBias = clampf(s.normalBias, 0.f, 1.0e3f);
    s.viewBias = clampf(s.viewBias, 0.f, 1.f);
    s.farDistance = clampf(s.farDistance, 1.0e-3f, 1.0e30f);
    s.giRayTMin = clampf(s.giRayTMin, 0.f, s.farDistance);
    s.cullMask &= 0x7Fu; // rt::kRtMaskAll: never the dead-slot bit
    return s;
}

bool buildRestirFrameConstants(const RestirSettings& settingsIn, const RestirCamera& camera, const RestirFrameParams& p,
                               RestirFrameConstants& c) {
    c = RestirFrameConstants{};
    f64 inv[16];
    if (p.width == 0u || p.height == 0u || !restirInvert(camera.viewProj, inv)) {
        return false;
    }
    const RestirSettings s = restirSanitize(settingsIn);
    for (u32 i = 0; i < 16u; ++i) {
        c.invViewProj[i] = static_cast<f32>(inv[i]);
    }
    const f64 fx = camera.forward[0];
    const f64 fy = camera.forward[1];
    const f64 fz = camera.forward[2];
    const f64 fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (!(fl > 1e-20)) {
        return false;
    }
    for (u32 k = 0; k < 3u; ++k) {
        c.cameraPosition[k] = camera.position[k];
        c.cameraForward[k] = static_cast<f32>(camera.forward[k] / fl);
    }
    c.width = p.width;
    c.height = p.height;
    c.invWidth = 1.f / static_cast<f32>(p.width);
    c.invHeight = 1.f / static_cast<f32>(p.height);
    c.frameIndex = p.frameIndex;
    c.seed = p.seed;
    c.lightCount = p.lightCount;
    u32 flags = 0u;
    flags |= s.unbiased ? kRestirFlagUnbiased : 0u;
    flags |= p.history ? kRestirFlagHistory : 0u;
    flags |= s.visibilityReuse ? kRestirFlagVisibilityReuse : 0u;
    flags |= p.motion ? kRestirFlagMotion : 0u;
    flags |= (s.giShadeVisibility || s.unbiased) ? kRestirFlagGiShadeVisibility : 0u;
    c.flags = flags;
    c.diCandidates = s.diCandidates;
    c.diNeighbors = s.diNeighbors;
    c.giNeighbors = s.giNeighbors;
    c.diRadius = s.diRadius;
    c.giRadius = s.giRadius;
    c.diMCap = s.diMCap;
    c.giMCap = s.giMCap;
    c.normalThreshold = s.normalThreshold;
    c.depthThreshold = s.depthThreshold;
    c.normalBias = s.normalBias;
    c.viewBias = s.viewBias;
    c.farDistance = s.farDistance;
    c.giJacobianClamp = s.giJacobianClamp;
    c.giRayTMin = s.giRayTMin;
    c.cullMask = s.cullMask;
    return true;
}

RestirLight makeRestirLight(const light_tree::LightTreeLight& light, const f32 (&rgb)[3]) {
    RestirLight l{};
    for (u32 k = 0; k < 3u; ++k) {
        l.radiance[k] = std::max(rgb[k], 0.f);
    }
    l.kind = light.kind;
    if (light.kind == light_tree::kLtKindSpot) {
        l.cosInner = light.cosInner;
        l.cosOuter = light.cosOuter;
    }
    return l;
}

} // namespace fuse::renderer::restir
