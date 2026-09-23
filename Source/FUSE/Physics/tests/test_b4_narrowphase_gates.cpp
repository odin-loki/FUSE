// B4.11 narrowphase gate rows (master plan):
//  - sphere-sphere matches the analytic (Bullet btSphereSphereCollisionAlgorithm) result within 0.001
//  - sphere-plane produces the correct normal and penetration depth at all angles
//  - capsule-capsule handles parallel capsules and endpoint degeneracies correctly
//  - GJK agrees with a SAT reference on 10k random convex hull pairs (oriented boxes)
//  - EPA penetration depth within 0.01 of the SAT reference for every overlapping pair
//  - SDF collision produces smooth contact normals across surface transitions
//  - narrowphase for 1k contact pairs < 3 ms (CPU reference; the RTX 3090 figure is a hardware row)
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/gjk.hpp>
#include <fuse/physics/physics_data.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

int g_failures = 0;

using namespace fuse::physics;
using namespace fuse::physics::narrowphase;
using fuse::u32;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

f32 maxAbsDiff(vec3 a, vec3 b) {
    return std::max({std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)});
}

void testSphereSphereAnalytic() {
    std::mt19937 rng(1u);
    std::uniform_real_distribution<f32> pos(-2.f, 2.f);
    std::uniform_real_distribution<f32> rad(0.1f, 1.5f);
    f32 worst = 0.f;
    int wrongValidity = 0;
    int contacts = 0;
    for (int i = 0; i < 10'000; ++i) {
        const vec3 a{pos(rng), pos(rng), pos(rng)};
        const vec3 b{pos(rng), pos(rng), pos(rng)};
        const f32 ra = rad(rng);
        const f32 rb = rad(rng);
        const f32 dist = (a - b).length();
        const ContactManifold m = collideSphereSphere(a, ra, b, rb, 0u, 1u);
        const bool expected = dist <= ra + rb;
        wrongValidity += m.valid != expected ? 1 : 0;
        if (!expected || !m.valid || dist < 1e-4f) {
            continue;
        }
        ++contacts;
        // Bullet: normalOnB = (A - B) / |A - B|, depth = rA + rB - dist, pointOnB = B + n * rB.
        const vec3 n = (a - b) * (1.f / dist);
        worst = std::max(worst, maxAbsDiff(m.contactNormal, n));
        worst = std::max(worst, std::fabs(m.penetrationDepth - (ra + rb - dist)));
        worst = std::max(worst, maxAbsDiff(m.contactPoint, b + n * rb));
    }
    std::printf("sphere-sphere: %d contacts, worst deviation from analytic %.2e, validity mismatches %d\n", contacts,
                worst, wrongValidity);
    expectTrue(wrongValidity == 0, "sphere-sphere overlap test matches the analytic predicate");
    expectTrue(worst < 1e-3f, "sphere-sphere normal/depth/point within 0.001 of the analytic result");
}

void testSpherePlaneAllAngles() {
    // Fibonacci sphere of plane normals, each with several offsets and penetrations.
    f32 worstNormal = 0.f;
    f32 worstDepth = 0.f;
    int missing = 0;
    const int count = 2000;
    std::mt19937 rng(2u);
    std::uniform_real_distribution<f32> offset(-5.f, 5.f);
    std::uniform_real_distribution<f32> signedDist(-0.49f, 0.49f);
    for (int i = 0; i < count; ++i) {
        const f32 y = 1.f - 2.f * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(count);
        const f32 r = std::sqrt(1.f - y * y);
        const f32 phi = 2.39996323f * static_cast<f32>(i);
        const vec3 n{r * std::cos(phi), y, r * std::sin(phi)};
        const f32 planeD = offset(rng);
        const f32 radius = 0.5f;
        const f32 s = signedDist(rng);
        // A point on the plane, pushed along the normal by `s`, plus in-plane jitter.
        vec3 tangent = n.cross(std::fabs(n.x) < 0.9f ? vec3{1.f, 0.f, 0.f} : vec3{0.f, 1.f, 0.f}).normalized();
        const vec3 center = n * (planeD + s) + tangent * offset(rng);
        const ContactManifold m = collideSpherePlane(center, radius, n, planeD, 0u, 1u);
        if (!m.valid) {
            ++missing;
            continue;
        }
        worstNormal = std::max(worstNormal, maxAbsDiff(m.contactNormal, n));
        worstDepth = std::max(worstDepth, std::fabs(m.penetrationDepth - (radius - s)));
    }
    std::printf("sphere-plane: %d orientations, worst normal error %.2e, worst depth error %.2e, missing %d\n", count,
                worstNormal, worstDepth, missing);
    expectTrue(missing == 0, "every penetrating sphere-plane pair reports a contact");
    expectTrue(worstNormal < 1e-5f && worstDepth < 1e-4f, "sphere-plane normal and depth exact at all angles");
}

/// Convex (s, t) minimisation by nested ternary search: reference segment distance.
f32 referenceSegmentDistance(vec3 p1, vec3 q1, vec3 p2, vec3 q2) {
    auto distAt = [&](f32 s) {
        const vec3 a = p1 + (q1 - p1) * s;
        f32 lo = 0.f;
        f32 hi = 1.f;
        for (int i = 0; i < 80; ++i) {
            const f32 m1 = lo + (hi - lo) / 3.f;
            const f32 m2 = hi - (hi - lo) / 3.f;
            const f32 d1 = (a - (p2 + (q2 - p2) * m1)).length();
            const f32 d2 = (a - (p2 + (q2 - p2) * m2)).length();
            (d1 < d2 ? hi : lo) = d1 < d2 ? m2 : m1;
        }
        return (a - (p2 + (q2 - p2) * (0.5f * (lo + hi)))).length();
    };
    f32 lo = 0.f;
    f32 hi = 1.f;
    for (int i = 0; i < 80; ++i) {
        const f32 m1 = lo + (hi - lo) / 3.f;
        const f32 m2 = hi - (hi - lo) / 3.f;
        (distAt(m1) < distAt(m2) ? hi : lo) = distAt(m1) < distAt(m2) ? m2 : m1;
    }
    return distAt(0.5f * (lo + hi));
}

void testCapsuleCapsule() {
    const vec3 params{0.5f, 1.f, 0.f}; // radius 0.5, half height 1
    // Parallel, side by side with overlapping extents: normal along x, depth 0.2.
    ContactManifold m = collideCapsuleCapsule({0.8f, 0.5f, 0.f}, params, {0.f, 0.f, 0.f}, params, 0u, 1u);
    expectTrue(m.valid && maxAbsDiff(m.contactNormal, {1.f, 0.f, 0.f}) < 1e-5f &&
                   std::fabs(m.penetrationDepth - 0.2f) < 1e-5f,
               "parallel side-by-side capsules: normal +x, depth 0.2");
    // Parallel and collinear, end to end: caps overlap by 0.1 along y.
    m = collideCapsuleCapsule({0.f, 0.f, 0.f}, params, {0.f, 2.9f, 0.f}, params, 0u, 1u);
    expectTrue(m.valid && maxAbsDiff(m.contactNormal, {0.f, -1.f, 0.f}) < 1e-5f &&
                   std::fabs(m.penetrationDepth - 0.1f) < 1e-5f,
               "collinear capsules touching end caps: normal -y, depth 0.1");
    // Parallel but separated.
    m = collideCapsuleCapsule({1.01f, 0.3f, 0.f}, params, {0.f, 0.f, 0.f}, params, 0u, 1u);
    expectTrue(!m.valid, "separated parallel capsules report no contact");
    // Axes crossing through each other: normal perpendicular to both axes, full radius depth.
    m = collideCapsuleSegments({-1.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 0.3f, {0.f, 0.f, 0.f}, {0.f, 0.f, -1.f},
                               {0.f, 0.f, 1.f}, 0.2f, {0.f, 0.f, 0.f}, 0u, 1u);
    expectTrue(m.valid && std::fabs(std::fabs(m.contactNormal.y) - 1.f) < 1e-5f &&
                   std::fabs(m.penetrationDepth - 0.5f) < 1e-5f,
               "crossing axes: normal perpendicular to both, depth = sum of radii");
    // Zero-length capsule degenerates to a sphere.
    const ContactManifold asSphere =
        collideCapsuleSegments({0.3f, 1.2f, 0.f}, {0.3f, 1.2f, 0.f}, 0.4f, {0.3f, 1.2f, 0.f}, {0.f, -1.f, 0.f},
                               {0.f, 1.f, 0.f}, 0.5f, {0.f, 0.f, 0.f}, 0u, 1u);
    const ContactManifold reference = collideCapsuleSphere({0.3f, 1.2f, 0.f}, 0.4f, {0.f, 0.f, 0.f}, {0.5f, 1.f, 0.f},
                                                           0u, 1u);
    expectTrue(asSphere.valid && reference.valid &&
                   maxAbsDiff(asSphere.contactNormal, reference.contactNormal) < 1e-5f &&
                   std::fabs(asSphere.penetrationDepth - reference.penetrationDepth) < 1e-5f,
               "zero-length capsule matches capsule-sphere");
    // Both zero-length and coincident: still a valid contact with a unit normal.
    m = collideCapsuleSegments({}, {}, 0.2f, {}, {}, {}, 0.2f, {}, 0u, 1u);
    expectTrue(m.valid && std::fabs(m.contactNormal.length() - 1.f) < 1e-5f, "coincident point capsules");

    // Random segments (including near-parallel ones) vs the reference distance.
    std::mt19937 rng(3u);
    std::uniform_real_distribution<f32> coord(-1.f, 1.f);
    f32 worst = 0.f;
    for (int i = 0; i < 1000; ++i) {
        const vec3 p1{coord(rng), coord(rng), coord(rng)};
        const vec3 q1 = p1 + vec3{coord(rng), coord(rng), coord(rng)};
        const vec3 p2{coord(rng), coord(rng), coord(rng)};
        vec3 q2 = p2 + vec3{coord(rng), coord(rng), coord(rng)};
        if (i % 4 == 0) {
            q2 = p2 + (q1 - p1) * (0.5f + 0.5f * std::fabs(coord(rng))); // exactly parallel
        }
        vec3 c1{};
        vec3 c2{};
        const f32 d = std::sqrt(closestPointsSegmentSegment(p1, q1, p2, q2, c1, c2));
        worst = std::max(worst, std::fabs(d - referenceSegmentDistance(p1, q1, p2, q2)));
    }
    std::printf("capsule-capsule: worst segment distance error %.2e over 1000 random pairs (250 parallel)\n", worst);
    expectTrue(worst < 1e-4f, "segment closest points match the reference distance, parallel included");
}

struct Obb {
    std::array<vec3, 8> vertices{};
    std::array<vec3, 3> axes{};
};

Obb makeObb(vec3 center, vec3 half, std::mt19937& rng) {
    std::normal_distribution<f32> g(0.f, 1.f);
    f32 qx = g(rng), qy = g(rng), qz = g(rng), qw = g(rng);
    const f32 len = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    qx /= len;
    qy /= len;
    qz /= len;
    qw /= len;
    Obb box{};
    box.axes[0] = {1.f - 2.f * (qy * qy + qz * qz), 2.f * (qx * qy + qz * qw), 2.f * (qx * qz - qy * qw)};
    box.axes[1] = {2.f * (qx * qy - qz * qw), 1.f - 2.f * (qx * qx + qz * qz), 2.f * (qy * qz + qx * qw)};
    box.axes[2] = {2.f * (qx * qz + qy * qw), 2.f * (qy * qz - qx * qw), 1.f - 2.f * (qx * qx + qy * qy)};
    for (int i = 0; i < 8; ++i) {
        const f32 sx = (i & 1) ? 1.f : -1.f;
        const f32 sy = (i & 2) ? 1.f : -1.f;
        const f32 sz = (i & 4) ? 1.f : -1.f;
        box.vertices[i] = center + box.axes[0] * (sx * half.x) + box.axes[1] * (sy * half.y) + box.axes[2] * (sz * half.z);
    }
    return box;
}

/// SAT over the 15 box axes: returns the minimum overlap (negative when separated). For convex
/// polyhedra the minimum over face normals and edge-edge axes is the exact penetration depth.
f32 satMinOverlap(const Obb& a, const Obb& b, vec3 offsetA = {}) {
    std::vector<vec3> axes(a.axes.begin(), a.axes.end());
    axes.insert(axes.end(), b.axes.begin(), b.axes.end());
    for (const vec3& u : a.axes) {
        for (const vec3& v : b.axes) {
            const vec3 c = u.cross(v);
            if (c.length() > 1e-5f) {
                axes.push_back(c.normalized());
            }
        }
    }
    f32 minOverlap = 1e30f;
    for (const vec3& axis : axes) {
        f32 minA = 1e30f, maxA = -1e30f, minB = 1e30f, maxB = -1e30f;
        for (const vec3& p : a.vertices) {
            const f32 d = (p + offsetA).dot(axis);
            minA = std::min(minA, d);
            maxA = std::max(maxA, d);
        }
        for (const vec3& p : b.vertices) {
            const f32 d = p.dot(axis);
            minB = std::min(minB, d);
            maxB = std::max(maxB, d);
        }
        minOverlap = std::min(minOverlap, std::min(maxA - minB, maxB - minA));
    }
    return minOverlap;
}

void testGjkEpaAgainstSat() {
    std::mt19937 rng(10'000u);
    std::uniform_real_distribution<f32> pos(-1.8f, 1.8f);
    std::uniform_real_distribution<f32> ext(0.2f, 1.2f);
    int gjkMismatch = 0;
    int ambiguous = 0;
    int overlapping = 0;
    int epaBadDepth = 0;
    int epaBadNormal = 0;
    int epaNotConverged = 0;
    f32 worstDepth = 0.f;
    u32 maxEpaIterations = 0;
    u32 maxGjkIterations = 0;
    for (int i = 0; i < 10'000; ++i) {
        const Obb a = makeObb({pos(rng), pos(rng), pos(rng)}, {ext(rng), ext(rng), ext(rng)}, rng);
        const Obb b = makeObb({pos(rng), pos(rng), pos(rng)}, {ext(rng), ext(rng), ext(rng)}, rng);
        const f32 sat = satMinOverlap(a, b);
        if (std::fabs(sat) < 1e-4f) {
            ++ambiguous; // grazing contact: either answer is within float tolerance
            continue;
        }
        u32 iterations = 0;
        GjkSimplex simplex{};
        const bool gjk = gjkIntersect(a.vertices.data(), 8, b.vertices.data(), 8, &simplex, &iterations);
        maxGjkIterations = std::max(maxGjkIterations, iterations);
        gjkMismatch += gjk != (sat > 0.f) ? 1 : 0;
        if (sat <= 0.f) {
            continue;
        }
        ++overlapping;
        const EpaResult epa = epaPenetration(a.vertices.data(), 8, b.vertices.data(), 8);
        maxEpaIterations = std::max(maxEpaIterations, epa.iterations);
        epaNotConverged += epa.converged ? 0 : 1;
        const f32 depthError = std::fabs(epa.depth - sat);
        worstDepth = std::max(worstDepth, depthError);
        epaBadDepth += depthError > 0.01f ? 1 : 0;
        // Translating A along the normal by the depth (plus a hair) must separate the boxes.
        epaBadNormal += satMinOverlap(a, b, epa.normal * (epa.depth + 2e-3f)) > 0.f ? 1 : 0;
    }
    std::printf("GJK vs SAT: 10000 OBB pairs, %d ambiguous skipped, %d mismatches, max %u GJK iterations\n",
                ambiguous, gjkMismatch, maxGjkIterations);
    std::printf("EPA vs SAT: %d overlapping, worst depth error %.2e, %d bad depths, %d bad normals, "
                "%d unconverged, max %u iterations\n",
                overlapping, worstDepth, epaBadDepth, epaBadNormal, epaNotConverged, maxEpaIterations);
    expectTrue(overlapping > 1000, "test set has many overlapping pairs");
    expectTrue(gjkMismatch == 0, "GJK intersection result agrees with SAT for every pair");
    expectTrue(epaBadDepth == 0, "EPA depth within 0.01 of the SAT reference for every pair");
    expectTrue(epaBadNormal == 0, "EPA normal separates the pair when applied with its depth");
    expectTrue(epaNotConverged == 0, "EPA converges on every pair");
}

f32 boxSdf(vec3 p, vec3 half) {
    const vec3 q{std::fabs(p.x) - half.x, std::fabs(p.y) - half.y, std::fabs(p.z) - half.z};
    const vec3 outside{std::max(q.x, 0.f), std::max(q.y, 0.f), std::max(q.z, 0.f)};
    return outside.length() + std::min(std::max(q.x, std::max(q.y, q.z)), 0.f);
}

f32 smoothMin(f32 a, f32 b, f32 k) {
    const f32 h = std::clamp(0.5f + 0.5f * (b - a) / k, 0.f, 1.f);
    return b + (a - b) * h - k * h * (1.f - h);
}

f32 worstNormalTurn(auto sdf) {
    const f32 radius = 0.5f;
    const f32 step = 0.001f;
    vec3 previous{};
    bool havePrevious = false;
    f32 worst = 0.f;
    for (f32 x = -0.5f; x <= 2.0f; x += step) {
        const ContactManifold m = collideSphereSdf(vec3{x, 0.8f, 0.f}, radius, sdf, 0u, 1u);
        if (!m.valid) {
            havePrevious = false;
            continue;
        }
        if (havePrevious) {
            worst = std::max(worst, std::acos(std::clamp(m.contactNormal.dot(previous), -1.f, 1.f)));
        }
        previous = m.contactNormal;
        havePrevious = true;
    }
    return worst * 57.29578f;
}

void testSdfNormalsSmooth() {
    // Box slab with a spherical dome: the sphere slides across the box-to-dome transition.
    const vec3 half{1.f, 0.5f, 1.f};
    const vec3 domeCenter{1.f, 0.5f, 0.f};
    auto smooth = [&](vec3 p) { return smoothMin(boxSdf(p, half), (p - domeCenter).length() - 0.6f, 0.25f); };
    auto hard = [&](vec3 p) { return std::min(boxSdf(p, half), (p - domeCenter).length() - 0.6f); };
    const f32 smoothTurn = worstNormalTurn(smooth);
    const f32 hardTurn = worstNormalTurn(hard);
    std::printf("SDF normals: worst turn per 1 mm step %.3f deg (smooth union) vs %.1f deg (hard union)\n", smoothTurn,
                hardTurn);
    expectTrue(smoothTurn < 1.f, "SDF contact normals turn smoothly across the surface transition");
    expectTrue(hardTurn > 10.f, "the probe detects a real discontinuity (hard union control)");
}

void testNarrowphaseThousandPairs() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    std::vector<broadphase::CandidatePair> pairs;
    std::mt19937 rng(4u);
    std::uniform_real_distribution<f32> jitter(-0.3f, 0.3f);
    for (u32 i = 0; i < 1000u; ++i) {
        const f32 x = static_cast<f32>(i) * 3.f;
        const u32 a = bodies.addBody({x, 0.f, 0.f}, 1.f);
        const u32 b = bodies.addBody({x + 0.8f + jitter(rng), jitter(rng), jitter(rng)}, 1.f);
        const CollisionShapeType type = i % 3 == 0 ? CollisionShapeType::Sphere
                                        : i % 3 == 1 ? CollisionShapeType::Box
                                                     : CollisionShapeType::Capsule;
        shapes.addShape(type, a, {0.5f, 0.5f, 0.5f});
        shapes.addShape(CollisionShapeType::Sphere, b, {0.5f, 0.f, 0.f});
        pairs.push_back({a, b});
    }
    std::vector<double> samples;
    std::size_t contacts = 0;
    for (int i = 0; i < 21; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const std::vector<ContactManifold> manifolds = runNarrowphase(pairs, bodies, shapes);
        samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        contacts = manifolds.size();
    }
    std::sort(samples.begin(), samples.end());
    std::printf("narrowphase: 1000 pairs -> %zu manifolds, median %.3f ms (CPU)\n", contacts, samples[10]);
#if defined(NDEBUG)
    expectTrue(samples[10] < 3.0, "narrowphase for 1k contact pairs < 3 ms (CPU reference)");
#endif
}

} // namespace

int main() {
    testSphereSphereAnalytic();
    testSpherePlaneAllAngles();
    testCapsuleCapsule();
    testGjkEpaAgainstSat();
    testSdfNormalsSmooth();
    testNarrowphaseThousandPairs();

    if (g_failures == 0) {
        std::printf("fuse_b4_narrowphase_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b4_narrowphase_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
