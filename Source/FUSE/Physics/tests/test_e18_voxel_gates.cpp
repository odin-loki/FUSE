// E18 part 3b (UNI-B4-VOX-1): voxel and SDF collision shapes on the one shared SVO type.
//  - physics VoxelVolume is stored in the Scene SVO: every get / solid count equals a dense reference over
//    random set / fill / carve, SVO::readBox equals per-voxel reads, SVO ray casts see the same voxels
//  - a destructible volume is a static Voxel body: a sphere rests on the voxel floor, then falls through
//    a carved hole (the shape refreshes on carve and the resting body is woken)
//  - a box rests on an SDF shape (analytic rounded box through the SdfSampler interface) and a sphere on the
//    Scene SVO's distance field (SvoSdfSampler)
//  - PhysicsManager::rayCast / shapeCast hit voxel and SDF bodies; two runs are bit-identical
#include "e18_test_common.hpp"

#include <fuse/core/init.hpp>
#include <fuse/physics/queries/shape_queries.hpp>
#include <fuse/physics/shapes/shape_pool.hpp>
#include <fuse/physics/spatial/svo.hpp>
#include <fuse/scene/svo.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

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

} // namespace

int main() {
    fuse::core::initialize();
    testVoxelVolumeOnSvo();
    testSphereOnVoxelFloor();
    testBodiesOnSdf();
    fuse::core::shutdown();
    return finish("fuse_e18_voxel_gates");
}
