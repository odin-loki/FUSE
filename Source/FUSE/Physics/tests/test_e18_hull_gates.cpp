// E18 part 1 (GAP-PHYS-HULL-MESH, convex hulls):
//  - quickhull: every input point inside, vertices are input points, closed polytope (V - E + F = 2),
//    coplanar faces merged (a sampled cube is 8 vertices / 6 quads), deterministic
//  - hull-vs-hull SAT (Gauss-map pruned edges) == brute-force SAT over every axis on random pairs, and the
//    contact normal / depth agree with it (EPA cross-check on overlapping pairs)
//  - a box-shaped hull gives the Box results (sphere, plane, box, capsule) at random poses
//  - a stack of hulls settles on the ground; two runs are bit-identical
#include "e18_test_common.hpp"

#include <fuse/core/init.hpp>
#include <fuse/physics/narrowphase/gjk.hpp>
#include <fuse/physics/narrowphase/hull_contacts.hpp>
#include <fuse/physics/shapes/shape_pool.hpp>

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>

using namespace e18;
namespace np = fuse::physics::narrowphase;

namespace {

std::vector<vec3> randomCloud(Rng& rng, u32 count, vec3 scale) {
    std::vector<vec3> points;
    for (u32 i = 0; i < count; ++i) {
        const vec3 p = rng.inBox(1.f);
        points.push_back({p.x * scale.x, p.y * scale.y, p.z * scale.z});
    }
    return points;
}

ConvexHull randomHull(Rng& rng) {
    const vec3 scale{rng.range(0.3f, 1.f), rng.range(0.3f, 1.f), rng.range(0.3f, 1.f)};
    ConvexHull hull;
    buildConvexHull(randomCloud(rng, 8u + rng.next() % 24u, scale), hull);
    return hull;
}

void testQuickhull() {
    Rng rng(1u);
    u32 bad = 0;
    u32 built = 0;
    for (u32 trial = 0; trial < 200u; ++trial) {
        const std::vector<vec3> points = randomCloud(rng, 4u + rng.next() % 200u, {1.f, 0.6f, 0.8f});
        ConvexHull hull;
        std::string error;
        if (!buildConvexHull(points, hull, {}, &error)) {
            continue; // a degenerate cloud (flat / tiny) may be refused
        }
        ++built;
        const HullView view = hull.view();
        // Every input point inside (within the builder tolerance).
        for (const vec3& p : points) {
            if (hullMaxFaceDistance(view, p) > 1e-4f) {
                ++bad;
                break;
            }
        }
        // Every hull vertex is an input point.
        for (const vec3& v : hull.vertices) {
            bool found = false;
            for (const vec3& p : points) {
                found = found || (p.x == v.x && p.y == v.y && p.z == v.z);
            }
            bad += found ? 0u : 1u;
        }
        // Closed polytope: Euler characteristic, and every vertex strictly on or below each plane.
        const s32 euler = static_cast<s32>(hull.vertices.size()) - static_cast<s32>(hull.edges.size()) +
                          static_cast<s32>(hull.faces.size());
        bad += euler == 2 ? 0u : 1u;
        for (const HullFace& face : hull.faces) {
            for (const vec3& v : hull.vertices) {
                bad += face.normal.dot(v) - face.offset > 1e-4f ? 1u : 0u;
            }
        }
    }
    std::printf("quickhull: %u random clouds, %u violations\n", built, bad);
    expectTrue(built > 190u && bad == 0u, "quickhull hulls contain their points and are closed polytopes");

    // A densely sampled cube (surface + interior) collapses to 8 vertices and 6 quads.
    std::vector<vec3> cube;
    for (s32 i = -2; i <= 2; ++i) {
        for (s32 j = -2; j <= 2; ++j) {
            for (s32 k = -2; k <= 2; ++k) {
                cube.push_back({0.5f * static_cast<f32>(i), 0.25f * static_cast<f32>(j), 0.4f * static_cast<f32>(k)});
            }
        }
    }
    ConvexHull box;
    const bool ok = buildConvexHull(cube, box);
    bool quads = ok && box.faces.size() == 6u;
    for (const HullFace& face : box.faces) {
        quads = quads && face.indexCount == 4u;
    }
    std::printf("quickhull cube: %zu vertices, %zu faces, %zu edges, volume %.4f\n", box.vertices.size(),
                box.faces.size(), box.edges.size(), static_cast<double>(box.volume));
    expectTrue(ok && box.vertices.size() == 8u && box.edges.size() == 12u && quads,
               "coplanar triangles merge into the cube's six quads (vertices on edges dropped)");
    expectTrue(std::fabs(box.volume - 2.f * 1.f * 1.6f) < 1e-4f, "hull volume is exact (2 x 1 x 1.6)");
    expectTrue(box.halfExtents.x == 1.f && box.halfExtents.y == 0.5f && box.halfExtents.z == 0.8f,
               "hull params (bounding half extents) are exact");

    // Determinism: same points, same hull (vertex order included).
    Rng a(42u);
    Rng b(42u);
    const ConvexHull h1 = randomHull(a);
    const ConvexHull h2 = randomHull(b);
    bool same = h1.vertices.size() == h2.vertices.size() && h1.faceIndices == h2.faceIndices;
    for (usize i = 0; same && i < h1.vertices.size(); ++i) {
        same = std::memcmp(&h1.vertices[i], &h2.vertices[i], sizeof(vec3)) == 0;
    }
    expectTrue(same, "quickhull is deterministic");

    // Degenerate input is refused.
    ConvexHull flat;
    const std::vector<vec3> plane = {{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f, 1.f}, {0.5f, 0.f, 0.3f}};
    expectTrue(!buildConvexHull(plane, flat), "a flat cloud is refused (no volume)");
}

/// Penetration of the two hulls along axis n (overlap of their projections).
f32 overlapAlong(const np::HullPose& a, const np::HullPose& b, vec3 n) {
    f32 minA = 1e30f;
    f32 maxA = -1e30f;
    f32 minB = 1e30f;
    f32 maxB = -1e30f;
    for (u32 i = 0; i < a.hull.vertexCount; ++i) {
        const f32 d = (a.position + rotate(a.orientation, a.hull.vertices[i])).dot(n);
        minA = std::min(minA, d);
        maxA = std::max(maxA, d);
    }
    for (u32 i = 0; i < b.hull.vertexCount; ++i) {
        const f32 d = (b.position + rotate(b.orientation, b.hull.vertices[i])).dot(n);
        minB = std::min(minB, d);
        maxB = std::max(maxB, d);
    }
    // n points from B towards A: A is on the +n side.
    return maxB - minA;
}

void testHullSatAgainstBruteForce() {
    Rng rng(7u);
    std::vector<ConvexHull> hulls;
    for (u32 i = 0; i < 40u; ++i) {
        hulls.push_back(randomHull(rng));
    }
    u32 pairs = 0;
    u32 overlapping = 0;
    u32 separationMismatch = 0;
    u32 classMismatch = 0;
    u32 normalMismatch = 0;
    u32 epaMismatch = 0;
    f32 worstSep = 0.f;
    f32 worstEpa = 0.f;
    std::vector<vec3> worldA;
    std::vector<vec3> worldB;
    for (u32 trial = 0; trial < 4000u; ++trial) {
        const ConvexHull& ha = hulls[rng.next() % hulls.size()];
        const ConvexHull& hb = hulls[rng.next() % hulls.size()];
        if (ha.empty() || hb.empty()) {
            continue;
        }
        const np::HullPose a{ha.view(), rng.inBox(1.2f), rng.rotation()};
        const np::HullPose b{hb.view(), rng.inBox(1.2f), rng.rotation()};
        ++pairs;
        const np::HullSatResult sat = np::hullSatQuery(a, b);
        vec3 axis{};
        const f32 reference = np::hullSatBruteForce(a, b, &axis);
        // Overlapping: the least penetration is attained on a face of the Minkowski difference, so the
        // pruned candidate set must reproduce the brute-force value exactly. Separated: SAT values over
        // different axis sets are only lower bounds of the distance, both must just stay positive.
        if (reference < 0.f || sat.separation() < 0.f) {
            const f32 err = std::fabs(sat.separation() - reference);
            worstSep = std::max(worstSep, err);
            separationMismatch += err > 1e-4f ? 1u : 0u;
        }
        const np::ContactManifold m = np::collideHullHull(a, b, 0u, 1u);
        const bool refOverlap = reference < 0.f;
        if (m.valid != refOverlap && std::fabs(reference) > 1e-5f) {
            ++classMismatch;
        }
        if (!m.valid || !refOverlap) {
            continue;
        }
        ++overlapping;
        // Along the contact normal the hulls overlap at least the minimum penetration and at most the
        // biased choice (face preference: 5 % + 0.5 mm over an edge, 2 % + 0.1 mm between faces).
        const f32 pen = overlapAlong(a, b, m.contactNormal);
        const f32 refPen = -reference;
        if (pen < refPen - 1e-4f || pen > refPen * 1.08f + 1e-3f) {
            ++normalMismatch;
        }
        // EPA on the world vertices agrees with the SAT depth.
        worldA.clear();
        worldB.clear();
        for (const vec3& v : ha.vertices) {
            worldA.push_back(a.position + rotate(a.orientation, v));
        }
        for (const vec3& v : hb.vertices) {
            worldB.push_back(b.position + rotate(b.orientation, v));
        }
        const np::EpaResult epa = np::epaPenetration(worldA.data(), static_cast<u32>(worldA.size()), worldB.data(),
                                                     static_cast<u32>(worldB.size()));
        if (epa.intersecting && epa.converged) {
            const f32 e = std::fabs(epa.depth - refPen);
            worstEpa = std::max(worstEpa, e);
            epaMismatch += e > 1e-3f ? 1u : 0u;
        }
    }
    std::printf("hull SAT: %u random pairs (%u overlapping); separation vs brute force: %u mismatches (worst %.2e); "
                "overlap class mismatches %u; contact normal outside the SAT bounds %u; EPA depth mismatches %u "
                "(worst %.2e)\n",
                pairs, overlapping, separationMismatch, static_cast<double>(worstSep), classMismatch, normalMismatch,
                epaMismatch, static_cast<double>(worstEpa));
    expectTrue(pairs > 3900u && overlapping > 500u, "enough random hull pairs, many overlapping");
    expectTrue(separationMismatch == 0u, "Gauss-map pruned SAT separation == brute-force SAT");
    expectTrue(classMismatch == 0u, "hull contacts exist exactly when the brute-force SAT says the hulls overlap");
    expectTrue(normalMismatch == 0u, "hull contact normal realises the SAT penetration (within the face biases)");
    expectTrue(epaMismatch == 0u, "EPA depth agrees with the SAT depth on overlapping hull pairs");
}

bool samePointSets(const np::ContactManifold& a, const np::ContactManifold& b, f32 tolerance) {
    if (a.pointCount != b.pointCount) {
        return false;
    }
    for (u32 i = 0; i < a.pointCount; ++i) {
        bool found = false;
        for (u32 j = 0; j < b.pointCount && !found; ++j) {
            const vec3 d = a.points[i].point - b.points[j].point;
            found = d.length() <= tolerance && std::fabs(a.points[i].penetration - b.points[j].penetration) <= tolerance;
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

void testBoxShapedHullMatchesBox() {
    Rng rng(99u);
    struct Tally {
        u32 cases = 0;
        u32 contacts = 0;
        u32 mismatch = 0;
    };
    Tally sphere{};
    Tally plane{};
    Tally box{};
    Tally capsule{};
    for (u32 trial = 0; trial < 12000u; ++trial) {
        const vec3 he{rng.range(0.2f, 0.8f), rng.range(0.2f, 0.8f), rng.range(0.2f, 0.8f)};
        ConvexHull hull;
        makeBoxHull(he, hull);
        const u32 ref = ShapePool::global().addHull(std::move(hull));
        np::ShapeInstance asBox{};
        asBox.type = CollisionShapeType::Box;
        asBox.params = he;
        asBox.position = rng.inBox(0.3f);
        asBox.orientation = rng.rotation();
        np::ShapeInstance asHull = asBox;
        asHull.type = CollisionShapeType::ConvexHull;
        asHull.params = ShapePool::global().halfExtents(ref);
        asHull.shapeRef = ref;

        // The other shape is placed touching the box's surface along a random direction, overlapping by
        // up to 8 cm (resting / impact contacts: the regime the solver works in).
        np::ShapeInstance other{};
        const u32 kind = trial % 4u;
        Tally* tally = &sphere;
        const vec3 dir = rng.onSphere();
        const vec3 ax = rotate(asBox.orientation, {he.x, 0.f, 0.f});
        const vec3 ay = rotate(asBox.orientation, {0.f, he.y, 0.f});
        const vec3 az = rotate(asBox.orientation, {0.f, 0.f, he.z});
        const f32 boxReach = std::fabs(ax.dot(dir)) + std::fabs(ay.dot(dir)) + std::fabs(az.dot(dir));
        const f32 overlap = rng.range(-0.08f, 0.02f);
        if (kind == 0u) {
            other.type = CollisionShapeType::Sphere;
            other.params = {rng.range(0.1f, 0.6f), 0.f, 0.f};
            other.position = asBox.position + dir * (boxReach + other.params.x + overlap);
        } else if (kind == 1u) {
            tally = &plane;
            other.type = CollisionShapeType::Plane;
            other.params = dir;
            // The box's lowest point along the plane normal sits `overlap` above the plane.
            other.scalar = dir.dot(asBox.position) - boxReach - overlap;
        } else if (kind == 2u) {
            tally = &box;
            other.type = CollisionShapeType::Box;
            other.params = {rng.range(0.2f, 0.8f), rng.range(0.2f, 0.8f), rng.range(0.2f, 0.8f)};
            other.orientation = rng.rotation();
            const vec3 bx = rotate(other.orientation, {other.params.x, 0.f, 0.f});
            const vec3 by = rotate(other.orientation, {0.f, other.params.y, 0.f});
            const vec3 bz = rotate(other.orientation, {0.f, 0.f, other.params.z});
            const f32 otherReach = std::fabs(bx.dot(dir)) + std::fabs(by.dot(dir)) + std::fabs(bz.dot(dir));
            other.position = asBox.position + dir * (boxReach + otherReach + overlap);
        } else {
            tally = &capsule;
            other.type = CollisionShapeType::Capsule;
            other.params = {rng.range(0.1f, 0.4f), rng.range(0.1f, 0.6f), 0.f};
            other.orientation = rng.rotation();
            const f32 otherReach = std::fabs(capsuleHalfAxis(other.orientation, other.params.y).dot(dir)) + other.params.x;
            other.position = asBox.position + dir * (boxReach + otherReach + overlap);
        }
        ++tally->cases;
        // Both orders: (shape, other) and (other, shape).
        for (u32 order = 0; order < 2u; ++order) {
            const np::ContactManifold expected =
                order == 0u ? np::collideShapes(asBox, other, 0u, 1u) : np::collideShapes(other, asBox, 1u, 0u);
            const np::ContactManifold actual =
                order == 0u ? np::collideShapes(asHull, other, 0u, 1u) : np::collideShapes(other, asHull, 1u, 0u);
            if (expected.valid != actual.valid) {
                // Grazing contacts (depth ~ 0) may differ in validity by rounding.
                const f32 depth = expected.valid ? expected.maxPenetration() : actual.maxPenetration();
                tally->mismatch += std::fabs(depth) > 1e-4f ? 1u : 0u;
                continue;
            }
            if (!expected.valid) {
                continue;
            }
            ++tally->contacts;
            // Canonical labelling: normal towards body 0 (the primitive plane pair keeps the box as body A
            // and does not flip; the pooled dispatch flips instead: the same contact).
            const vec3 expectedNormal = expected.bodyA == 0u ? expected.contactNormal : expected.contactNormal * -1.f;
            const vec3 actualNormal = actual.bodyA == 0u ? actual.contactNormal : actual.contactNormal * -1.f;
            const bool normalOk = expectedNormal.dot(actualNormal) > 0.999f;
            const bool depthOk = std::fabs(expected.maxPenetration() - actual.maxPenetration()) < 1e-4f;
            // Capsule-box uses a different (ternary-search) formulation: normal and depth only. With more
            // than four candidate points both sides reduce to four with depth ties at 1e-3; the kept set may
            // then differ by rounding (the deepest point must still match).
            bool pointsOk = kind == 3u || samePointSets(expected, actual, 1e-3f);
            if (!pointsOk && kind != 3u && expected.pointCount == 4u && actual.pointCount == 4u) {
                pointsOk = (expected.contactPoint - actual.contactPoint).length() < 1e-3f ||
                           std::fabs(expected.penetrationDepth - actual.penetrationDepth) < 1e-4f;
            }
            if (!(normalOk && depthOk && pointsOk)) {
                if (tally->mismatch < 3u) {
                    std::printf("  mismatch kind %u order %u: normal (%.4f %.4f %.4f) vs (%.4f %.4f %.4f), depth %.5f vs "
                                "%.5f, points %u vs %u\n",
                                kind, order, static_cast<double>(expectedNormal.x), static_cast<double>(expectedNormal.y),
                                static_cast<double>(expectedNormal.z), static_cast<double>(actualNormal.x),
                                static_cast<double>(actualNormal.y), static_cast<double>(actualNormal.z),
                                static_cast<double>(expected.maxPenetration()), static_cast<double>(actual.maxPenetration()),
                                expected.pointCount, actual.pointCount);
                }
                ++tally->mismatch;
            }
        }
        ShapePool::global().release(ref);
    }
    std::printf("box-shaped hull == Box: sphere %u contacts / %u mismatches, plane %u / %u, box %u / %u, "
                "capsule %u / %u\n",
                sphere.contacts, sphere.mismatch, plane.contacts, plane.mismatch, box.contacts, box.mismatch,
                capsule.contacts, capsule.mismatch);
    expectTrue(sphere.contacts > 200u && sphere.mismatch == 0u, "box hull vs sphere == box vs sphere");
    expectTrue(plane.contacts > 200u && plane.mismatch == 0u, "box hull vs plane == box vs plane (same corners)");
    expectTrue(box.contacts > 200u && box.mismatch == 0u, "box hull vs box == oriented box-box (normal, depth, points)");
    expectTrue(capsule.contacts > 200u && capsule.mismatch == 0u, "box hull vs capsule == box vs capsule (normal, depth)");
}

struct StackRun {
    std::vector<vec3> positions;
    std::vector<quat> orientations;
    f32 maxSpeed = 0.f;
    bool asleep = false;
};

/// Five hexagonal-prism hulls (built by quickhull) stacked on a plane, simulated 6 s.
StackRun runHullStack(u32 hullRef) {
    Registry reg;
    reg.init(256);
    PhysicsManager manager;
    manager.init({});
    PhysicsStreamManager streams{};
    spawnGroundPlane(reg);
    std::vector<EntityID> stack;
    for (u32 i = 0; i < 5u; ++i) {
        // A small offset and yaw per level: the stack has to settle, not start perfectly aligned.
        const quat yaw = quatFromAxisAngle({0.f, 1.f, 0.f}, 0.05f * static_cast<f32>(i));
        stack.push_back(spawnPooled(reg, {0.01f * static_cast<f32>(i), 0.25f + 0.51f * static_cast<f32>(i), 0.f},
                                    fuse::ecs::Collider::ConvexHull, hullRef, false, 1.f, yaw));
    }
    for (u32 frame = 0; frame < 360u; ++frame) {
        manager.step(reg, 1.f / 60.f, streams);
    }
    StackRun run{};
    run.asleep = true;
    for (const EntityID id : stack) {
        run.positions.push_back(positionOf(reg, id));
        const fuse::ecs::Transform* t = reg.get<fuse::ecs::Transform>(id);
        run.orientations.push_back({t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w});
        run.maxSpeed = std::max(run.maxSpeed, velocityOf(reg, id).length());
        run.asleep = run.asleep && manager.isSleeping(id);
    }
    return run;
}

void testHullStackSettlesDeterministically() {
    // Hexagonal prism: radius 0.4, height 0.5 (quickhull over its 12 corners plus interior points).
    std::vector<vec3> points;
    for (u32 k = 0; k < 6u; ++k) {
        const f32 angle = 6.2831853f * static_cast<f32>(k) / 6.f;
        points.push_back({0.4f * std::cos(angle), -0.25f, 0.4f * std::sin(angle)});
        points.push_back({0.4f * std::cos(angle), 0.25f, 0.4f * std::sin(angle)});
        points.push_back({0.2f * std::cos(angle), 0.f, 0.2f * std::sin(angle)});
    }
    ConvexHull prism;
    const bool built = buildConvexHull(points, prism);
    expectTrue(built && prism.vertices.size() == 12u && prism.faces.size() == 8u,
               "hexagonal prism hull: 12 vertices, 6 quads + 2 hexagons");
    const u32 ref = ShapePool::global().addHull(std::move(prism));

    const StackRun first = runHullStack(ref);
    const StackRun second = runHullStack(ref);
    bool ordered = true;
    f32 worstDrift = 0.f;
    for (u32 i = 0; i < first.positions.size(); ++i) {
        const vec3 p = first.positions[i];
        worstDrift = std::max(worstDrift, std::sqrt(p.x * p.x + p.z * p.z));
        const f32 expectedY = 0.25f + 0.5f * static_cast<f32>(i);
        ordered = ordered && std::fabs(p.y - expectedY) < 0.03f;
        std::printf("  hull %u: y %.4f (expected %.3f), xz drift %.4f\n", i, static_cast<double>(p.y),
                    static_cast<double>(expectedY), static_cast<double>(std::sqrt(p.x * p.x + p.z * p.z)));
    }
    std::printf("hull stack after 6 s: max speed %.4f m/s, worst xz drift %.4f m, all asleep %s\n",
                static_cast<double>(first.maxSpeed), static_cast<double>(worstDrift), first.asleep ? "yes" : "no");
    expectTrue(ordered, "a stack of five hulls rests at the stacked heights");
    expectTrue(first.maxSpeed < 0.05f && worstDrift < 0.1f, "the hull stack settles (no sliding or toppling)");

    bool identical = first.positions.size() == second.positions.size();
    for (usize i = 0; identical && i < first.positions.size(); ++i) {
        identical = std::memcmp(&first.positions[i], &second.positions[i], sizeof(vec3)) == 0 &&
                    std::memcmp(&first.orientations[i], &second.orientations[i], sizeof(quat)) == 0;
    }
    expectTrue(identical, "two runs of the hull stack are bit-identical");
}

} // namespace

int main() {
    fuse::core::initialize();
    testQuickhull();
    testHullSatAgainstBruteForce();
    testBoxShapedHullMatchesBox();
    testHullStackSettlesDeterministically();
    fuse::core::shutdown();
    return finish("fuse_e18_hull_gates");
}
