#pragma once

// WP-7.1 light tree gates: deterministic light lists and shading points shared by the CPU gates
// (test_rp_light_tree_cpu.cpp) and the Lavapipe gates (test_rp_light_tree.cpp).

#include <fuse/renderer/light_tree/light_tree.hpp>

#include <cmath>
#include <vector>

namespace lt_test {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using namespace fuse::renderer::light_tree;

/// xorshift64*: deterministic on every platform.
struct Rng {
    u64 state = 0x9E3779B97F4A7C15ull;
    explicit Rng(u64 seed) : state(seed * 0x9E3779B97F4A7C15ull + 1u) {}
    u64 next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }
    /// [0, 1) with 24 random bits (exactly representable).
    f32 uniform() { return static_cast<f32>(next() >> 40) * (1.f / 16777216.f); }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * uniform(); }
};

inline void unitVector(Rng& rng, f32 (&out)[3]) {
    for (;;) {
        const f32 x = rng.range(-1.f, 1.f);
        const f32 y = rng.range(-1.f, 1.f);
        const f32 z = rng.range(-1.f, 1.f);
        const f32 l2 = x * x + y * y + z * z;
        if (l2 > 0.01f && l2 <= 1.f) {
            const f32 inv = 1.f / std::sqrt(l2);
            out[0] = x * inv;
            out[1] = y * inv;
            out[2] = z * inv;
            return;
        }
    }
}

inline void cross(const f32 (&a)[3], const f32 (&b)[3], f32 (&out)[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/// Two half axes spanning the plane perpendicular to n, scaled.
inline void planeAxes(const f32 (&n)[3], f32 su, f32 sv, f32 (&u)[3], f32 (&v)[3]) {
    const f32 helper[3] = {std::fabs(n[0]) < 0.9f ? 1.f : 0.f, std::fabs(n[0]) < 0.9f ? 0.f : 1.f, 0.f};
    f32 t[3];
    cross(helper, n, t);
    const f32 inv = 1.f / std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    for (f32& c : t) {
        c *= inv;
    }
    f32 b[3];
    cross(n, t, b);
    for (u32 i = 0; i < 3u; ++i) {
        u[i] = t[i] * su;
        v[i] = b[i] * sv;
    }
}

/// Every kind: points, spots, rectangles, disks, triangles, directional lights, spread over a 20 m box.
inline std::vector<LightTreeLight> mixedScene(u64 seed, u32 scale = 1u) {
    Rng rng(seed);
    std::vector<LightTreeLight> lights;
    auto pos = [&](f32 (&p)[3]) {
        p[0] = rng.range(-10.f, 10.f);
        p[1] = rng.range(-10.f, 10.f);
        p[2] = rng.range(-10.f, 10.f);
    };
    for (u32 i = 0; i < 48u * scale; ++i) {
        f32 p[3];
        pos(p);
        lights.push_back(makePointLight(p, rng.range(0.1f, 10.f), i));
    }
    for (u32 i = 0; i < 24u * scale; ++i) {
        f32 p[3], d[3];
        pos(p);
        unitVector(rng, d);
        const f32 cosOuter = rng.range(0.3f, 0.95f);
        const f32 cosInner = cosOuter + (1.f - cosOuter) * rng.uniform();
        lights.push_back(makeSpotLight(p, d, cosInner, cosOuter, rng.range(0.1f, 20.f), 1000u + i));
    }
    for (u32 i = 0; i < 24u * scale; ++i) {
        f32 p[3], n[3], u[3], v[3];
        pos(p);
        unitVector(rng, n);
        planeAxes(n, rng.range(0.05f, 1.f), rng.range(0.05f, 1.f), u, v);
        const bool disk = (i % 2u) == 1u;
        lights.push_back(disk ? makeDiskLight(p, u, v, rng.range(0.5f, 5.f), (i % 3u) == 0u, 2000u + i)
                              : makeRectLight(p, u, v, rng.range(0.5f, 5.f), (i % 3u) == 0u, 2000u + i));
    }
    for (u32 i = 0; i < 400u * scale; ++i) {
        f32 c[3], a[3], b[3], d[3];
        pos(c);
        unitVector(rng, a);
        unitVector(rng, b);
        unitVector(rng, d);
        const f32 s = rng.range(0.05f, 0.5f);
        const f32 v0[3] = {c[0] + a[0] * s, c[1] + a[1] * s, c[2] + a[2] * s};
        const f32 v1[3] = {c[0] + b[0] * s, c[1] + b[1] * s, c[2] + b[2] * s};
        const f32 v2[3] = {c[0] + d[0] * s, c[1] + d[1] * s, c[2] + d[2] * s};
        lights.push_back(makeTriangleLight(v0, v1, v2, rng.range(0.5f, 5.f), (i % 5u) == 0u, 3000u + i));
    }
    const f32 sun[3] = {0.3f, -1.f, 0.2f};
    const f32 moon[3] = {-0.5f, -0.4f, 0.8f};
    lights.push_back(makeDirectionalLight(sun, 3.f, 9000u));
    lights.push_back(makeDirectionalLight(moon, 0.1f, 9001u));
    // Shuffle so kinds interleave in the input order (the tree must not depend on grouping).
    for (u32 i = static_cast<u32>(lights.size()); i > 1u; --i) {
        const u32 j = static_cast<u32>(rng.next() % i);
        std::swap(lights[i - 1u], lights[j]);
    }
    return lights;
}

/// Emissive triangles only: a ceiling of `quads` x `quads` quads (2 triangles each, facing down), a lamp shade
/// of small triangles around a point and some floating triangles. `quads` = 71 gives 10082 + 400 + 200 = ~10.7k.
inline std::vector<LightTreeLight> triangleScene(u64 seed, u32 quads) {
    Rng rng(seed);
    std::vector<LightTreeLight> lights;
    const f32 size = 16.f / static_cast<f32>(quads);
    u32 id = 0;
    for (u32 y = 0; y < quads; ++y) {
        for (u32 x = 0; x < quads; ++x) {
            const f32 x0 = -8.f + size * static_cast<f32>(x);
            const f32 z0 = -8.f + size * static_cast<f32>(y);
            const f32 h = 4.f + 0.01f * rng.uniform();
            const f32 a[3] = {x0, h, z0};
            const f32 b[3] = {x0 + size, h, z0};
            const f32 c[3] = {x0 + size, h, z0 + size};
            const f32 d[3] = {x0, h, z0 + size};
            // Facing down (-y): (b - a) x (c - a) = (size, 0, 0) x (size, 0, size) = (0, -size^2, 0).
            const f32 radiance = ((x / 8u + y / 8u) % 3u == 0u) ? 5.f : 0.5f;
            lights.push_back(makeTriangleLight(a, b, c, radiance, false, id++));
            lights.push_back(makeTriangleLight(a, c, d, radiance, false, id++));
        }
    }
    for (u32 i = 0; i < 400u; ++i) {
        f32 a[3], b[3], c[3];
        unitVector(rng, a);
        unitVector(rng, b);
        unitVector(rng, c);
        const f32 r = 0.3f;
        const f32 v0[3] = {2.f + a[0] * r, 1.f + a[1] * r, -3.f + a[2] * r};
        const f32 v1[3] = {2.f + b[0] * r, 1.f + b[1] * r, -3.f + b[2] * r};
        const f32 v2[3] = {2.f + c[0] * r, 1.f + c[1] * r, -3.f + c[2] * r};
        lights.push_back(makeTriangleLight(v0, v1, v2, 20.f, true, id++));
    }
    for (u32 i = 0; i < 200u; ++i) {
        f32 c[3] = {rng.range(-8.f, 8.f), rng.range(0.f, 3.f), rng.range(-8.f, 8.f)};
        f32 a[3], b[3];
        unitVector(rng, a);
        unitVector(rng, b);
        const f32 v0[3] = {c[0], c[1], c[2]};
        const f32 v1[3] = {c[0] + a[0] * 0.2f, c[1] + a[1] * 0.2f, c[2] + a[2] * 0.2f};
        const f32 v2[3] = {c[0] + b[0] * 0.2f, c[1] + b[1] * 0.2f, c[2] + b[2] * 0.2f};
        lights.push_back(makeTriangleLight(v0, v1, v2, 2.f, false, id++));
    }
    return lights;
}

struct ShadingPoint {
    f32 p[3] = {0.f, 0.f, 0.f};
    f32 n[3] = {0.f, 0.f, 0.f};
};

/// Random shading points in a box; every other one without a normal (a volume point).
inline std::vector<ShadingPoint> shadingPoints(u64 seed, u32 count, f32 extent) {
    Rng rng(seed);
    std::vector<ShadingPoint> points(count);
    for (u32 i = 0; i < count; ++i) {
        ShadingPoint& s = points[i];
        s.p[0] = rng.range(-extent, extent);
        s.p[1] = rng.range(-extent, extent);
        s.p[2] = rng.range(-extent, extent);
        if ((i % 2u) == 0u) {
            unitVector(rng, s.n);
        }
    }
    return points;
}

} // namespace lt_test
