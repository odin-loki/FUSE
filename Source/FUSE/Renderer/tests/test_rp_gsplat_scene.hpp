#pragma once
// WP-9.2 test scenes shared by the CPU and Lavapipe gates (test_rp_gsplat_cpu.cpp, test_rp_gsplat.cpp):
// deterministic random splat clouds, orbit cameras and an occluder depth image in the visibility buffer's
// forward z/w convention.

#include <fuse/renderer/gsplat/gsplat_ply.hpp>
#include <fuse/renderer/gsplat/gsplat_types.hpp>

#include <cmath>
#include <vector>

namespace gs_test {

using fuse::f32;
using fuse::u32;
using namespace fuse::renderer::gsplat;

struct Lcg {
    u32 state = 1u;
    f32 next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<f32>(state >> 8) * (1.f / 16777216.f);
    }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * next(); }
};

/// `count` splats in the box [-1.2, 1.2] x [-0.9, 0.9] x [-1, 1]: sizes 0.015 .. 0.12 (anisotropic), random
/// unit quaternions, opacity 0.15 .. 0.95, SH of `degree` (DC colour + small higher bands).
inline GsAsset makeScene(u32 count, u32 degree, u32 seed) {
    GsAsset a;
    a.shDegree = degree;
    a.splats.resize(count);
    Lcg rng{seed * 747796405u + 2891336453u};
    const u32 coeffs = (degree + 1u) * (degree + 1u);
    for (GsSplat& s : a.splats) {
        s.position[0] = rng.range(-1.2f, 1.2f);
        s.position[1] = rng.range(-0.9f, 0.9f);
        s.position[2] = rng.range(-1.f, 1.f);
        for (f32& v : s.scale) {
            v = rng.range(0.015f, 0.12f);
        }
        f32 q[4];
        f32 len = 0.f;
        for (f32& v : q) {
            v = rng.range(-1.f, 1.f);
            len += v * v;
        }
        len = std::sqrt(len) + 1e-6f;
        for (u32 i = 0; i < 4u; ++i) {
            s.rotation[i] = q[i] / len;
        }
        s.opacity = rng.range(0.15f, 0.95f);
        for (u32 c = 0; c < 3u; ++c) {
            s.sh[c] = rng.range(-1.2f, 1.2f);
        }
        for (u32 k = 1; k < coeffs; ++k) {
            for (u32 c = 0; c < 3u; ++c) {
                s.sh[k * 3u + c] = rng.range(-0.25f, 0.25f);
            }
        }
    }
    return a;
}

inline constexpr f32 kNear = 0.1f;
inline constexpr f32 kFar = 100.f;
inline constexpr f32 kOrbit = 4.f;

/// Orbit camera at distance kOrbit around the origin (angle about +Y), 50 degree vertical field of view.
inline GsCamera orbitCamera(u32 w, u32 h, f32 angle) {
    const f32 eye[3] = {kOrbit * std::sin(angle), 0.3f, -kOrbit * std::cos(angle)};
    const f32 target[3] = {0.f, 0.f, 0.f};
    const f32 up[3] = {0.f, 1.f, 0.f};
    return gs_camera_look_at(eye, target, up, 50.f * 3.14159265f / 180.f, w, h, kNear, kFar);
}

/// Visibility-buffer depth: the left `fraction` of the image is a view-aligned occluder at view z = `z`
/// (forward z/w of the camera), the rest is empty (1.0).
inline std::vector<f32> occluderDepth(const GsCamera& camera, u32 w, u32 h, f32 z, f32 fraction) {
    std::vector<f32> d(static_cast<size_t>(w) * h, 1.f);
    const f32 depth = camera.depthA + camera.depthB / z;
    const u32 edge = static_cast<u32>(static_cast<f32>(w) * fraction);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < edge; ++x) {
            d[static_cast<size_t>(y) * w + x] = depth;
        }
    }
    return d;
}

} // namespace gs_test
