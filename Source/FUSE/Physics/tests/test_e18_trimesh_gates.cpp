// E18 part 2 (GAP-PHYS-HULL-MESH, static triangle meshes):
//  - BVH ray cast == brute force over every triangle on random rays (same t bit for bit, same triangle), and
//    PhysicsManager::rayCast against a TriMesh body (rotated, translated) == the brute force
//  - a box rests stably on a triangle-mesh floor: drift bound after 600 steps; a sphere rolls across the
//    floor's triangle seams without being kicked up (internal-edge normal fix)
//  - a convex hull rests on the mesh (hull-vs-triangle SAT)
//  - .fusecol round trip: serialise -> parse -> serialise is byte-identical; corrupt files are refused; an
//    ECS Collider naming the asset id collides with the cooked mesh
//  - determinism: two runs bit-identical
#include "e18_test_common.hpp"

#include <fuse/core/init.hpp>
#include <fuse/physics/assets/collision_asset.hpp>
#include <fuse/physics/shapes/shape_pool.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

using namespace e18;
namespace np = fuse::physics::narrowphase;

namespace {

f32 hills(f32 x, f32 z) {
    return 0.35f * std::sin(0.9f * x) * std::cos(0.7f * z) + 0.1f * std::sin(2.3f * x + 1.f);
}

TriMesh makeTerrain(u32 cells, f32 half, f32 y, bool bumpy) {
    std::vector<vec3> vertices;
    std::vector<u32> indices;
    gridMesh(cells, half, y, vertices, indices, bumpy ? hills : nullptr);
    TriMesh mesh;
    mesh.build(vertices, indices);
    return mesh;
}

void testRayCastMatchesBruteForce() {
    const TriMesh mesh = makeTerrain(64u, 20.f, 0.f, true);
    Rng rng(3u);
    u32 hits = 0;
    u32 mismatches = 0;
    for (u32 i = 0; i < 20000u; ++i) {
        const vec3 origin{rng.range(-25.f, 25.f), rng.range(-2.f, 6.f), rng.range(-25.f, 25.f)};
        vec3 dir = rng.onSphere();
        if (i % 3u == 0u) {
            dir = {dir.x * 0.2f, -1.f, dir.z * 0.2f}; // mostly downward rays hit the terrain
            dir = dir.normalized();
        }
        const f32 maxT = rng.range(1.f, 60.f);
        TriMeshRayHit bvh{};
        TriMeshRayHit brute{};
        const bool a = mesh.rayCast(origin, dir, maxT, bvh);
        const bool b = mesh.rayCastBruteForce(origin, dir, maxT, brute);
        if (a != b || (a && (std::memcmp(&bvh.t, &brute.t, sizeof(f32)) != 0 || bvh.triangle != brute.triangle))) {
            ++mismatches;
        }
        hits += a ? 1u : 0u;
    }
    std::printf("trimesh BVH: %u triangles, %zu nodes, depth %u; 20000 rays, %u hits, %u mismatches vs brute force\n",
                mesh.triangleCount(), mesh.nodes().size(), mesh.maxDepth(), hits, mismatches);
    expectTrue(hits > 5000u && mismatches == 0u, "BVH ray cast == brute force (t bit-identical, same triangle)");

    // Through PhysicsManager::rayCast (a rotated, translated static mesh body).
    Registry reg;
    reg.init(64);
    PhysicsManager manager;
    manager.init({});
    PhysicsStreamManager streams{};
    const u32 ref = ShapePool::global().addMesh(makeTerrain(32u, 10.f, 0.f, true));
    const vec3 bodyPos{3.f, -1.f, 2.f};
    const quat bodyRot = quatFromAxisAngle({0.3f, 1.f, 0.2f}, 0.6f);
    const EntityID terrain = spawnPooled(reg, bodyPos, fuse::ecs::Collider::TriMesh, ref, true, 1.f, bodyRot);
    manager.step(reg, 1.f / 60.f, streams);
    const TriMesh* local = ShapePool::global().mesh(ref);
    u32 managerHits = 0;
    u32 managerMismatch = 0;
    for (u32 i = 0; i < 4000u; ++i) {
        const vec3 origin = bodyPos + vec3{rng.range(-12.f, 12.f), rng.range(3.f, 8.f), rng.range(-12.f, 12.f)};
        const vec3 dir = (vec3{rng.range(-0.3f, 0.3f), -1.f, rng.range(-0.3f, 0.3f)}).normalized();
        EntityID hit{};
        vec3 normal{};
        f32 t = 0.f;
        const bool a = manager.rayCast(origin, dir, 50.f, hit, normal, t);
        TriMeshRayHit brute{};
        const bool b = local->rayCastBruteForce(inverseRotate(bodyRot, origin - bodyPos), inverseRotate(bodyRot, dir),
                                                50.f, brute);
        if (a != b || (a && (std::fabs(t - brute.t) > 1e-5f || hit != terrain ||
                             normal.dot(rotate(bodyRot, brute.normal)) < 0.9999f))) {
            ++managerMismatch;
        }
        managerHits += a ? 1u : 0u;
    }
    std::printf("PhysicsManager::rayCast on a TriMesh body: %u hits, %u mismatches vs brute force\n", managerHits,
                managerMismatch);
    expectTrue(managerHits > 2000u && managerMismatch == 0u, "PhysicsManager::rayCast supports triangle meshes");
}

struct FloorRun {
    vec3 boxStart{};
    vec3 box{};
    quat boxRotation{};
    f32 boxSpeed = 0.f;
    f32 sphereMaxY = 0.f;
    f32 sphereMinY = 0.f;
    f32 sphereTravel = 0.f;
    vec3 hull{};
    f32 hullSpeed = 0.f;
};

FloorRun runFloor(u32 meshRef, u32 hullRef, bool viaAsset, u64 assetId) {
    Registry reg;
    reg.init(256);
    PhysicsManager manager;
    manager.init({});
    PhysicsStreamManager streams{};
    EntityID floor{};
    if (meshRef == kNoShapeRef && !viaAsset) {
        floor = spawnGroundPlane(reg); // analytic reference floor
    } else if (viaAsset) {
        floor = spawnBody(reg, {}, fuse::ecs::Collider::TriMesh, {}, true);
        reg.get<fuse::ecs::Collider>(floor)->shape_asset = assetId;
        reg.get<fuse::ecs::Collider>(floor)->shape_piece = 0u;
    } else {
        floor = spawnPooled(reg, {}, fuse::ecs::Collider::TriMesh, meshRef, true);
    }
    (void)floor;
    // A 1 m box dropped onto the mesh floor, straddling triangle seams.
    const EntityID box = spawnBody(reg, {0.13f, 0.7f, 0.21f}, fuse::ecs::Collider::Box, {0.5f, 0.5f, 0.5f, 0.f}, false);
    // A sphere rolling across the seams.
    const EntityID sphere = spawnBody(reg, {-6.f, 0.3f, 3.f}, fuse::ecs::Collider::Sphere, {0.3f, 0.f, 0.f, 0.f}, false);
    reg.get<fuse::ecs::RigidBody>(sphere)->velocity = {3.f, 0.f, 0.37f, 0.f};
    // A convex hull (a pooled box hull) dropped next to the box.
    const EntityID hull = spawnPooled(reg, {3.f, 1.f, -2.f}, fuse::ecs::Collider::ConvexHull, hullRef, false);

    FloorRun run{};
    run.sphereMinY = 1e30f;
    run.sphereMaxY = -1e30f;
    for (u32 frame = 0; frame < 600u; ++frame) {
        manager.step(reg, 1.f / 60.f, streams);
        if (frame == 119u) {
            run.boxStart = positionOf(reg, box);
        }
        if (frame >= 30u && frame < 150u) {
            const f32 y = positionOf(reg, sphere).y;
            run.sphereMinY = std::min(run.sphereMinY, y);
            run.sphereMaxY = std::max(run.sphereMaxY, y);
        }
    }
    run.box = positionOf(reg, box);
    const fuse::ecs::Transform* t = reg.get<fuse::ecs::Transform>(box);
    run.boxRotation = {t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w};
    run.boxSpeed = velocityOf(reg, box).length();
    run.sphereTravel = positionOf(reg, sphere).x + 6.f;
    run.hull = positionOf(reg, hull);
    run.hullSpeed = velocityOf(reg, hull).length();
    return run;
}

void testBodiesRestOnMeshFloor() {
    // 40 x 40 m floor of 16 x 16 quads (512 triangles, many shared edges).
    const u32 meshRef = ShapePool::global().addMesh(makeTerrain(16u, 20.f, 0.f, false));
    ConvexHull hullBox;
    makeBoxHull({0.4f, 0.3f, 0.4f}, hullBox);
    const u32 hullRef = ShapePool::global().addHull(std::move(hullBox));

    const FloorRun first = runFloor(meshRef, hullRef, false, 0u);
    const f32 drift = (first.box - first.boxStart).length();
    std::printf("box on a trimesh floor: y %.4f (expected 0.500), drift frames 120..600 %.6f m, speed %.5f m/s\n",
                static_cast<double>(first.box.y), static_cast<double>(drift), static_cast<double>(first.boxSpeed));
    expectTrue(std::fabs(first.box.y - 0.5f) < 0.01f, "the box rests on the triangle-mesh floor");
    expectTrue(drift < 0.005f && first.boxSpeed < 0.02f, "the resting box does not drift (600 steps)");
    const FloorRun plane = runFloor(kNoShapeRef, hullRef, false, 0u);
    std::printf("sphere rolling over triangle seams: y in [%.4f, %.4f] (radius 0.3), travelled %.4f m (%.4f m on an "
                "analytic plane)\n",
                static_cast<double>(first.sphereMinY), static_cast<double>(first.sphereMaxY),
                static_cast<double>(first.sphereTravel), static_cast<double>(plane.sphereTravel));
    expectTrue(first.sphereMaxY < 0.305f && first.sphereMinY > 0.29f &&
                   std::fabs(first.sphereTravel - plane.sphereTravel) < 0.02f * plane.sphereTravel + 0.01f,
               "a sphere rolls across triangle seams like on a plane, never kicked up (internal-edge fix)");
    std::printf("box on the analytic plane (reference): y %.4f\n", static_cast<double>(plane.box.y));
    std::printf("hull on the trimesh: y %.4f (expected 0.300), speed %.5f\n", static_cast<double>(first.hull.y),
                static_cast<double>(first.hullSpeed));
    expectTrue(std::fabs(first.hull.y - 0.3f) < 0.01f && first.hullSpeed < 0.02f, "a convex hull rests on the mesh");

    const FloorRun second = runFloor(meshRef, hullRef, false, 0u);
    const bool identical = std::memcmp(&first.box, &second.box, sizeof(vec3)) == 0 &&
                           std::memcmp(&first.boxRotation, &second.boxRotation, sizeof(quat)) == 0 &&
                           std::memcmp(&first.hull, &second.hull, sizeof(vec3)) == 0 &&
                           first.sphereTravel == second.sphereTravel;
    expectTrue(identical, "two trimesh-floor runs are bit-identical");
}

void testFusecolRoundTrip() {
    CollisionAsset asset;
    {
        Rng rng(11u);
        std::vector<vec3> points;
        for (u32 i = 0; i < 64u; ++i) {
            points.push_back(rng.inBox(0.5f));
        }
        ConvexHull hull;
        buildConvexHull(points, hull);
        asset.hulls.push_back(std::move(hull));
        ConvexHull box;
        makeBoxHull({0.2f, 0.3f, 0.4f}, box);
        asset.hulls.push_back(std::move(box));
        asset.meshes.push_back(makeTerrain(8u, 5.f, 0.f, true));
    }
    const std::vector<u8> bytes = serializeCollisionAsset(asset);
    CollisionAsset parsed;
    std::string error;
    const bool ok = deserializeCollisionAsset(bytes.data(), bytes.size(), parsed, &error);
    const std::vector<u8> again = ok ? serializeCollisionAsset(parsed) : std::vector<u8>{};
    std::printf(".fusecol: %zu bytes, parse %s%s, re-serialise %s\n", bytes.size(), ok ? "ok" : "FAILED ",
                error.c_str(), again == bytes ? "byte-identical" : "DIFFERENT");
    expectTrue(ok && again == bytes, ".fusecol round trip is byte-identical");
    expectTrue(ok && parsed.hulls.size() == 2u && parsed.meshes.size() == 1u &&
                   parsed.hulls[0].faces.size() == asset.hulls[0].faces.size() &&
                   parsed.meshes[0].triangleCount() == asset.meshes[0].triangleCount(),
               ".fusecol keeps every hull and mesh");

    // Ray casts on the parsed mesh equal the original (same BVH).
    Rng rng(12u);
    u32 same = 0;
    for (u32 i = 0; i < 500u; ++i) {
        const vec3 origin{rng.range(-5.f, 5.f), 3.f, rng.range(-5.f, 5.f)};
        const vec3 dir = (vec3{rng.range(-0.5f, 0.5f), -1.f, rng.range(-0.5f, 0.5f)}).normalized();
        TriMeshRayHit h1{};
        TriMeshRayHit h2{};
        const bool a = asset.meshes[0].rayCast(origin, dir, 20.f, h1);
        const bool b = ok && parsed.meshes[0].rayCast(origin, dir, 20.f, h2);
        same += (a == b && (!a || (h1.t == h2.t && h1.triangle == h2.triangle))) ? 1u : 0u;
    }
    expectTrue(same == 500u, "the loaded mesh answers ray casts exactly like the cooked one");

    // Corruption and truncation are refused.
    std::vector<u8> corrupt = bytes;
    corrupt[corrupt.size() / 2u] ^= 0x40u;
    CollisionAsset rejected;
    expectTrue(!deserializeCollisionAsset(corrupt.data(), corrupt.size(), rejected), "a corrupt .fusecol is refused");
    expectTrue(!deserializeCollisionAsset(bytes.data(), bytes.size() - 8u, rejected), "a truncated .fusecol is refused");

    // Asset binding: an ECS Collider names (asset id, piece) and gets the cooked mesh.
    const u64 assetId = 0xC011151011ull;
    CollisionAsset floorAsset;
    floorAsset.meshes.push_back(makeTerrain(16u, 20.f, 0.f, false));
    ConvexHull hullBox;
    makeBoxHull({0.4f, 0.3f, 0.4f}, hullBox);
    floorAsset.hulls.push_back(std::move(hullBox));
    const std::string path = "e18_floor.fusecol";
    std::string writeError;
    const bool written = writeCollisionAssetFile(path, floorAsset, &writeError);
    CollisionAssetRefs refs{};
    const bool loaded = written && loadCollisionAssetFile(path, assetId, &refs, &writeError);
    expectTrue(loaded && refs.meshRefs.size() == 1u && refs.hullRefs.size() == 1u, ".fusecol loads into the shape pool");
    expectTrue(ShapePool::global().findAsset(assetId, 0u, CollisionShapeType::TriMesh) == refs.meshRefs[0],
               "the asset id resolves to the cooked mesh");
    if (loaded) {
        const FloorRun run = runFloor(kNoShapeRef, refs.hullRefs[0], true, assetId);
        expectTrue(std::fabs(run.box.y - 0.5f) < 0.01f, "a Collider naming the .fusecol asset collides with its mesh");
        unregisterCollisionAsset(refs);
        expectTrue(ShapePool::global().findAsset(assetId, 0u, CollisionShapeType::TriMesh) == kNoShapeRef,
                   "unregistering releases the asset binding");
    }
    std::remove(path.c_str());
}

} // namespace

int main() {
    fuse::core::initialize();
    testRayCastMatchesBruteForce();
    testBodiesRestOnMeshFloor();
    testFusecolRoundTrip();
    fuse::core::shutdown();
    return finish("fuse_e18_trimesh_gates");
}
