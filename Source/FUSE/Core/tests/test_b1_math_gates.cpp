// B1.4 / B1.8 math gates (FUSE_MASTER_PLAN "B1.8 — Phase 1 Deliverables & Test Suite"):
//   - vec4 SIMD operations produce bit-identical results to the scalar reference
//   - mat4 multiply matches the reference scalar implementation exactly
//   - All SDF primitives match a reference ray marcher within 0.0001
//   - SDF normals via gradient match finite-difference normals within 0.001
//   - Quaternion slerp produces a unit quaternion at all interpolation points
//   - GRIA Alpha evaluates correctly on the host (constexpr; CUDA device path is hardware-only)

#include <fuse/gria.hpp>
#include <fuse/math/mat.hpp>
#include <fuse/math/quat.hpp>
#include <fuse/math/sdf.hpp>
#include <fuse/math/simd.hpp>
#include <fuse/math/vec.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>

namespace {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::math::Vec3;
namespace simd = fuse::math::simd;
namespace SDF = fuse::math::SDF;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// Bit-identical, except that a NaN result only has to be NaN on both paths: ISO C++ leaves the
// NaN sign/payload of an arithmetic result unspecified and compilers may commute scalar operands.
u32 g_nanPayloadDiffs = 0;

bool sameBits(f32 a, f32 b) {
    if (std::memcmp(&a, &b, sizeof(f32)) == 0) {
        return true;
    }
    if (std::isnan(a) && std::isnan(b)) {
        ++g_nanPayloadDiffs;
        return true;
    }
    return false;
}

bool sameBits(const simd::Float4& a, const simd::Float4& b) {
    return sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.z, b.z) && sameBits(a.w, b.w);
}

f32 randomLane(std::mt19937& rng) {
    static const f32 kSpecials[] = {
        0.f, -0.f, 1.f, -1.f, std::numeric_limits<f32>::denorm_min(), -std::numeric_limits<f32>::denorm_min(),
        std::numeric_limits<f32>::min(), std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max(),
        std::numeric_limits<f32>::infinity(), -std::numeric_limits<f32>::infinity(),
        std::numeric_limits<f32>::quiet_NaN(), 1e-30f, 3.4e37f,
    };
    const u32 pick = rng() % 16u;
    if (pick == 0u) {
        return kSpecials[rng() % (sizeof(kSpecials) / sizeof(kSpecials[0]))];
    }
    if (pick == 1u) {
        // Arbitrary bit pattern (any exponent), NaNs excluded.
        u32 bits = static_cast<u32>(rng());
        f32 v = 0.f;
        std::memcpy(&v, &bits, sizeof(v));
        return std::isnan(v) ? 0.5f : v;
    }
    std::uniform_real_distribution<f32> dist(-1000.f, 1000.f);
    return dist(rng);
}

simd::Float4 randomFloat4(std::mt19937& rng) {
    return {randomLane(rng), randomLane(rng), randomLane(rng), randomLane(rng)};
}

void testVec4SimdBitIdentical() {
    std::printf("  simd backend: %s\n", simd::hasSseBackend() ? "SSE" : "scalar");
    std::mt19937 rng(0x5EEDu);
    constexpr u32 kSamples = 1'000'000u;
    u32 mismatches = 0;
    for (u32 i = 0; i < kSamples; ++i) {
        const simd::Float4 a = randomFloat4(rng);
        const simd::Float4 b = randomFloat4(rng);
        const f32 s = randomLane(rng);
        mismatches += sameBits(simd::add(a, b), simd::detail::addScalar(a, b)) ? 0u : 1u;
        mismatches += sameBits(simd::sub(a, b), simd::detail::subScalar(a, b)) ? 0u : 1u;
        mismatches += sameBits(simd::mul(a, b), simd::detail::mulScalar(a, b)) ? 0u : 1u;
        mismatches += sameBits(simd::div(a, b), simd::detail::divScalar(a, b)) ? 0u : 1u;
        mismatches += sameBits(simd::scale(a, s), simd::detail::scaleScalar(a, s)) ? 0u : 1u;
        mismatches += sameBits(simd::min(a, b), simd::detail::minScalar(a, b)) ? 0u : 1u;
        mismatches += sameBits(simd::max(a, b), simd::detail::maxScalar(a, b)) ? 0u : 1u;
        mismatches += sameBits(simd::dot(a, b), simd::detail::dotScalar(a, b)) ? 0u : 1u;

        simd::Mat4 m{};
        for (auto& col : m.cols) {
            col = randomFloat4(rng);
        }
        mismatches +=
            sameBits(simd::multiplyColumn(m, a), simd::detail::multiplyColumnScalar(m, a)) ? 0u : 1u;
    }
    std::printf("  vec4 simd vs scalar: %u samples x 9 ops, %u bit mismatches (%u NaN-payload-only lanes)\n",
                kSamples, mismatches, g_nanPayloadDiffs);
    expectTrue(mismatches == 0u, "vec4 SIMD ops are bit-identical to the scalar reference");
}

void testMat4MultiplyExact() {
    std::mt19937 rng(0x4A7u);
    std::uniform_real_distribution<f32> dist(-100.f, 100.f);
    u32 mismatches = 0;
    constexpr u32 kSamples = 200'000u;
    for (u32 i = 0; i < kSamples; ++i) {
        fuse::math::Mat4 a{};
        fuse::math::Mat4 b{};
        for (u32 k = 0; k < 16u; ++k) {
            a.data[k] = dist(rng);
            b.data[k] = dist(rng);
        }
        // Reference: straightforward row-by-column scalar loop summed in k order.
        fuse::math::Mat4 reference{};
        for (u32 row = 0; row < 4u; ++row) {
            for (u32 col = 0; col < 4u; ++col) {
                f32 sum = a.at(row, 0) * b.at(0, col);
                for (u32 k = 1; k < 4u; ++k) {
                    sum += a.at(row, k) * b.at(k, col);
                }
                reference.at(row, col) = sum;
            }
        }
        const fuse::math::Mat4 scalar = a * b;
        const fuse::math::Mat4 lanes = simd::multiply(simd::Mat4::fromScalar(a), simd::Mat4::fromScalar(b)).toScalar();
        for (u32 k = 0; k < 16u; ++k) {
            mismatches += scalar.data[k] == reference.data[k] ? 0u : 1u;
            mismatches += lanes.data[k] == reference.data[k] ? 0u : 1u;
        }
    }
    std::printf("  mat4 multiply: %u products (scalar + SIMD) vs reference, %u mismatching elements\n", kSamples,
                mismatches);
    expectTrue(mismatches == 0u, "mat4 multiply (scalar and SIMD) matches the reference exactly");
}

// ---- SDF --------------------------------------------------------------------------------------

struct Ray {
    Vec3 origin;
    Vec3 dir;
};

/// Reference sphere tracer: march until |d| < 1e-6, return hit t (or -1).
template <typename Sdf>
f32 rayMarch(const Sdf& sdf, const Ray& ray, f32 maxT = 100.f) {
    f32 t = 0.f;
    for (u32 step = 0; step < 4096u && t < maxT; ++step) {
        const f32 d = sdf(ray.origin + ray.dir * t);
        if (d < 1e-6f) {
            return t;
        }
        t += d;
    }
    return -1.f;
}

f64 analyticSphereHit(const Ray& ray, f64 radius) {
    const f64 b = static_cast<f64>(ray.origin.x) * ray.dir.x + static_cast<f64>(ray.origin.y) * ray.dir.y +
                  static_cast<f64>(ray.origin.z) * ray.dir.z;
    const f64 c = static_cast<f64>(ray.origin.x) * ray.origin.x + static_cast<f64>(ray.origin.y) * ray.origin.y +
                  static_cast<f64>(ray.origin.z) * ray.origin.z - radius * radius;
    const f64 disc = b * b - c;
    return disc < 0.0 ? -1.0 : -b - std::sqrt(disc);
}

f64 analyticBoxHit(const Ray& ray, const Vec3& h) {
    const f64 o[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
    const f64 d[3] = {ray.dir.x, ray.dir.y, ray.dir.z};
    const f64 e[3] = {h.x, h.y, h.z};
    f64 tNear = -1e30;
    f64 tFar = 1e30;
    for (int i = 0; i < 3; ++i) {
        const f64 t0 = (-e[i] - o[i]) / d[i];
        const f64 t1 = (e[i] - o[i]) / d[i];
        tNear = std::max(tNear, std::min(t0, t1));
        tFar = std::min(tFar, std::max(t0, t1));
    }
    return tNear <= tFar ? tNear : -1.0;
}

f64 referenceBoxDistance(const Vec3& p, const Vec3& h) {
    const f64 q[3] = {std::fabs(static_cast<f64>(p.x)) - h.x, std::fabs(static_cast<f64>(p.y)) - h.y,
                      std::fabs(static_cast<f64>(p.z)) - h.z};
    const f64 ox = std::max(q[0], 0.0);
    const f64 oy = std::max(q[1], 0.0);
    const f64 oz = std::max(q[2], 0.0);
    return std::sqrt(ox * ox + oy * oy + oz * oz) + std::min(std::max(q[0], std::max(q[1], q[2])), 0.0);
}

Vec3 randomUnit(std::mt19937& rng) {
    std::normal_distribution<f32> n(0.f, 1.f);
    return Vec3{n(rng), n(rng), n(rng)}.normalized();
}

Ray aimedRay(std::mt19937& rng, const Vec3& target, f32 distance) {
    const Vec3 origin = target + randomUnit(rng) * distance;
    return {origin, (target - origin).normalized()};
}

void testSdfPrimitivesMatchRayMarcher() {
    std::mt19937 rng(0x5DFu);
    std::uniform_real_distribution<f32> uni(-1.f, 1.f);
    f64 worstSphere = 0.0;
    f64 worstBox = 0.0;
    f64 worstField = 0.0;
    u32 misses = 0;

    for (u32 i = 0; i < 20'000u; ++i) {
        const f32 radius = 0.25f + 2.f * std::fabs(uni(rng));
        const Ray ray = aimedRay(rng, Vec3{uni(rng), uni(rng), uni(rng)} * (0.5f * radius), 3.f + 10.f * std::fabs(uni(rng)) + radius);
        const f64 expected = analyticSphereHit(ray, radius);
        const f32 marched = rayMarch([&](Vec3 p) { return SDF::sphere(p, radius); }, ray);
        if (expected < 0.0 || marched < 0.f) {
            ++misses;
            continue;
        }
        worstSphere = std::max(worstSphere, std::fabs(expected - marched));

        const Vec3 half{0.25f + std::fabs(uni(rng)), 0.25f + std::fabs(uni(rng)), 0.25f + std::fabs(uni(rng))};
        const Ray boxRay = aimedRay(rng, Vec3{uni(rng) * half.x, uni(rng) * half.y, uni(rng) * half.z} * 0.8f,
                                    4.f + 8.f * std::fabs(uni(rng)));
        const f64 boxExpected = analyticBoxHit(boxRay, half);
        const f32 boxMarched = rayMarch([&](Vec3 p) { return SDF::box(p, half); }, boxRay);
        if (boxExpected < 0.0 || boxMarched < 0.f) {
            ++misses;
            continue;
        }
        worstBox = std::max(worstBox, std::fabs(boxExpected - boxMarched));

        // Field values against double-precision references at random points.
        const Vec3 p{uni(rng) * 4.f, uni(rng) * 4.f, uni(rng) * 4.f};
        const f64 sphereRef = std::sqrt(static_cast<f64>(p.x) * p.x + static_cast<f64>(p.y) * p.y +
                                        static_cast<f64>(p.z) * p.z) - radius;
        worstField = std::max(worstField, std::fabs(sphereRef - SDF::sphere(p, radius)));
        worstField = std::max(worstField, std::fabs(referenceBoxDistance(p, half) - SDF::box(p, half)));
        const f32 k = 0.1f + std::fabs(uni(rng));
        const f64 d1 = sphereRef;
        const f64 d2 = referenceBoxDistance(p, half);
        const f64 h = std::max(k - std::fabs(d1 - d2), 0.0) / k;
        const f64 smoothRef = std::min(d1, d2) - h * h * k * 0.25;
        worstField = std::max(worstField, std::fabs(smoothRef - SDF::opSmoothUnion(SDF::sphere(p, radius),
                                                                                  SDF::box(p, half), k)));
    }
    std::printf("  SDF vs reference: max hit error sphere=%.3g box=%.3g, max field error=%.3g, misses=%u\n",
                worstSphere, worstBox, worstField, misses);
    expectTrue(misses == 0u, "reference ray marcher and analytic intersection agree on every hit");
    expectTrue(worstSphere <= 1e-4, "SDF sphere matches the reference ray marcher within 0.0001");
    expectTrue(worstBox <= 1e-4, "SDF box matches the reference ray marcher within 0.0001");
    expectTrue(worstField <= 1e-4, "SDF sphere/box/smooth-union field values within 0.0001 of reference");
}

void testSdfGradientNormals() {
    std::mt19937 rng(0x6AADu);
    std::uniform_real_distribution<f32> uni(-3.f, 3.f);
    f32 worst = 0.f;
    u32 checked = 0;
    const Vec3 half{1.f, 0.6f, 1.4f};
    for (u32 i = 0; i < 100'000u; ++i) {
        const Vec3 p{uni(rng), uni(rng), uni(rng)};
        const f32 radius = 1.25f;
        if (p.length() > 0.05f) {
            const Vec3 fd = SDF::finiteDifferenceNormal([&](Vec3 x) { return SDF::sphere(x, radius); }, p);
            const Vec3 g = SDF::sphereGradient(p);
            worst = std::max(worst, (fd - g).length());
            ++checked;
        }

        // Central differences are only a valid reference where the field is smooth across the
        // ±h stencil. Skip the box's non-smooth loci: the inner medial surfaces (two axes tie for
        // the nearest face), the face/edge-region boundaries outside (a non-dominant q_i crosses
        // 0) and the |p_i| kinks on the coordinate planes.
        const Vec3 q{std::fabs(p.x) - half.x, std::fabs(p.y) - half.y, std::fabs(p.z) - half.z};
        const f32 qs[3] = {q.x, q.y, q.z};
        const u32 dominant = (q.x >= q.y && q.x >= q.z) ? 0u : (q.y >= q.z ? 1u : 2u);
        constexpr f32 kBand = 0.01f;
        bool nonSmooth = std::fabs(p.x) < kBand || std::fabs(p.y) < kBand || std::fabs(p.z) < kBand;
        for (u32 axis = 0; axis < 3u; ++axis) {
            if (axis == dominant) {
                continue;
            }
            nonSmooth = nonSmooth || std::fabs(qs[axis]) < kBand || std::fabs(qs[axis] - qs[dominant]) < kBand;
        }
        if (!nonSmooth) {
            const Vec3 fd = SDF::finiteDifferenceNormal([&](Vec3 x) { return SDF::box(x, half); }, p);
            const Vec3 g = SDF::boxGradient(p, half);
            worst = std::max(worst, (fd - g).length());
            ++checked;
        }
    }
    std::printf("  SDF gradient vs finite-difference normals: %u points, max error %.3g\n", checked, worst);
    expectTrue(worst <= 1e-3f, "SDF gradient normals match finite-difference normals within 0.001");
}

void testQuatSlerpUnit() {
    std::mt19937 rng(0x51E7u);
    std::normal_distribution<f32> n(0.f, 1.f);
    f32 worst = 0.f;
    u32 samples = 0;
    for (u32 pair = 0; pair < 2'000u; ++pair) {
        const fuse::math::Quat a = fuse::math::Quat{n(rng), n(rng), n(rng), n(rng)}.normalized();
        fuse::math::Quat b = fuse::math::Quat{n(rng), n(rng), n(rng), n(rng)}.normalized();
        switch (pair % 4u) {
        case 0: b = {-a.x, -a.y, -a.z, -a.w}; break; // antipodal (same rotation)
        case 1: b = fuse::math::Quat{a.x + 1e-4f, a.y, a.z, a.w}.normalized(); break; // nearly equal
        default: break;
        }
        for (u32 step = 0; step <= 1000u; ++step) {
            const f32 t = static_cast<f32>(step) / 1000.f;
            const f32 len = fuse::math::slerp(a, b, t).length();
            worst = std::max(worst, std::fabs(len - 1.f));
            ++samples;
        }
    }
    std::printf("  slerp: %u samples, max |len - 1| = %.3g\n", samples, worst);
    expectTrue(worst <= 1e-6f, "slerp output is a unit quaternion at every interpolation point");
}

void testGriaAlphaHost() {
    static_assert(fuse::ALPHA_EXACT.is_exact());
    static_assert(fuse::ALPHA_APPROXIMATE.is_approximate());
    static_assert(fuse::ALPHA_CHAOS_EDGE.at_edge_of_chaos());
    static_assert(fuse::Alpha::from_entropy_ratio(2.f, 8.f).get() == 0.75f);
    static_assert(fuse::Alpha(2.f).get() == 1.f && fuse::Alpha(-1.f).get() == 0.f, "alpha clamps to [0,1]");
    static_assert(fuse::Alpha(0.25f).blend(10.f, 20.f) == 12.5f);
    static_assert(sizeof(fuse::Alpha) == sizeof(f32), "Alpha is passed by value as one float");

    const volatile f32 hOut = 3.f;
    const volatile f32 hIn = 4.f;
    const fuse::Alpha a = fuse::Alpha::from_entropy_ratio(hOut, hIn);
    expectTrue(a.get() == 0.25f, "from_entropy_ratio = 1 - H(out)/H(in)");
    expectTrue(!a.is_exact() && !a.is_approximate() && !a.at_edge_of_chaos(), "0.25 is interior");
    expectTrue(fuse::Alpha::from_entropy_ratio(1.f, 0.f).is_exact(), "zero input entropy maps to exact");
    expectTrue(fuse::Alpha(std::numeric_limits<f32>::quiet_NaN()).get() == 0.f, "NaN alpha maps to exact");
    expectTrue(fuse::Alpha(0.495f).at_edge_of_chaos() && !fuse::Alpha(0.52f).at_edge_of_chaos(),
               "edge-of-chaos band is (0.49, 0.51)");
}

} // namespace

int main() {
    testVec4SimdBitIdentical();
    testMat4MultiplyExact();
    testSdfPrimitivesMatchRayMarcher();
    testSdfGradientNormals();
    testQuatSlerpUnit();
    testGriaAlphaHost();

    if (g_failures == 0) {
        std::printf("fuse_core_b1_math_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_core_b1_math_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
