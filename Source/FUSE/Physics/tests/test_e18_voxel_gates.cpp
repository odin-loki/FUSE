// E18 part 3b (UNI-B4-VOX-1): voxel and SDF collision shapes on the one shared SVO type.
//  - physics VoxelVolume is stored in the Scene SVO: every get / solid count equals a dense reference over
//    random set / fill / carve, SVO::readBox equals per-voxel reads, SVO ray casts see the same voxels
//  - a destructible volume is a static Voxel body: a sphere rests on the voxel floor, then falls through
//    a carved hole (the shape refreshes on carve and the resting body is woken)
//  - a box rests on an SDF shape (analytic rounded box through the SdfSampler interface) and a sphere on the
//    Scene SVO's distance field (SvoSdfSampler)
//  - an A-SDF brick (AsdfBrickSampler) trilinearly interpolates its lattice and, outside the brick, returns
//    the distance to the brick AABB plus the edge sample
//  - PhysicsManager::rayCast / shapeCast hit voxel and SDF bodies; two runs are bit-identical
#include "e18_test_common.hpp"

#include <fuse/core/init.hpp>
#include <fuse/physics/destruction/voxel_damage.hpp>
#include <fuse/physics/destruction/voxel_res.hpp>
#include <fuse/physics/queries/shape_queries.hpp>
#include <fuse/physics/shapes/sdf_sampler.hpp>
#include <fuse/physics/shapes/shape_pool.hpp>
#include <fuse/physics/spatial/svo.hpp>
#include <fuse/scene/svo.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

using namespace e18;

namespace {

constexpr f32 kDt = 1.f / 60.f;

void testVoxelVolumeOnSvo() {
    Rng rng(21u);
    const ivec3 dims{37, 21, 29};
    VoxelVolume volume;
    volume.init({-1.f, 0.5f, 2.f}, 0.1f, dims);
    std::vector<u8> reference(static_cast<usize>(dims.x) * dims.y * dims.z, 0u);
    const auto at = [&](ivec3 v) -> u8& { return reference[(static_cast<usize>(v.z) * dims.y + v.y) * dims.x + v.x]; };
    for (u32 op = 0; op < 400u; ++op) {
        const u32 kind = rng.next() % 3u;
        if (kind == 0u) {
            const ivec3 v{static_cast<s32>(rng.next() % 40u) - 1, static_cast<s32>(rng.next() % 24u) - 1,
                          static_cast<s32>(rng.next() % 32u) - 1};
            const u8 material = static_cast<u8>(rng.next() % 4u);
            volume.set(v, material);
            if (volume.inBounds(v)) {
                at(v) = material;
            }
        } else if (kind == 1u) {
            const ivec3 a{static_cast<s32>(rng.next() % 40u) - 2, static_cast<s32>(rng.next() % 24u) - 2,
                          static_cast<s32>(rng.next() % 32u) - 2};
            const ivec3 b{a.x + static_cast<s32>(rng.next() % 12u), a.y + static_cast<s32>(rng.next() % 12u),
                          a.z + static_cast<s32>(rng.next() % 12u)};
            const u8 material = static_cast<u8>(rng.next() % 3u);
            volume.fill(a, b, material);
            for (s32 z = std::max(a.z, 0); z <= std::min(b.z, dims.z - 1); ++z) {
                for (s32 y = std::max(a.y, 0); y <= std::min(b.y, dims.y - 1); ++y) {
                    for (s32 x = std::max(a.x, 0); x <= std::min(b.x, dims.x - 1); ++x) {
                        at({x, y, z}) = material;
                    }
                }
            }
        } else {
            const vec3 centre = volume.origin() + vec3{rng.range(0.f, 3.7f), rng.range(0.f, 2.1f), rng.range(0.f, 2.9f)};
            const f32 radius = rng.range(0.05f, 0.6f);
            u32 expected = 0;
            for (s32 z = 0; z < dims.z; ++z) {
                for (s32 y = 0; y < dims.y; ++y) {
                    for (s32 x = 0; x < dims.x; ++x) {
                        if (at({x, y, z}) != 0u && (volume.voxelCenter({x, y, z}) - centre).length() < radius) {
                            at({x, y, z}) = 0u;
                            ++expected;
                        }
                    }
                }
            }
            const u32 removed = volume.carve(centre, radius);
            if (removed != expected) {
                std::printf("carve removed %u, expected %u\n", removed, expected);
                expectTrue(false, "carve count matches the reference");
            }
        }
    }
    u32 mismatches = 0;
    u32 solid = 0;
    for (s32 z = 0; z < dims.z; ++z) {
        for (s32 y = 0; y < dims.y; ++y) {
            for (s32 x = 0; x < dims.x; ++x) {
                const u8 expected = at({x, y, z});
                solid += expected != 0u ? 1u : 0u;
                mismatches += volume.get({x, y, z}) != expected ? 1u : 0u;
                mismatches += volume.svo().get(fuse::scene::ivec3(x, y, z)) != expected ? 1u : 0u;
            }
        }
    }
    // readBox (one octree walk per brick) == per-voxel reads, including an out-of-range apron.
    std::vector<u32> box;
    volume.svo().readBox(fuse::scene::ivec3(-3, -2, -1), fuse::scene::ivec3(dims.x + 6, dims.y + 4, dims.z + 2), box);
    u32 boxMismatch = 0;
    usize i = 0;
    for (s32 z = -1; z < dims.z + 1; ++z) {
        for (s32 y = -2; y < dims.y + 2; ++y) {
            for (s32 x = -3; x < dims.x + 3; ++x, ++i) {
                boxMismatch += box[i] != volume.get({x, y, z}) ? 1u : 0u;
            }
        }
    }
    std::printf("VoxelVolume on the Scene SVO: %u solid voxels, %u get mismatches vs dense reference, solid count %u "
                "(reference %u), readBox mismatches %u, SVO nodes %zu bricks %zu\n",
                solid, mismatches, volume.solidCount(), solid, boxMismatch, volume.svo().nodeCount(),
                volume.svo().brickCount());
    expectTrue(mismatches == 0u && volume.solidCount() == solid, "VoxelVolume storage == dense reference (SVO-backed)");
    expectTrue(boxMismatch == 0u, "SVO::readBox == per-voxel reads");

    // The SVO's ray cast sees the physics voxels (same storage).
    u32 rayMismatch = 0;
    for (u32 r = 0; r < 500u; ++r) {
        const vec3 origin = volume.origin() + vec3{rng.range(0.f, 3.7f), 3.f, rng.range(0.f, 2.9f)};
        fuse::scene::ivec3 voxel{};
        fuse::scene::vec3 normal{};
        f32 t = 0.f;
        const bool hit = volume.svo().rayCast(fuse::scene::vec3(origin.x, origin.y, origin.z),
                                              fuse::scene::vec3(0.f, -1.f, 0.f), 10.f, voxel, normal, t);
        // Brute force: first solid voxel down the column.
        const ivec3 column = volume.voxelAt(origin);
        s32 top = -1;
        for (s32 y = dims.y - 1; y >= 0 && top < 0; --y) {
            top = volume.get({column.x, y, column.z}) != 0u ? y : -1;
        }
        const bool expected = top >= 0;
        rayMismatch += (hit != expected || (hit && voxel.y != top)) ? 1u : 0u;
    }
    expectTrue(rayMismatch == 0u, "the SVO ray cast walks the physics voxels");
}

struct VoxelRun {
    vec3 restPosition{};
    bool restAsleep = false;
    vec3 afterCarve{};
    f32 rayDistance = 0.f;
    bool rayHitVolume = false;
};

VoxelRun runSphereOnVoxels() {
    Registry reg;
    reg.init(256);
    PhysicsManager manager;
    manager.init({});
    PhysicsStreamManager streams{};
    spawnGroundPlane(reg, -3.f);
    // A 4 x 0.5 x 4 m slab of 0.1 m voxels (top at y = 0.5), registered as a destructible.
    VoxelVolume slab;
    slab.init({-2.f, 0.f, -2.f}, 0.1f, {40, 5, 40});
    slab.fill({0, 0, 0}, {39, 4, 39}, 1u);
    const EntityID floor = reg.create();
    manager.addDestructible(floor, slab, VoxelMaterial{});
    const EntityID ball = spawnBody(reg, {0.05f, 1.2f, 0.07f}, fuse::ecs::Collider::Sphere, {0.3f, 0.f, 0.f, 0.f}, false);
    for (u32 frame = 0; frame < 180u; ++frame) {
        manager.step(reg, kDt, streams);
    }
    VoxelRun run{};
    run.restPosition = positionOf(reg, ball);
    run.restAsleep = manager.isSleeping(ball);
    EntityID hit{};
    vec3 normal{};
    f32 t = 0.f;
    run.rayHitVolume = manager.rayCast({1.5f, 3.f, 1.5f}, {0.f, -1.f, 0.f}, 10.f, hit, normal, t) && hit == floor;
    run.rayDistance = t;

    DestructionEvent carve{};
    carve.target = floor;
    carve.impactPoint = {0.05f, 0.5f, 0.07f};
    carve.impactNormal = {0.f, 1.f, 0.f};
    carve.impulse = 10.f;
    carve.carveRadius = 0.65f;
    manager.pushDestructionEvent(carve);
    for (u32 frame = 0; frame < 180u; ++frame) {
        manager.step(reg, kDt, streams);
    }
    run.afterCarve = positionOf(reg, ball);
    return run;
}

void testSphereOnVoxelFloor() {
    const VoxelRun first = runSphereOnVoxels();
    std::printf("sphere on a voxel floor: y %.4f (expected 0.800), asleep %s; ray down hits the volume %s at %.4f "
                "(expected 2.5); after carving a 0.65 m crater under it: y %.3f\n",
                static_cast<double>(first.restPosition.y), first.restAsleep ? "yes" : "no",
                first.rayHitVolume ? "yes" : "no", static_cast<double>(first.rayDistance),
                static_cast<double>(first.afterCarve.y));
    expectTrue(std::fabs(first.restPosition.y - 0.8f) < 0.01f, "a sphere rests on the voxel floor");
    expectTrue(first.rayHitVolume && std::fabs(first.rayDistance - 2.5f) < 1e-4f,
               "PhysicsManager::rayCast hits the destructible's Voxel body");
    expectTrue(first.afterCarve.y < -2.5f, "the sphere falls through the carved hole (shape refreshed, body woken)");

    const VoxelRun second = runSphereOnVoxels();
    expectTrue(std::memcmp(&first.restPosition, &second.restPosition, sizeof(vec3)) == 0 &&
                   std::memcmp(&first.afterCarve, &second.afterCarve, sizeof(vec3)) == 0,
               "two voxel runs are bit-identical");
}

struct SdfRun {
    vec3 box{};
    vec3 boxStart{};
    f32 boxSpeed = 0.f;
    vec3 sphere{};
};

SdfRun runOnSdf(u32 floorRef, u32 svoRef) {
    Registry reg;
    reg.init(256);
    PhysicsManager manager;
    manager.init({});
    PhysicsStreamManager streams{};
    spawnPooled(reg, {0.f, -0.5f, 0.f}, fuse::ecs::Collider::SdfMesh, floorRef, true);
    spawnPooled(reg, {}, fuse::ecs::Collider::SdfMesh, svoRef, true);
    const EntityID box = spawnBody(reg, {0.1f, 1.f, -0.2f}, fuse::ecs::Collider::Box, {0.4f, 0.3f, 0.4f, 0.f}, false);
    const EntityID sphere = spawnBody(reg, {10.05f, 1.5f, 0.05f}, fuse::ecs::Collider::Sphere, {0.3f, 0.f, 0.f, 0.f}, false);
    SdfRun run{};
    for (u32 frame = 0; frame < 600u; ++frame) {
        manager.step(reg, kDt, streams);
        if (frame == 119u) {
            run.boxStart = positionOf(reg, box);
        }
    }
    run.box = positionOf(reg, box);
    run.boxSpeed = velocityOf(reg, box).length();
    run.sphere = positionOf(reg, sphere);
    return run;
}

void testBodiesOnSdf() {
    // Analytic rounded box floor, top at y = 0 once placed at y = -0.5.
    const u32 floorRef =
        ShapePool::global().addSdf(std::make_shared<AnalyticSdf>(AnalyticSdfKind::Box, vec3{6.f, 0.5f, 6.f}, 0.05f));
    // Scene SVO with stored distances: a 2 x 1 x 2 m block around x = 10 (top at y = 1).
    auto svo = std::make_shared<fuse::scene::SVO>();
    fuse::scene::SVODesc desc{};
    desc.origin = fuse::scene::vec3(8.f, -1.f, -2.f);
    desc.rootSize = 6.4f; // 64 voxels of 0.1 m
    desc.maxDepth = 6;
    desc.storeSdf = true;
    svo->init(desc);
    svo->fill(fuse::scene::ivec3(10, 0, 10), fuse::scene::ivec3(29, 19, 29), 1u);
    const u32 svoRef = ShapePool::global().addSdf(
        std::make_shared<SvoSdfSampler>(svo, vec3{8.f, -1.f, -2.f}, vec3{14.4f, 5.4f, 4.4f}));

    const SdfRun first = runOnSdf(floorRef, svoRef);
    const f32 drift = (first.box - first.boxStart).length();
    std::printf("box on an SDF (rounded box field): y %.4f (expected 0.300), drift 2..10 s %.6f m, speed %.5f; sphere "
                "on the SVO distance field: y %.4f (expected ~1.3)\n",
                static_cast<double>(first.box.y), static_cast<double>(drift), static_cast<double>(first.boxSpeed),
                static_cast<double>(first.sphere.y));
    expectTrue(std::fabs(first.box.y - 0.3f) < 0.01f, "a box rests on an SDF shape");
    expectTrue(drift < 0.005f && first.boxSpeed < 0.02f, "the box on the SDF does not drift");
    // The SVO field's zero crossing sits on the block surface within half a voxel.
    expectTrue(std::fabs(first.sphere.y - 1.3f) < 0.06f, "a sphere rests on the Scene SVO's distance field");

    const SdfRun second = runOnSdf(floorRef, svoRef);
    expectTrue(std::memcmp(&first.box, &second.box, sizeof(vec3)) == 0 &&
                   std::memcmp(&first.sphere, &second.sphere, sizeof(vec3)) == 0,
               "two SDF runs are bit-identical");

    // Shape casts against the SDF: a sphere dropped onto the analytic floor.
    fuse::physics::narrowphase::ShapeInstance caster{};
    caster.type = CollisionShapeType::Sphere;
    caster.params = {0.25f, 0.f, 0.f};
    caster.position = {1.f, 3.f, 1.f};
    fuse::physics::narrowphase::ShapeInstance target{};
    target.type = CollisionShapeType::SdfMesh;
    target.position = {0.f, -0.5f, 0.f};
    target.shapeRef = floorRef;
    ShapeCastHit hit{};
    const bool cast = shapeCast(caster, {0.f, -1.f, 0.f}, 10.f, target, hit);
    std::printf("sphere cast onto the SDF floor: %s at %.4f (expected 2.75), normal y %.4f\n", cast ? "hit" : "miss",
                static_cast<double>(hit.distance), static_cast<double>(hit.normal.y));
    expectTrue(cast && std::fabs(hit.distance - 2.75f) < 1e-3f && hit.normal.y > 0.999f,
               "sphere casts against an SDF stop at the surface");
}

void testAsdfBrickSampler() {
    const auto near = [](f32 got, f32 expect, const char* message) {
        if (!std::isfinite(got) || !(std::fabs(got - expect) <= 1e-4f)) {
            std::printf("  %s: got %.7g expected %.7g\n", message, static_cast<double>(got), static_cast<double>(expect));
        }
        expectTrue(std::isfinite(got) && std::fabs(got - expect) <= 1e-4f, message);
    };

    // Field = lattice x on the unit brick [0, 1]^3. Samples are 0 on x = 0 and 1 on x = 1.
    const AsdfBrickSampler linear({}, 1.f, 2u, 2u, 2u, {0.f, 1.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f});
    const f32 centre = linear.sample({0.5f, 0.25f, 0.75f});
    const f32 outsideCorner = linear.sample({4.f, 5.f, -1.f});
    const f32 outsideCornerExpect = 1.f + std::sqrt(26.f);
    const AsdfBrickSampler empty({}, 1.f, 0u, 4u, 4u, {});
    std::printf("A-SDF brick: linear centre %.4f (expected 0.500), outside corner %.4f (expected %.4f), empty %.3e\n",
                static_cast<double>(centre), static_cast<double>(outsideCorner), static_cast<double>(outsideCornerExpect),
                static_cast<double>(empty.sample({1.f, 2.f, 3.f})));

    near(centre, 0.5f, "trilinear sample of a linear brick");
    near(linear.sample({1.f, 0.f, 1.f}), 1.f, "lattice corners return the stored sample");
    near(linear.sample({1.f, 0.5f, 0.5f}), 1.f, "a face centre is the interpolated edge sample");
    near(linear.sample({3.f, 0.25f, 0.75f}), 3.f, "outside a face is the AABB distance plus the edge sample");
    near(outsideCorner, outsideCornerExpect, "outside a corner is the AABB distance plus the corner sample");
    expectTrue(linear.distance({4.f, 5.f, -1.f}) == outsideCorner, "distance() is the brick sample");
    expectTrue(linear.boundsMin().x == 0.f && linear.boundsMin().y == 0.f && linear.boundsMin().z == 0.f &&
                   linear.boundsMax().x == 1.f && linear.boundsMax().y == 1.f && linear.boundsMax().z == 1.f,
               "non-negative boundary samples keep the surface bounds on the brick AABB");
    expectTrue(linear.gradientStep() == 0.25f, "gradient step is a quarter voxel");
    const vec3 g = linear.gradient({0.5f, 0.5f, 0.5f});
    near(g.x, 0.5f, "unnormalised gradient matches the linear field");
    near(g.y, 0.f, "linear field has no y gradient");
    near(g.z, 0.f, "linear field has no z gradient");
    const vec3 far{-4.f, -5.f, 2.f};
    const vec3 gFar = linear.gradient(far);
    const vec3 away = far - vec3{0.f, 0.f, 1.f};
    expectTrue(std::isfinite(gFar.x) && std::isfinite(gFar.y) && std::isfinite(gFar.z) && gFar.dot(away) > 0.f,
               "gradient outside the brick is finite and points away from it");

    const SdfSampler& base = linear;
    expectTrue(base.distance({0.5f, 0.5f, 0.5f}) == linear.sample({0.5f, 0.5f, 0.5f}) && base.boundsMax().x == 1.f,
               "AsdfBrickSampler plugs into SdfSampler");
    const std::shared_ptr<const SdfSampler> held =
        std::make_shared<AsdfBrickSampler>(vec3{}, 1.f, 2u, 2u, 2u, std::vector<f32>{0.f, 1.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f});
    near(held->distance({0.5f, 0.2f, 0.7f}), 0.5f, "a pooled brick samples through SdfSampler::distance");

    // One non-zero corner: the cell centre is the mean of the eight weights, 8 * 0.125 = 1.
    const AsdfBrickSampler corner({}, 1.f, 2u, 2u, 2u, {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 8.f});
    near(corner.sample({0.5f, 0.5f, 0.5f}), 1.f, "trilinear weight of the far corner");
    near(corner.sample({1.f, 0.5f, 0.5f}), 2.f, "bilinear edge sample on the far face");
    near(corner.sample({2.f, 0.5f, 0.5f}), 3.f, "outside the far face adds the distance to that edge sample");

    // Values are the x-fastest index, so a swapped axis reads a different sample.
    std::vector<f32> row(12u);
    for (u32 i = 0; i < 12u; ++i) {
        row[i] = static_cast<f32>(i);
    }
    const AsdfBrickSampler layout({}, 1.f, 3u, 2u, 2u, std::move(row));
    near(layout.sample({1.f, 0.f, 0.f}), 1.f, "samples are row-major with x fastest");
    near(layout.sample({0.f, 1.f, 0.f}), 3.f, "y is the middle sample axis");
    near(layout.sample({0.f, 0.f, 1.f}), 6.f, "z is the slowest sample axis");
    near(layout.sample({2.f, 1.f, 1.f}), 11.f, "the last lattice sample is index nx*ny*nz - 1");

    std::vector<f32> extra{0.f, 1.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f, 1000.f};
    const AsdfBrickSampler extraBrick({}, 1.f, 2u, 2u, 2u, std::move(extra));
    near(extraBrick.sample({0.5f, 0.5f, 0.5f}), 0.5f, "samples past nx*ny*nz are ignored");

    // Negative corner sample: the solid extends past the brick, and the bounds follow it.
    std::vector<f32> neg(8u, 0.f);
    neg[0] = -2.f;
    const AsdfBrickSampler dipped({}, 1.f, 2u, 2u, 2u, std::move(neg));
    near(dipped.sample({0.f, 0.f, 0.f}), -2.f, "a negative corner sample is inside");
    near(dipped.sample({-1.f, 0.f, 0.f}), -1.f, "outside the brick can still be inside the solid");
    near(dipped.sample({-2.25f, 0.f, 0.f}), 0.25f, "past the expanded bounds the distance is positive");
    expectTrue(dipped.boundsMin().x == -2.f && dipped.boundsMin().y == -2.f && dipped.boundsMax().x == 3.f &&
                   dipped.boundsMax().z == 3.f,
               "surface bounds grow by a negative boundary sample");

    // Deep interior sample must not leak into the outside edge value.
    std::vector<f32> shell(27u, 0.f);
    shell[(1u * 3u + 1u) * 3u + 1u] = -10.f;
    const AsdfBrickSampler interior({}, 1.f, 3u, 3u, 3u, std::move(shell));
    near(interior.sample({1.f, 1.f, 1.f}), -10.f, "an interior sample is stored x-fastest");
    near(interior.sample({-1.f, 1.f, 1.f}), 1.f, "outside uses the edge sample, not the interior");
    expectTrue(interior.boundsMin().x == 0.f && interior.boundsMax().x == 2.f,
               "a negative interior sample does not expand the brick");

    // ny = nz = 1: trilinear must not read a missing neighbour, including on the last sample.
    const AsdfBrickSampler line({}, 1.f, 3u, 1u, 1u, {0.f, 10.f, 30.f});
    near(line.sample({0.5f, 0.f, 0.f}), 5.f, "linear interpolation along a single span");
    near(line.sample({1.5f, 0.f, 0.f}), 20.f, "linear interpolation on the last span");
    near(line.sample({2.f, 0.f, 0.f}), 30.f, "the last sample of a degenerate brick");
    near(line.sample({1.5f, 7.f, -3.f}), 20.f + std::sqrt(58.f), "a flat axis still has a brick AABB");
    expectTrue(line.boundsMax().x == 2.f && line.boundsMax().y == 0.f && line.boundsMin().z == 0.f,
               "a one-sample axis has zero brick extent");

    const AsdfBrickSampler point({2.f, 3.f, 4.f}, 0.5f, 1u, 1u, 1u, {-1.5f});
    near(point.sample({2.f, 3.f, 4.f}), -1.5f, "a single sample is the distance at its origin");
    near(point.sample({2.f, 3.f, 6.f}), 0.5f, "outside a point brick adds the distance to that sample");
    near(point.sample({2.f, 3.f, 5.5f}), 0.f, "the expanded bounds meet the zero crossing");
    expectTrue(point.boundsMin().x == 0.5f && point.boundsMin().y == 1.5f && point.boundsMin().z == 2.5f &&
                   point.boundsMax().x == 3.5f && point.boundsMax().y == 4.5f && point.boundsMax().z == 5.5f,
               "bounds enclose a negative point sample");
    expectTrue(point.gradientStep() == 0.125f, "gradient step scales with the voxel size");

    const AsdfBrickSampler shifted({10.f, 20.f, 30.f}, 2.f, 2u, 2u, 2u, {0.f, 1.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f});
    near(shifted.sample({11.f, 20.f, 31.f}), 0.5f, "origin and voxel size place the lattice");
    near(shifted.sample({12.f, 22.f, 32.f}), 1.f, "the far corner honours origin and voxel size");

    const AsdfBrickSampler shortGrid({}, 1.f, 2u, 2u, 2u, std::vector<f32>(7u, 1.f));
    const AsdfBrickSampler zeroVoxel({}, 0.f, 2u, 2u, 2u, std::vector<f32>(8u, 1.f));
    const AsdfBrickSampler negVoxel({}, -2.f, 2u, 2u, 2u, std::vector<f32>(8u, 1.f));
    const AsdfBrickSampler nanVoxel({}, std::numeric_limits<f32>::quiet_NaN(), 1u, 1u, 1u, {4.f});
    const f32 emptyDistance = empty.sample({1.f, 2.f, 3.f});
    expectTrue(std::isfinite(emptyDistance) && emptyDistance > 1e20f, "an empty grid is a large positive distance");
    expectTrue(empty.sample({-40.f, 8.f, 0.f}) == emptyDistance && shortGrid.sample(vec3{}) == emptyDistance &&
                   zeroVoxel.sample({3.f, 3.f, 3.f}) == emptyDistance && negVoxel.distance(vec3{}) == emptyDistance &&
                   nanVoxel.sample({1.f, 1.f, 1.f}) == emptyDistance,
               "zero axes, a short buffer, and a non-positive voxel size are empty");
    expectTrue(empty.boundsMin().x == 0.f && empty.boundsMax().z == 0.f && zeroVoxel.gradientStep() == 1e-3f,
               "an empty grid collapses its bounds and keeps a finite gradient step");
    const f32 nanQuery = linear.sample({std::numeric_limits<f32>::quiet_NaN(), 0.f, 0.f});
    expectTrue(std::isfinite(nanQuery) && nanQuery == emptyDistance, "a non-finite query does not produce NaN");
}

void testVoxelResOk() {
    expectTrue(voxel_res_ok(8), "resolution 8 is a valid voxel grid");
    expectTrue(voxel_res_ok(64), "resolution 64 is a valid voxel grid");
    expectTrue(voxel_res_ok(128), "resolution 128 is the voxel grid cap");
    expectTrue(!voxel_res_ok(7), "resolution 7 is below the voxel grid floor");
    expectTrue(!voxel_res_ok(24), "resolution 24 is not a power of two");
    expectTrue(!voxel_res_ok(256), "resolution 256 exceeds the voxel grid cap");
}

void testVoxelDamage() {
    VoxelDamage voxel{10.f, 10.f};
    const bool first = apply(voxel, 4.f);
    const f32 afterFirst = voxel.health;
    const bool second = apply(voxel, 4.f);
    const f32 afterSecond = voxel.health;
    const bool aliveAfterTwo = !broken(voxel);
    const bool third = apply(voxel, 4.f);
    const bool fourth = apply(voxel, 4.f);
    std::printf("voxel damage: health 10, hits of 4 -> %.1f then %.1f (%s), third breaks %s, further apply %s\n",
                static_cast<double>(afterFirst), static_cast<double>(afterSecond), aliveAfterTwo ? "alive" : "broken",
                third ? "yes" : "no", fourth ? "true" : "false");
    expectTrue(!first && afterFirst == 6.f, "the first hit of 4 on health 10 stays alive");
    expectTrue(!second && afterSecond == 2.f && aliveAfterTwo, "the second hit of 4 stays alive");
    expectTrue(third && voxel.health == 0.f && broken(voxel) && voxel.threshold == 10.f,
               "the third hit of 4 breaks the voxel");
    expectTrue(!fourth && voxel.health == 0.f && broken(voxel), "a further apply on a broken voxel returns false");

    VoxelDamage ignored{10.f, 10.f};
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    expectTrue(!apply(ignored, 0.f) && !apply(ignored, -4.f) && !apply(ignored, nan) &&
                   !apply(ignored, std::numeric_limits<f32>::infinity()) && ignored.health == 10.f && !broken(ignored),
               "non-positive and non-finite damage does not accumulate");
}

} // namespace

int main() {
    testVoxelResOk();
    fuse::core::initialize();
    testAsdfBrickSampler();
    testVoxelVolumeOnSvo();
    testSphereOnVoxelFloor();
    testBodiesOnSdf();
    testVoxelDamage();
    fuse::core::shutdown();
    return finish("fuse_e18_voxel_gates");
}
