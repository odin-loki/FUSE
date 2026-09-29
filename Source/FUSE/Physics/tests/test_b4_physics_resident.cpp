// Resident physics pipeline gates (fuse/physics/resident/resident_physics.hpp), linked into
// fuse_b4_physics_kernel_gates:
//
// residentCpuGates() — runs everywhere. The resident launch sequence (device-count stages over capacity-
// sized grids, sentinel-padded radix sorts, plane pairs merged by sort + unique, the device narrowphase
// preflight / dispatch / finalize, the coloured solve over plain arrays) runs on CpuReference and
// CpuParallel over host memory and must equal the legacy paths bit for bit:
//   - broadphase == runBroadphaseIntoBuffer / runBroadphase2DIntoBuffer (pairs, order, counters) on 3D /
//     2D / layer / occupancy / small-table / mixed-shape scenes and on the 10k-body 3090 scene, with the
//     capacity growth path exercised; entries / cells / cell pairs == runBroadphaseKernels stats;
//   - narrowphase == the CPU narrowphase per pair (narrowphasePairContact, i.e. runNarrowphaseIntoBuffer)
//     and == collidePairs with a speculative margin, on spheres / boxes (axis-aligned and rotated) /
//     capsules / planes with static, sleeping, kinematic, trigger, massless and degenerate bodies;
//   - coloured solve == the PBDSolver colour loop (solveContactConstraint / solveDistanceConstraint per
//     colour, serial overflow list) for several iterations: predicted poses and every lambda.
//
// residentCudaBench(standalone) — needs a CUDA device (exit 77 / "FUSE_GATE_SKIP" otherwise). Uploads
// the plan workloads once, checks the device results against the CPU, and times resident buffers with
// CUDA events: broadphase of 10k bodies (target < 2 ms), narrowphase of 1k contact pairs (< 3 ms),
// 10 solver iterations over >= 10k contacts (< 5 ms). Targets are asserted only when
// fuse::core::timingBudgetsEnforced() (off under sanitizers / valgrind / FUSE_INSTRUMENTED_RUN).

#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/sanitizer.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/physics/broadphase/broadphase_kernel.hpp>
#include <fuse/physics/broadphase/broadphase_kernels.hpp>
#include <fuse/physics/broadphase/pair_buffer.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>
#include <fuse/physics/narrowphase/narrowphase_kernels.hpp>
#include <fuse/physics/resident/resident_physics.hpp>
#include <fuse/physics/rotation.hpp>
#include <fuse/physics/solver/constraint_accumulation.hpp>
#include <fuse/physics/solver/constraint_coloring.hpp>
#include <fuse/physics/solver/solver_work_buffers.hpp>

#include "physics_kernel_test_scenes.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

namespace {

using namespace fuse::physics;
using fuse::u32;
using fuse::u64;
namespace kernel = fuse::kernel;
namespace bp = fuse::physics::broadphase;
namespace np = fuse::physics::narrowphase;
namespace rs = fuse::physics::resident;
using test_scenes::Scene;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
        return;
    }
    std::printf("  ok  %s\n", message);
}

void setWorkers(u32 workers) {
    fuse::jobs::JobScheduler::instance().shutdown();
    fuse::jobs::JobScheduler::instance().initialize(workers);
}

template <typename T>
bool sameBits(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

template <typename T>
bool sameVectorBits(const std::vector<T>& a, const std::vector<T>& b, size_t count) {
    return a.size() >= count && b.size() >= count && (count == 0u || std::memcmp(a.data(), b.data(), count * sizeof(T)) == 0);
}

bool samePairBuffers(const bp::PairBufferSoA& a, const bp::PairBufferSoA& b) {
    return a.activeCount == b.activeCount && a.pairSlotCount == b.pairSlotCount && a.droppedCount == b.droppedCount &&
           a.bodyA.size() == b.bodyA.size() && sameVectorBits(a.bodyA, b.bodyA, a.bodyA.size()) &&
           sameVectorBits(a.bodyB, b.bodyB, a.bodyB.size()) && sameVectorBits(a.validFlags, b.validFlags, a.validFlags.size());
}

/// Every field bit for bit (padding bytes excluded).
bool sameManifold(const np::ContactManifold& a, const np::ContactManifold& b) {
    bool same = sameBits(a.contactNormal, b.contactNormal) && sameBits(a.minSeparation, b.minSeparation) &&
                a.bodyA == b.bodyA && a.bodyB == b.bodyB && a.pointCount == b.pointCount &&
                sameBits(a.warmNormalImpulse, b.warmNormalImpulse) && sameBits(a.warmTangentImpulse, b.warmTangentImpulse) &&
                sameBits(a.frictionBasis.tangent1, b.frictionBasis.tangent1) &&
                sameBits(a.frictionBasis.tangent2, b.frictionBasis.tangent2) && sameBits(a.contactPoint, b.contactPoint) &&
                sameBits(a.penetrationDepth, b.penetrationDepth) && a.valid == b.valid;
    for (u32 k = 0; k < np::kMaxContactPointsPerManifold; ++k) {
        same = same && sameBits(a.points[k].point, b.points[k].point) && sameBits(a.points[k].penetration, b.points[k].penetration);
    }
    return same;
}

f32 vecDiff(const vec3& a, const vec3& b) {
    return std::max({std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)});
}

/// Largest absolute difference over the manifold's floats (same bodies / point count required).
f32 manifoldDiff(const np::ContactManifold& a, const np::ContactManifold& b) {
    if (a.bodyA != b.bodyA || a.bodyB != b.bodyB || a.pointCount != b.pointCount || a.valid != b.valid) {
        return 1e30f;
    }
    f32 d = std::max({vecDiff(a.contactNormal, b.contactNormal), std::fabs(a.minSeparation - b.minSeparation),
                      vecDiff(a.frictionBasis.tangent1, b.frictionBasis.tangent1),
                      vecDiff(a.frictionBasis.tangent2, b.frictionBasis.tangent2), vecDiff(a.contactPoint, b.contactPoint),
                      std::fabs(a.penetrationDepth - b.penetrationDepth)});
    for (u32 k = 0; k < a.pointCount; ++k) {
        d = std::max({d, vecDiff(a.points[k].point, b.points[k].point),
                      std::fabs(a.points[k].penetration - b.points[k].penetration)});
    }
    return d;
}

bool sameManifoldLists(const std::vector<np::ContactManifold>& a, const std::vector<np::ContactManifold>& b,
                       u32* firstMismatch = nullptr) {
    if (a.size() != b.size()) {
        if (firstMismatch != nullptr) {
            *firstMismatch = static_cast<u32>(std::min(a.size(), b.size()));
        }
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (!sameManifold(a[i], b[i])) {
            if (firstMismatch != nullptr) {
                *firstMismatch = static_cast<u32>(i);
            }
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Legacy references
// ---------------------------------------------------------------------------------------------

void legacyBroadphase(const Scene& scene, const bp::SpatialHashParams& params, bool use2D, bp::PairBufferSoA& out) {
    bp::BroadphaseScratch scratch;
    out.setMaxCapacity(0u);
    if (use2D) {
        bp::runBroadphase2DIntoBuffer(scene.bodies, scene.shapes, params, out, scratch);
    } else {
        bp::runBroadphaseIntoBuffer(scene.bodies, scene.shapes, params, out, scratch);
    }
}

/// runNarrowphaseIntoBuffer's per-pair body, in pair order.
std::vector<np::ContactManifold> legacyPipelineContacts(const std::vector<bp::CandidatePair>& pairs, const Scene& scene) {
    std::vector<np::ContactManifold> out;
    for (const bp::CandidatePair& pair : pairs) {
        np::ContactManifold manifold;
        if (np::detail::narrowphasePairContact(pair, scene.bodies, scene.shapes, manifold)) {
            out.push_back(manifold);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Scenes
// ---------------------------------------------------------------------------------------------

/// Spheres, boxes (axis-aligned and rotated), capsules (upright and tilted), a ground plane, a tilted
/// plane, multi-shape bodies and every body flag the narrowphase preflight looks at.
Scene makeMixedScene(u32 count, u32 seed) {
    Scene s;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> jitter(-0.25f, 0.25f);
    std::uniform_real_distribution<float> angle(0.f, 3.f);
    const u32 ground = s.bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    s.shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);
    const u32 wall = s.bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC, 0x2u, 0xFFFFFFFFu);
    s.shapes.addShape(CollisionShapeType::Plane, wall, vec3{1.f, 0.2f, 0.f}.normalized(), -0.3f);
    const u32 side = static_cast<u32>(std::ceil(std::cbrt(static_cast<double>(count))));
    for (u32 i = 0; i < count; ++i) {
        const vec3 p{static_cast<float>(i % side) * 0.85f + jitter(rng),
                     0.4f + static_cast<float>((i / side) % side) * 0.85f + jitter(rng),
                     static_cast<float>(i / (side * side)) * 0.85f + jitter(rng)};
        u32 flags = 0u;
        f32 invMass = 1.f;
        switch (i % 23u) {
        case 3u: flags = RB_STATIC; invMass = 0.f; break;
        case 5u: flags = RB_SLEEPING; break;
        case 7u: flags = RB_KINEMATIC; invMass = 0.f; break;
        case 11u: flags = RB_TRIGGER; break;
        case 13u: invMass = 0.f; break; // massless dynamic
        case 17u: flags = RB_SLEEPING; break;
        default: break;
        }
        const u32 layer = (i % 7u == 0u) ? 0x4u : 0x1u;
        const u32 mask = (i % 11u == 0u) ? 0x1u : 0xFFFFFFFFu;
        const u32 body = s.bodies.addBody(p, invMass, flags, layer, mask);
        const quat tilt = quatFromAxisAngle({0.4f, 1.f, -0.3f}, angle(rng));
        switch (i % 5u) {
        case 0u:
            s.shapes.addShape(CollisionShapeType::Box, body, {0.4f, 0.35f, 0.3f});
            break;
        case 1u:
            s.shapes.addShape(CollisionShapeType::Box, body, {0.35f, 0.4f, 0.3f});
            s.bodies.orientations[body] = tilt;
            break;
        case 2u:
            s.shapes.addShape(CollisionShapeType::Capsule, body, {0.25f, i % 31u == 2u ? 0.f : 0.3f, 0.f});
            if (i % 2u == 0u) {
                s.bodies.orientations[body] = tilt;
            }
            break;
        default:
            s.shapes.addShape(CollisionShapeType::Sphere, body, {i % 37u == 4u ? 0.f : 0.45f, 0.f, 0.f});
            break;
        }
        if (i % 29u == 1u) {
            // Multi-shape body: the narrowphase uses the sphere (findShapeForBody's preference).
            s.shapes.addShape(CollisionShapeType::Sphere, body, {0.3f, 0.f, 0.f});
        }
    }
    // A capsule cluster (capsule-capsule / -box / -sphere / -plane contacts) away from the grid.
    for (u32 k = 0; k < 12u; ++k) {
        const vec3 p{-4.f + 0.45f * static_cast<f32>(k % 4u), 0.3f + 0.4f * static_cast<f32>(k / 4u),
                     -3.f + 0.2f * static_cast<f32>(k % 3u)};
        const u32 body = s.bodies.addBody(p, 1.f);
        if (k % 4u == 3u) {
            s.shapes.addShape(k % 8u == 3u ? CollisionShapeType::Box : CollisionShapeType::Sphere, body,
                              {0.25f, 0.25f, 0.25f});
        } else {
            s.shapes.addShape(CollisionShapeType::Capsule, body, {0.22f, 0.25f, 0.f});
            s.bodies.orientations[body] = quatFromAxisAngle({1.f, 0.2f * static_cast<f32>(k), 0.3f}, 0.5f * static_cast<f32>(k));
        }
    }
    // A ceiling plane created last, so plane pairs also come in (shape, plane) order.
    const f32 top = 0.4f + static_cast<f32>(side - 1u) * 0.85f;
    const u32 ceiling = s.bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    s.shapes.addShape(CollisionShapeType::Plane, ceiling, {0.f, -1.f, 0.f}, -(top + 0.2f));
    return s;
}

// ---------------------------------------------------------------------------------------------
// Solver problem (one PBDSolver substep: predicted poses, speculative contacts, anchors, colouring)
// ---------------------------------------------------------------------------------------------

struct SolverProblemData {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    SolverWorkBuffers work;
    std::vector<DistanceConstraint> links;
    ConstraintColoring coloring;
    f32 dt = 1.f / 240.f;

    rs::SolverProblem view() const {
        rs::SolverProblem p{};
        p.bodies = &bodies;
        p.invInertia = &work.bodyInvInertia();
        p.contacts = &work.contactManifolds();
        p.anchors = &work.contactAnchors();
        p.contactLambdas = &work.contactLambdas();
        p.pointLambdas = &work.contactPointLambdas();
        p.distances = &links;
        p.distanceLambdas = &work.distanceLambdas();
        p.coloring = &coloring;
        return p;
    }
};

/// Stacked columns (makeSolverScene), some tilted, moved one substep forward under gravity; contacts
/// from the broadphase + collidePairs(margin) at the predicted pose; soft links, plus `hubLinks` links on
/// one body (more than ConstraintColoring::kMaxColors fills the serial overflow list).
void buildSolverProblem(SolverProblemData& d, u32 columns, u32 height, u32 hubLinks) {
    test_scenes::SolverScene scene = test_scenes::makeSolverScene(columns, height);
    d.bodies = std::move(scene.bodies);
    d.shapes = std::move(scene.shapes);
    RigidBodySoA& b = d.bodies;
    const u32 n = b.count();
    for (u32 i = 1; i < n; i += 5u) {
        b.orientations[i] = quatFromAxisAngle({0.2f, 1.f, 0.1f}, 0.02f * static_cast<f32>(i % 7u));
    }
    const vec3 gravity{0.f, -9.81f, 0.f};
    for (u32 i = 0; i < n; ++i) {
        b.frictionStatic[i] = 0.4f + 0.05f * static_cast<f32>(i % 5u);
        if ((b.flags[i] & RB_STATIC) != 0u) {
            b.predictedPositions[i] = b.positions[i];
            b.predictedOrientations[i] = b.orientations[i];
            continue;
        }
        b.linearVelocities[i] = gravity * (0.05f + 0.01f * static_cast<f32>(i % 3u));
        b.predictedPositions[i] = b.positions[i] + b.linearVelocities[i] * d.dt + gravity * (d.dt * d.dt);
        b.predictedOrientations[i] = applyRotationVector(b.orientations[i], vec3{0.f, 0.01f, 0.f} * static_cast<f32>(i % 3u));
    }

    // Contacts at the predicted pose (PBDSolver::generateContacts swaps the pose the same way).
    std::swap(b.positions, b.predictedPositions);
    std::swap(b.orientations, b.predictedOrientations);
    bp::SpatialHashParams params{};
    params.cellSize = 2.f;
    params.tableSize = 4096u;
    bp::PairBufferSoA pairBuffer;
    bp::runBroadphaseIntoBuffer(b, d.shapes, params, pairBuffer);
    std::vector<bp::CandidatePair> pairs;
    pairBuffer.copyTo(pairs);
    std::vector<np::ContactManifold> manifolds;
    np::collidePairs(pairs, b, d.shapes, manifolds, 0.005f);
    std::swap(b.positions, b.predictedPositions);
    std::swap(b.orientations, b.predictedOrientations);

    d.links.clear();
    for (u32 i = 1; i + 1u < n; i += 13u) {
        DistanceConstraint link{};
        link.bodyA = i;
        link.bodyB = i + 1u;
        link.localAnchorA = {0.1f, 0.2f, 0.f};
        link.restLength = (b.positions[i] - b.positions[i + 1u]).length();
        link.compliance = 1e-4f;
        d.links.push_back(link);
    }
    for (u32 k = 0; k < hubLinks && k + 2u < n; ++k) {
        DistanceConstraint link{};
        link.bodyA = 1u;
        link.bodyB = n - 1u - k;
        link.restLength = (b.positions[1] - b.positions[n - 1u - k]).length() * 0.99f;
        link.compliance = 1e-3f;
        d.links.push_back(link);
    }

    d.work.init(n, static_cast<u32>(manifolds.size()) + 1u, static_cast<u32>(d.links.size()));
    std::vector<np::ContactManifold>& contacts = d.work.contactManifolds();
    for (const np::ContactManifold& m : manifolds) {
        if (m.valid) {
            contacts.push_back(m);
        }
    }
    std::vector<vec3>& inertia = d.work.bodyInvInertia();
    inertia.assign(n, vec3{});
    for (u32 s = 0; s < d.shapes.count(); ++s) {
        const u32 body = d.shapes.bodyIndices[s];
        inertia[body] = shapeInverseInertia(static_cast<CollisionShapeType>(d.shapes.types[s]), d.shapes.params[s],
                                            b.invMasses[body]);
    }
    d.work.ensureLambdaCapacity(static_cast<u32>(contacts.size()), static_cast<u32>(d.links.size()));
    std::vector<f32>& lambdas = d.work.contactLambdasMutable();
    for (u32 c = 0; c < contacts.size(); c += 3u) {
        lambdas[c] = 1e-5f * static_cast<f32>(c % 7u); // warm-started contacts
    }
    d.work.prepareContactPoints(b);
    d.coloring.build(b, contacts, d.links);
}

f32 effectiveInvMass(const RigidBodySoA& bodies, u32 index) {
    const u32 flags = bodies.flags[index];
    if ((flags & (RB_STATIC | RB_KINEMATIC | RB_SLEEPING)) != 0u) {
        return 0.f;
    }
    return bodies.invMasses[index];
}

/// The PBDSolver colour loop (pbd_solver.cpp SolveColorKernel + solveColored_) through the public
/// per-constraint functions: colours in order, the serial overflow list last.
void legacyColoredIterations(SolverProblemData& d, u32 iterations) {
    RigidBodySoA& bodies = d.bodies;
    SolverWorkBuffers& work = d.work;
    const std::vector<np::ContactManifold>& contacts = work.contactManifolds();
    const auto solveRef = [&](u32 ref) {
        if ((ref & ConstraintColoring::kDistanceBit) != 0u) {
            const u32 index = ref & ~ConstraintColoring::kDistanceBit;
            const DistanceConstraint& c = d.links[index];
            const f32 mA = effectiveInvMass(bodies, c.bodyA);
            const f32 mB = effectiveInvMass(bodies, c.bodyB);
            const ContactBody a{c.bodyA, mA, work.effectiveInvInertia(c.bodyA, mA)};
            const ContactBody bb{c.bodyB, mB, work.effectiveInvInertia(c.bodyB, mB)};
            solveDistanceConstraint(bodies, c, a, bb, d.dt, work.distanceLambda(index));
            return;
        }
        const np::ContactManifold& c = contacts[ref];
        const f32 mA = effectiveInvMass(bodies, c.bodyA);
        const f32 mB = effectiveInvMass(bodies, c.bodyB);
        const ContactBody a{c.bodyA, mA, work.effectiveInvInertia(c.bodyA, mA)};
        const ContactBody bb{c.bodyB, mB, work.effectiveInvInertia(c.bodyB, mB)};
        solveContactConstraint(bodies, work, ref, c, a, bb, d.dt, 0.f, work.contactLambda(ref));
    };
    for (u32 iter = 0; iter < iterations; ++iter) {
        for (u32 color = 0; color < d.coloring.colorCount; ++color) {
            for (u32 i = 0; i < d.coloring.colorSize(color); ++i) {
                solveRef(d.coloring.colorItems(color)[i]);
            }
        }
        for (u32 i = 0; i < d.coloring.overflowCount; ++i) {
            solveRef(d.coloring.colorItems(ConstraintColoring::kMaxColors)[i]);
        }
    }
}

struct SolveResult {
    RigidBodySoA bodies;
    std::vector<f32> contactLambdas;
    std::vector<f32> pointLambdas;
    std::vector<f32> distanceLambdas;
};

bool runResidentSolve(rs::ResidentPhysics& physics, const SolverProblemData& d, u32 iterations, SolveResult& out) {
    out.bodies = d.bodies;
    return physics.uploadSolver(d.view()) && physics.solveIterations(iterations, d.dt, 0.f) &&
           physics.downloadSolver(out.bodies, &out.contactLambdas, &out.pointLambdas, &out.distanceLambdas);
}

struct SolveCompare {
    bool bitExact = false;
    f32 maxPosition = 0.f;
    f32 maxLambda = 0.f;
};

SolveCompare compareSolve(const RigidBodySoA& refBodies, const std::vector<f32>& refContact,
                          const std::vector<f32>& refPoint, const std::vector<f32>& refDistance, const SolveResult& got) {
    SolveCompare c{};
    const size_t n = refBodies.count();
    c.bitExact = sameVectorBits(refBodies.predictedPositions, got.bodies.predictedPositions, n) &&
                 sameVectorBits(refBodies.predictedOrientations, got.bodies.predictedOrientations, n) &&
                 refContact.size() == got.contactLambdas.size() && sameVectorBits(refContact, got.contactLambdas, refContact.size()) &&
                 refPoint.size() == got.pointLambdas.size() && sameVectorBits(refPoint, got.pointLambdas, refPoint.size()) &&
                 refDistance.size() == got.distanceLambdas.size() &&
                 sameVectorBits(refDistance, got.distanceLambdas, refDistance.size());
    for (size_t i = 0; i < n && i < got.bodies.predictedPositions.size(); ++i) {
        c.maxPosition = std::max(c.maxPosition, vecDiff(refBodies.predictedPositions[i], got.bodies.predictedPositions[i]));
        const quat& qa = refBodies.predictedOrientations[i];
        const quat& qb = got.bodies.predictedOrientations[i];
        c.maxPosition = std::max({c.maxPosition, std::fabs(qa.x - qb.x), std::fabs(qa.y - qb.y), std::fabs(qa.z - qb.z),
                                  std::fabs(qa.w - qb.w)});
    }
    const auto lambdaDiff = [&](const std::vector<f32>& a, const std::vector<f32>& b) {
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
            c.maxLambda = std::max(c.maxLambda, std::fabs(a[i] - b[i]));
        }
        if (a.size() != b.size()) {
            c.maxLambda = 1e30f;
        }
    };
    lambdaDiff(refContact, got.contactLambdas);
    lambdaDiff(refPoint, got.pointLambdas);
    lambdaDiff(refDistance, got.distanceLambdas);
    return c;
}

// ---------------------------------------------------------------------------------------------
// CPU gates
// ---------------------------------------------------------------------------------------------

struct BroadphaseCase {
    const char* name;
    Scene scene;
    bp::SpatialHashParams params;
    bool use2D;
    bool parallelOnly;
};

void testResidentBroadphase() {
    bp::SpatialHashParams base{};
    base.cellSize = 1.f;
    base.tableSize = 4096u;
    std::vector<BroadphaseCase> cases;
    cases.push_back({"3D spheres+boxes+plane", test_scenes::makeScene3D(1000, 1), base, false, false});
    cases.push_back({"3D layers", test_scenes::makeScene3D(700, 2, true, true, true), base, false, false});
    bp::SpatialHashParams occupancy = base;
    occupancy.maxCellOccupancy = 4u;
    occupancy.cellSize = 0.4f;
    cases.push_back({"3D occupancy budget", test_scenes::makeScene3D(500, 4, false), occupancy, false, false});
    bp::SpatialHashParams coarse = base;
    coarse.cellSize = 3.f;
    coarse.tableSize = 97u;
    cases.push_back({"3D coarse cells, small table", test_scenes::makeScene3D(800, 5), coarse, false, false});
    cases.push_back({"2D circles+boxes", test_scenes::makeScene2D(1000, 6), base, true, false});
    cases.push_back({"mixed shapes + flags + tilted plane", makeMixedScene(1200, 9), base, false, false});
    cases.push_back({"singleton", test_scenes::makeScene3D(1, 8, false), base, false, false});
    bp::SpatialHashParams bench{};
    bench.cellSize = 2.f;
    bench.tableSize = 20000u;
    cases.push_back({"10k bodies (3090 benchmark scene)", test_scenes::makeScene3D(10000, 99), bench, false, true});

    for (const BroadphaseCase& c : cases) {
        bp::PairBufferSoA reference;
        legacyBroadphase(c.scene, c.params, c.use2D, reference);
        bp::BroadphaseKernelContext kernelContext;
        bp::PairBufferSoA kernelOut;
        bp::runBroadphaseKernels(c.scene.bodies, c.scene.shapes, c.params, c.use2D, kernelOut, kernelContext);
        for (kernel::Backend backend : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel}) {
            if (c.parallelOnly && backend == kernel::Backend::CpuReference) {
                continue;
            }
            for (u32 workers : {0u, 3u}) {
                if (backend == kernel::Backend::CpuReference && workers != 0u) {
                    continue;
                }
                setWorkers(workers);
                rs::ResidentPhysics physics;
                const bool initOk = physics.init(rs::Space::Host, backend);
                rs::Status status{};
                const bool fit = initOk && physics.uploadScene(c.scene.bodies, c.scene.shapes) &&
                                 physics.fitBroadphase(c.params, c.use2D, &status);
                bp::PairBufferSoA out;
                const bool downloaded = fit && physics.downloadPairs(out);
                // Steady state: the grown buffers are reused and the result is unchanged.
                rs::Status again{};
                bp::PairBufferSoA out2;
                const bool steady = downloaded && physics.broadphase(c.params, c.use2D) && physics.readStatus(again) &&
                                    again.overflow == 0u && physics.downloadPairs(out2) && samePairBuffers(out, out2);
                char label[256];
                std::snprintf(label, sizeof(label),
                              "[%s] resident broadphase == legacy (%s, %u workers, %u pairs, entries %u cells %u "
                              "cell pairs %u, capacities %u / %u)",
                              c.name, kernel::backend_name(backend), workers, reference.activeCount, status.entries,
                              status.cells, status.cellPairs, physics.capacities().entries, physics.capacities().pairs);
                expectTrue(downloaded && samePairBuffers(reference, out), label);
                std::snprintf(label, sizeof(label), "[%s] resident broadphase steady-state rerun (%s, %u workers)",
                              c.name, kernel::backend_name(backend), workers);
                expectTrue(steady, label);
                if (kernelContext.stats.usedKernels && reference.activeCount > 0u) {
                    std::snprintf(label, sizeof(label),
                                  "[%s] resident counters == runBroadphaseKernels stats (entries %u/%u, cells %u/%u, "
                                  "cell pairs %u/%u)",
                                  c.name, status.entries, kernelContext.stats.entries, status.cells,
                                  kernelContext.stats.cells, status.cellPairs, kernelContext.stats.cellPairs);
                    expectTrue(status.entries == kernelContext.stats.entries && status.cells == kernelContext.stats.cells &&
                                   status.cellPairs == kernelContext.stats.cellPairs && status.pairs == reference.activeCount,
                               label);
                }
            }
        }
    }
    setWorkers(2);

    // Capacity growth: the 10k scene overflows the first-run estimate (16 pair slots per shape) and
    // must have been grown and rerun inside fitBroadphase; a clipped run never writes past a buffer.
    {
        const Scene scene = test_scenes::makeScene3D(10000, 99);
        rs::ResidentPhysics physics;
        rs::Status first{};
        const bool ok = physics.init(rs::Space::Host, kernel::Backend::CpuParallel) &&
                        physics.uploadScene(scene.bodies, scene.shapes) && physics.broadphase(bench, false) &&
                        physics.readStatus(first);
        char label[200];
        std::snprintf(label, sizeof(label),
                      "resident broadphase reports overflow on an undersized first run (%u pair slots > capacity %u, "
                      "%u writes clipped)",
                      first.pairSlots, physics.capacities().pairs, first.clipped);
        expectTrue(ok && (first.overflow & rs::kOverflowPairs) != 0u && first.clipped > 0u, label);
        bp::PairBufferSoA reference;
        legacyBroadphase(scene, bench, false, reference);
        bp::PairBufferSoA out;
        const bool grown = ok && physics.growFromStatus(first) && physics.broadphase(bench, false) &&
                           physics.downloadPairs(out) && samePairBuffers(reference, out);
        expectTrue(grown, "resident broadphase after growFromStatus == legacy");
    }

    // More than 65536 bodies (packed 32-bit pair keys) and convex hulls are refused, not mis-computed.
    {
        Scene hull = test_scenes::makeScene3D(20, 3);
        hull.shapes.addShape(CollisionShapeType::ConvexHull, 5u, {0.3f, 0.3f, 0.3f});
        rs::ResidentPhysics physics;
        expectTrue(physics.init(rs::Space::Host) && !physics.uploadScene(hull.bodies, hull.shapes),
                   "resident uploadScene refuses convex hulls (they stay on the CPU narrowphase)");
    }
}

void testResidentNarrowphase() {
    bp::SpatialHashParams params{};
    params.cellSize = 1.f;
    params.tableSize = 4096u;
    struct Case {
        const char* name;
        Scene scene;
        bool coverage;
    };
    std::vector<Case> cases;
    cases.push_back({"spheres+boxes+plane (kernel gate scene)", test_scenes::makeScene3D(1000, 11), false});
    cases.push_back({"mixed shapes + flags", makeMixedScene(1500, 12), true});
    for (const Case& c : cases) {
        bp::PairBufferSoA pairBuffer;
        legacyBroadphase(c.scene, params, false, pairBuffer);
        std::vector<bp::CandidatePair> pairs;
        pairBuffer.copyTo(pairs);
        const std::vector<np::ContactManifold> reference = legacyPipelineContacts(pairs, c.scene);
        np::ContactBufferSoA contactBuffer;
        np::runNarrowphaseIntoBuffer(pairs, c.scene.bodies, c.scene.shapes, contactBuffer);
        std::vector<np::ContactManifold> speculative;
        np::collidePairs(pairs, c.scene.bodies, c.scene.shapes, speculative, 0.02f);
        expectTrue(reference.size() == contactBuffer.activeCount && reference.size() > 200u,
                   "narrowphase reference: per-pair body == runNarrowphaseIntoBuffer count");

        std::vector<bp::CandidatePair> reversed = pairs;
        for (bp::CandidatePair& pair : reversed) {
            std::swap(pair.bodyA, pair.bodyB);
        }
        const std::vector<np::ContactManifold> reversedReference = legacyPipelineContacts(reversed, c.scene);
        if (c.coverage) {
            // Every ordered primitive shape pair the dispatch distinguishes produced contacts.
            // Keyed by the pair's (bodyA, bodyB) shapes (the dispatch branch), over both pair orders.
            u32 seen[7][7]{};
            for (const std::vector<bp::CandidatePair>* list : {&pairs, &reversed}) {
                for (const bp::CandidatePair& pair : *list) {
                    np::ContactManifold m;
                    if (np::detail::narrowphasePairContact(pair, c.scene.bodies, c.scene.shapes, m)) {
                        const u32 sa = np::contact_shape_for_body(c.scene.shapes, pair.bodyA);
                        const u32 sb = np::contact_shape_for_body(c.scene.shapes, pair.bodyB);
                        ++seen[c.scene.shapes.types[sa]][c.scene.shapes.types[sb]];
                    }
                }
            }
            const CollisionShapeType prims[4] = {CollisionShapeType::Sphere, CollisionShapeType::Box,
                                                 CollisionShapeType::Capsule, CollisionShapeType::Plane};
            u32 missing = 0;
            for (CollisionShapeType a : prims) {
                for (CollisionShapeType b : prims) {
                    if (!(a == CollisionShapeType::Plane && b == CollisionShapeType::Plane) &&
                        seen[static_cast<u32>(a)][static_cast<u32>(b)] == 0u) {
                        std::printf("  (no contacts for shape pair %u-%u)\n", static_cast<u32>(a), static_cast<u32>(b));
                        ++missing;
                    }
                }
            }
            char coverage[160];
            std::snprintf(coverage, sizeof(coverage),
                          "[%s] contacts cover all 15 ordered sphere/box/capsule/plane pairs (%u missing)", c.name,
                          missing);
            expectTrue(missing == 0u, coverage);
        }

        for (kernel::Backend backend : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel}) {
            setWorkers(backend == kernel::Backend::CpuParallel ? 3u : 0u);
            rs::ResidentPhysics physics;
            bool ok = physics.init(rs::Space::Host, backend) && physics.uploadScene(c.scene.bodies, c.scene.shapes);
            // Chained: device pairs straight from the resident broadphase.
            std::vector<np::ContactManifold> chained;
            rs::Status status{};
            const bool chainOk = ok && physics.fitBroadphase(params, false) && physics.fitNarrowphase(0.f, false, &status) &&
                                 physics.downloadContacts(chained);
            u32 mismatch = 0;
            char label[256];
            const bool chainSame = chainOk && sameManifoldLists(reference, chained, &mismatch);
            std::snprintf(label, sizeof(label),
                          "[%s] resident broadphase -> narrowphase == CPU narrowphase bit for bit (%s, %zu contacts "
                          "from %zu pairs, first mismatch %u)",
                          c.name, kernel::backend_name(backend), reference.size(), pairs.size(), chainSame ? 0u : mismatch);
            expectTrue(chainSame, label);
            bool fieldsMatch = chainOk && chained.size() == contactBuffer.activeCount;
            for (u32 i = 0; fieldsMatch && i < chained.size(); ++i) {
                fieldsMatch = chained[i].bodyA == contactBuffer.bodyA[i] && chained[i].bodyB == contactBuffer.bodyB[i] &&
                              sameBits(chained[i].contactNormal, contactBuffer.contactNormals[i]) &&
                              sameBits(chained[i].penetrationDepth, contactBuffer.penetrationDepths[i]);
            }
            std::snprintf(label, sizeof(label), "[%s] resident contacts == runNarrowphaseIntoBuffer SoA (%s)", c.name,
                          kernel::backend_name(backend));
            expectTrue(fieldsMatch, label);

            // Uploaded pairs (a caller-owned pair list), pipeline and speculative (solver) modes.
            std::vector<np::ContactManifold> uploaded;
            const bool upOk = ok && physics.uploadPairs(pairs) && physics.fitNarrowphase(0.f, false) &&
                              physics.downloadContacts(uploaded);
            std::snprintf(label, sizeof(label), "[%s] resident narrowphase on uploaded pairs == CPU (%s)", c.name,
                          kernel::backend_name(backend));
            expectTrue(upOk && sameManifoldLists(reference, uploaded), label);
            std::vector<np::ContactManifold> swapped;
            const bool swapOk = ok && physics.uploadPairs(reversed) && physics.fitNarrowphase(0.f, false) &&
                                physics.downloadContacts(swapped);
            std::snprintf(label, sizeof(label),
                          "[%s] resident narrowphase on (bodyB, bodyA) pairs == CPU (%s, %zu contacts, flipped normals)",
                          c.name, kernel::backend_name(backend), reversedReference.size());
            expectTrue(swapOk && sameManifoldLists(reversedReference, swapped), label);
            ok = ok && physics.uploadPairs(pairs);
            std::vector<np::ContactManifold> spec;
            const bool specOk = ok && physics.fitNarrowphase(0.02f, true) && physics.downloadContacts(spec);
            mismatch = 0;
            const bool specSame = specOk && sameManifoldLists(speculative, spec, &mismatch);
            std::snprintf(label, sizeof(label),
                          "[%s] resident speculative narrowphase == collidePairs(margin 0.02) (%s, %zu manifolds, first "
                          "mismatch %u)",
                          c.name, kernel::backend_name(backend), speculative.size(), specSame ? 0u : mismatch);
            expectTrue(specSame, label);
        }
    }
    setWorkers(2);
}

void testResidentSolver() {
    SolverProblemData problem;
    buildSolverProblem(problem, 8u, 6u, 70u);
    char label[256];
    std::snprintf(label, sizeof(label),
                  "solver problem: %zu contacts, %zu distance constraints, %u colours + %u overflow constraints",
                  problem.work.contactManifolds().size(), problem.links.size(), problem.coloring.colorCount,
                  problem.coloring.overflowCount);
    expectTrue(problem.work.contactManifolds().size() > 100u && problem.coloring.colorCount >= 2u &&
                   problem.coloring.overflowCount > 0u,
               label);
    std::printf("resident solver gate: %s\n", label);

    constexpr u32 kIterations = 6u;
    SolverProblemData reference = problem;
    legacyColoredIterations(reference, kIterations);
    for (kernel::Backend backend : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel}) {
        for (u32 workers : {0u, 3u}) {
            setWorkers(workers);
            rs::ResidentPhysics physics;
            SolveResult got;
            const bool ok = physics.init(rs::Space::Host, backend) && runResidentSolve(physics, problem, kIterations, got);
            const SolveCompare cmp =
                compareSolve(reference.bodies, reference.work.contactLambdas(), reference.work.contactPointLambdas(),
                             reference.work.distanceLambdas(), got);
            std::snprintf(label, sizeof(label),
                          "resident coloured solve == PBDSolver colour loop bit for bit (%s, %u workers, %u iterations; "
                          "max |dx| %.3g, max |dlambda| %.3g)",
                          kernel::backend_name(backend), workers, kIterations, static_cast<double>(cmp.maxPosition),
                          static_cast<double>(cmp.maxLambda));
            expectTrue(ok && cmp.bitExact, label);
        }
    }
    setWorkers(2);
    // The problem actually moves something (the comparison above is not vacuous).
    f32 moved = 0.f;
    for (u32 i = 0; i < problem.bodies.count(); ++i) {
        moved = std::max(moved, vecDiff(problem.bodies.predictedPositions[i], reference.bodies.predictedPositions[i]));
    }
    expectTrue(moved > 1e-5f, "coloured iterations move the predicted poses");
}

// ---------------------------------------------------------------------------------------------
// CUDA benchmark helpers
// ---------------------------------------------------------------------------------------------

struct Timing {
    double median = 0.0;
    double fastest = 0.0;
    double slowest = 0.0;
};

template <typename Fn>
Timing timeEvents(rs::ResidentPhysics& physics, u32 runs, Fn&& body) {
    std::vector<double> t;
    for (u32 i = 0; i < runs; ++i) {
        physics.timerBegin();
        body();
        const double ms = physics.timerEndMs();
        if (ms >= 0.0) {
            t.push_back(ms);
        }
    }
    Timing out{};
    if (!t.empty()) {
        std::sort(t.begin(), t.end());
        out.median = t[t.size() / 2u];
        out.fastest = t.front();
        out.slowest = t.back();
    }
    return out;
}

template <typename Fn>
double timeWallMedian(u32 runs, Fn&& body) {
    std::vector<double> t;
    for (u32 i = 0; i < runs; ++i) {
        const auto start = std::chrono::steady_clock::now();
        body();
        t.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2u];
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Entry points (called from test_b4_physics_kernel_gates.cpp)
// ---------------------------------------------------------------------------------------------

int residentCpuGates() {
    g_failures = 0;
    testResidentBroadphase();
    testResidentNarrowphase();
    testResidentSolver();
    std::printf("resident pipeline CPU gates: %s\n", g_failures == 0 ? "passed" : "FAILED");
    return g_failures;
}

/// The plan workloads on resident buffers. cuda == false runs the identical flow on host memory
/// (CpuParallel, host clock, targets not asserted): a dry run of the benchmark code without a GPU.
int residentBench(bool cuda, bool standalone) {
    if (cuda && !rs::ResidentPhysics::cudaAvailable()) {
        if (standalone) {
            std::printf("FUSE_GATE_SKIP: no CUDA device (the resident physics benchmarks run on the RTX 3090)\n");
            return 77;
        }
        return EXIT_SUCCESS;
    }
    g_failures = 0;
    setWorkers(2);
    const bool enforce = cuda && fuse::core::timingBudgetsEnforcedNoted();
    const u32 kRuns = cuda ? 31u : 3u;
    rs::ResidentPhysics gpu;
    rs::ResidentPhysics cpu; // reference for the device results (== legacy per residentCpuGates)
    if (!gpu.init(cuda ? rs::Space::Cuda : rs::Space::Host, kernel::Backend::CpuParallel) ||
        !cpu.init(rs::Space::Host, kernel::Backend::CpuParallel)) {
        std::fprintf(stderr, "FAIL: resident init\n");
        return EXIT_FAILURE;
    }
    if (cuda) {
        std::printf("resident CUDA benchmarks (CUDA events on resident buffers, median of %u; copies only where stated):\n",
                    kRuns);
    } else {
        std::printf("resident benchmark dry run on host memory (CpuParallel, host clock, median of %u; targets not "
                    "asserted):\n",
                    kRuns);
    }
    char label[256];
    const char* where = cuda ? "CUDA" : "host";

    // ---- Broadphase: 10k bodies (the 2026-09-25 scene: cell size 2, table 20000) -------------------
    const Scene scene = test_scenes::makeScene3D(10000, 99);
    bp::SpatialHashParams params{};
    params.cellSize = 2.f;
    params.tableSize = 20000u;
    bp::PairBufferSoA reference;
    legacyBroadphase(scene, params, false, reference);
    rs::Status status{};
    bp::PairBufferSoA gpuPairs;
    const bool bpOk = gpu.uploadScene(scene.bodies, scene.shapes) && gpu.fitBroadphase(params, false, &status) &&
                      gpu.downloadPairs(gpuPairs);
    std::snprintf(label, sizeof(label), "%s resident broadphase == legacy pairs (%u pairs, %u cell pairs, %u entries)",
                  where, reference.activeCount, status.cellPairs, status.entries);
    expectTrue(bpOk && samePairBuffers(reference, gpuPairs), label);
    for (u32 i = 0; i < 3u; ++i) {
        gpu.broadphase(params, false);
    }
    const Timing bpTime = timeEvents(gpu, kRuns, [&] { gpu.broadphase(params, false); });
    gpu.timerBegin();
    for (u32 i = 0; i < kRuns; ++i) {
        gpu.broadphase(params, false);
    }
    const double bpBatch = gpu.timerEndMs() / kRuns;
    rs::Status after{};
    std::snprintf(label, sizeof(label), "%s resident broadphase: timed runs did not overflow", where);
    expectTrue(gpu.readStatus(after) && after.overflow == 0u && after.pairs == reference.activeCount, label);
    const double bpWall = timeWallMedian(kRuns, [&] {
        gpu.uploadBodyState(scene.bodies);
        gpu.broadphase(params, false);
        gpu.downloadPairs(gpuPairs);
    });
    std::printf("  broadphase 10k bodies: %.3f ms median (min %.3f, max %.3f), %.3f ms/call back to back; %u pairs; "
                "wall incl. body upload + pair download into a PairBufferSoA %.3f ms  [target < 2 ms]\n",
                bpTime.median, bpTime.fastest, bpTime.slowest, bpBatch, after.pairs, bpWall);
    if (enforce) {
        std::snprintf(label, sizeof(label), "broadphase 10k bodies < 2 ms on the device (CUDA events): %.3f ms",
                      bpTime.median);
        expectTrue(bpTime.median > 0.0 && bpTime.median < 2.0, label);
    }
    {
        // Informational: the same bodies with a cell size matched to the shapes (fewer candidate pairs).
        bp::SpatialHashParams tuned = params;
        tuned.cellSize = 1.f;
        tuned.tableSize = 32768u;
        rs::Status tunedStatus{};
        if (gpu.fitBroadphase(tuned, false, &tunedStatus)) {
            const Timing t = timeEvents(gpu, kRuns, [&] { gpu.broadphase(tuned, false); });
            std::printf("  broadphase 10k bodies, cell size 1 m: %.3f ms median; %u pairs\n", t.median, tunedStatus.pairs);
        }
    }

    // ---- Narrowphase: 1k contact pairs -----------------------------------------------------------
    std::vector<bp::CandidatePair> allPairs;
    reference.copyTo(allPairs);
    std::vector<bp::CandidatePair> contactPairs;
    for (const bp::CandidatePair& pair : allPairs) {
        np::ContactManifold m;
        if (np::detail::narrowphasePairContact(pair, scene.bodies, scene.shapes, m)) {
            contactPairs.push_back(pair);
            if (contactPairs.size() == 1000u) {
                break;
            }
        }
    }
    const std::vector<np::ContactManifold> npReference = legacyPipelineContacts(contactPairs, scene);
    std::vector<np::ContactManifold> npGpu;
    const bool npOk = gpu.uploadPairs(contactPairs) && gpu.fitNarrowphase(0.f, false) && gpu.downloadContacts(npGpu);
    u32 exact = 0;
    f32 worst = 0.f;
    for (size_t i = 0; npOk && i < npGpu.size() && i < npReference.size(); ++i) {
        exact += sameManifold(npReference[i], npGpu[i]) ? 1u : 0u;
        worst = std::max(worst, manifoldDiff(npReference[i], npGpu[i]));
    }
    std::snprintf(label, sizeof(label),
                  "%s resident narrowphase == CPU on 1k contact pairs (%zu/%zu manifolds, %u bit-exact, max |diff| %.3g; "
                  "tolerance 1e-5)",
                  where, npGpu.size(), npReference.size(), exact, static_cast<double>(worst));
    expectTrue(npOk && npGpu.size() == npReference.size() && worst <= 1e-5f, label);
    std::printf("  %s\n", label);
    const Timing npTime = timeEvents(gpu, kRuns, [&] { gpu.narrowphase(0.f, false); });
    std::printf("  narrowphase 1k contact pairs: %.3f ms median (min %.3f, max %.3f)  [target < 3 ms]\n", npTime.median,
                npTime.fastest, npTime.slowest);
    if (enforce) {
        std::snprintf(label, sizeof(label), "narrowphase 1k contact pairs < 3 ms on the device (CUDA events): %.3f ms",
                      npTime.median);
        expectTrue(npTime.median > 0.0 && npTime.median < 3.0, label);
    }
    {
        // Informational: the whole 10k-body frame front end resident (broadphase -> narrowphase).
        std::vector<np::ContactManifold> frame;
        rs::Status s{};
        if (gpu.fitBroadphase(params, false) && gpu.fitNarrowphase(0.f, false, &s) && gpu.downloadContacts(frame)) {
            const std::vector<np::ContactManifold> frameRef = legacyPipelineContacts(allPairs, scene);
            u32 frameExact = 0;
            for (size_t i = 0; i < frame.size() && i < frameRef.size(); ++i) {
                frameExact += sameManifold(frameRef[i], frame[i]) ? 1u : 0u;
            }
            std::snprintf(label, sizeof(label), "%s resident broadphase -> narrowphase contact count == CPU (%zu)", where,
                          frameRef.size());
            expectTrue(frame.size() == frameRef.size(), label);
            const Timing all = timeEvents(gpu, kRuns, [&] { gpu.narrowphase(0.f, false); });
            const Timing chain = timeEvents(gpu, kRuns, [&] {
                gpu.broadphase(params, false);
                gpu.narrowphase(0.f, false);
            });
            std::printf("  narrowphase of all %u broadphase pairs: %.3f ms (%zu contacts, %u bit-exact); broadphase + "
                        "narrowphase back to back %.3f ms\n",
                        s.pairs, all.median, frame.size(), frameExact, chain.median);
        }
    }

    // ---- Solver: 10 iterations over >= 10k contacts ----------------------------------------------
    SolverProblemData problem;
    buildSolverProblem(problem, 46u, 10u, 0u);
    const u32 contactCount = static_cast<u32>(problem.work.contactManifolds().size());
    SolveResult gpuSolve;
    SolveResult cpuSolve;
    const bool solveOk = runResidentSolve(gpu, problem, 10u, gpuSolve) && runResidentSolve(cpu, problem, 10u, cpuSolve);
    const SolveCompare cmp = compareSolve(cpuSolve.bodies, cpuSolve.contactLambdas, cpuSolve.pointLambdas,
                                          cpuSolve.distanceLambdas, gpuSolve);
    std::snprintf(label, sizeof(label),
                  "%s resident solve == CPU after 10 iterations (%u contacts, %u colours; bit-exact %s, max |dx| %.3g, "
                  "max |dlambda| %.3g; tolerance 1e-4)",
                  where, contactCount, problem.coloring.colorCount, cmp.bitExact ? "yes" : "no",
                  static_cast<double>(cmp.maxPosition), static_cast<double>(cmp.maxLambda));
    expectTrue(solveOk && contactCount >= 10000u && cmp.maxPosition <= 1e-4f && cmp.maxLambda <= 1e-4f, label);
    std::printf("  %s\n", label);
    gpu.uploadSolver(problem.view());
    const Timing solveTime = timeEvents(gpu, kRuns, [&] { gpu.solveIterations(10u, problem.dt, 0.f); });
    std::printf("  solver 10 iterations x %u contacts: %.3f ms median (min %.3f, max %.3f), %.3f ms/iteration  "
                "[target < 5 ms]\n",
                contactCount, solveTime.median, solveTime.fastest, solveTime.slowest, solveTime.median / 10.0);
    if (enforce) {
        std::snprintf(label, sizeof(label), "solver 10 iterations over 10k contacts < 5 ms on the device: %.3f ms",
                      solveTime.median);
        expectTrue(solveTime.median > 0.0 && solveTime.median < 5.0, label);
    }

    std::printf("resident %s: %s\n", cuda ? "CUDA benchmarks" : "benchmark dry run", g_failures == 0 ? "passed" : "FAILED");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int residentCudaBench(bool standalone) { return residentBench(true, standalone); }

int residentHostBench() { return residentBench(false, true); }
