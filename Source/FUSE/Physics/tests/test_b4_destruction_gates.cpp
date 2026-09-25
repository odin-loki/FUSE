// B4.11 destruction gate rows (master plan):
//  - sphere carve removes exactly the voxels within the radius (re-query the carved region)
//  - dual contouring extracts a watertight mesh from the carved surface
//  - debris entities spawn with mass proportional to their voxel count
//  - debris rigid bodies collide correctly with the scene after spawning
//  - 10 simultaneous impacts each spawning 5 debris pieces: no frame spike > 10 ms
//  - 5 simultaneous destruction events with 10 debris each: < 16 ms total
#include <fuse/core/sanitizer.hpp>
#include <fuse/core/init.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/physics/physics_manager.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

using fuse::f32;
using fuse::s32;
using fuse::u32;
using fuse::usize;
using fuse::ecs::EntityID;
using fuse::ecs::Registry;
using namespace fuse::physics;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr f32 kDt = 1.f / 60.f;

/// Every directed edge must be matched by its reverse: a closed (watertight) oriented surface.
bool watertight(const std::vector<vec3>& verts, const std::vector<u32>& indices, u32& openEdges) {
    std::map<std::pair<u32, u32>, int> edges;
    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        for (int e = 0; e < 3; ++e) {
            const u32 a = indices[t + e];
            const u32 b = indices[t + (e + 1) % 3];
            ++edges[{a, b}];
        }
    }
    openEdges = 0;
    for (const auto& [edge, count] : edges) {
        const auto reverse = edges.find({edge.second, edge.first});
        openEdges += (reverse == edges.end() || reverse->second != count) ? 1u : 0u;
    }
    return openEdges == 0 && !indices.empty() && !verts.empty();
}

void testCarveExact() {
    VoxelVolume volume;
    volume.init({-4.f, 0.f, -4.f}, 0.25f, {32, 32, 32});
    volume.fill({0, 0, 0}, {31, 31, 31}, 1u);
    const vec3 center{0.1f, 4.2f, -0.3f};
    const f32 radius = 1.3f;
    const u32 removed = volume.carve(center, radius);
    u32 expectedRemoved = 0;
    u32 wrong = 0;
    for (s32 z = 0; z < 32; ++z) {
        for (s32 y = 0; y < 32; ++y) {
            for (s32 x = 0; x < 32; ++x) {
                const bool inside = (volume.voxelCenter({x, y, z}) - center).length() < radius;
                expectedRemoved += inside ? 1u : 0u;
                wrong += (volume.get({x, y, z}) == 0u) != inside ? 1u : 0u;
            }
        }
    }
    std::printf("carve: removed %u voxels (expected %u), %u voxels in the wrong state\n", removed, expectedRemoved,
                wrong);
    expectTrue(removed == expectedRemoved && wrong == 0, "carve removes exactly the voxels inside the sphere");
    expectTrue(volume.solidCount() == 32u * 32u * 32u - removed, "solid count follows the carve");
}

void testDualContouringWatertight() {
    VoxelVolume volume;
    volume.init({0.f, 0.f, 0.f}, 0.1f, {40, 24, 40});
    volume.fill({2, 0, 2}, {37, 20, 37}, 1u);
    volume.carve({2.f, 1.2f, 2.f}, 0.9f);  // crater through the top face
    volume.carve({1.2f, 1.f, 3.9f}, 0.5f); // bite out of a side
    volume.carve({2.f, 0.9f, 2.f}, 0.35f); // (merges with the crater)
    u32 open = 0;
    std::vector<vec3> verts;
    std::vector<u32> indices;
    volume.extractSurface(verts, indices);
    const bool closed = watertight(verts, indices, open);

    // Every vertex sits within one voxel of the solid/empty boundary it approximates.
    u32 stray = 0;
    for (const vec3& v : verts) {
        bool nearSolid = false;
        bool nearEmpty = false;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const vec3 probe = v + vec3{static_cast<f32>(dx), static_cast<f32>(dy), static_cast<f32>(dz)} * 0.06f;
                    const bool solid = volume.get(volume.voxelAt(probe)) != 0u;
                    nearSolid = nearSolid || solid;
                    nearEmpty = nearEmpty || !solid;
                }
            }
        }
        stray += nearSolid && nearEmpty ? 0u : 1u;
    }
    std::printf("dual contouring: %zu vertices, %zu triangles, %u open edges, %u off-surface vertices\n",
                verts.size(), indices.size() / 3, open, stray);
    expectTrue(closed, "carved surface mesh is watertight");
    expectTrue(stray == 0, "vertices lie on the solid/empty boundary");
}

/// A destructible: an anchored pillar with `pieces` 3x3x3 blocks hung off its top by thin rods.
/// Carving the pillar top frees every block.
VoxelVolume makeHub(vec3 origin, u32 pieces) {
    VoxelVolume volume;
    volume.init(origin, 0.1f, {24, 12, 24});
    volume.fill({11, 0, 11}, {12, 8, 12}, 1u);
    for (u32 i = 0; i < pieces; ++i) {
        const f32 angle = 6.2831853f * static_cast<f32>(i) / static_cast<f32>(pieces);
        const s32 cx = 11 + static_cast<s32>(std::lround(8.f * std::cos(angle)));
        const s32 cz = 11 + static_cast<s32>(std::lround(8.f * std::sin(angle)));
        volume.fill({cx - 1, 6, cz - 1}, {cx + 1, 8, cz + 1}, 1u);
        // One-voxel, face-connected rod from the block back to the pillar at y = 7 (diagonal
        // steps get the corner voxel so the rod has no diagonal-only joints).
        const s32 steps = 32;
        s32 pz = 11;
        for (s32 s = 0; s <= steps; ++s) {
            const f32 t = static_cast<f32>(s) / static_cast<f32>(steps);
            const s32 x = static_cast<s32>(std::lround(11.f + static_cast<f32>(cx - 11) * t));
            const s32 z = static_cast<s32>(std::lround(11.f + static_cast<f32>(cz - 11) * t));
            volume.set({x, 7, pz}, 1u);
            volume.set({x, 7, z}, 1u);
            pz = z;
        }
    }
    return volume;
}

EntityID spawnGround(Registry& reg) {
    const EntityID id = reg.create();
    reg.add(id, fuse::ecs::Transform{});
    fuse::ecs::RigidBody body{};
    body.is_static = true;
    reg.add(id, body);
    fuse::ecs::Collider plane{};
    plane.shape = fuse::ecs::Collider::Plane;
    plane.params = {0.f, 1.f, 0.f, 0.f};
    reg.add(id, plane);
    return id;
}

struct HubScene {
    Registry reg;
    PhysicsManager manager;
    std::vector<EntityID> hubs;
    std::vector<vec3> impacts;
};

void buildHubScene(HubScene& scene, u32 hubCount, u32 piecesPerHub) {
    scene.reg.init(4096);
    spawnGround(scene.reg);
    scene.manager.init({});
    VoxelMaterial material{};
    material.density = 1000.f;
    for (u32 h = 0; h < hubCount; ++h) {
        const vec3 origin{static_cast<f32>(h % 5) * 4.f, 0.f, static_cast<f32>(h / 5) * 4.f};
        const EntityID hub = scene.reg.create();
        scene.manager.addDestructible(hub, makeHub(origin, piecesPerHub), material);
        scene.hubs.push_back(hub);
        scene.impacts.push_back(origin + vec3{1.2f, 0.75f, 1.2f}); // pillar top
    }
}

void queueImpacts(HubScene& scene) {
    for (usize i = 0; i < scene.hubs.size(); ++i) {
        DestructionEvent event{};
        event.target = scene.hubs[i];
        event.impactPoint = scene.impacts[i];
        event.impactNormal = {0.f, 1.f, 0.f};
        event.impulse = 0.05f;
        event.carveRadius = 0.42f;
        scene.manager.pushDestructionEvent(event);
    }
}

void testDebrisMassAndCollision() {
    HubScene scene;
    buildHubScene(scene, 1, 5);
    PhysicsStreamManager streams{};
    const u32 before = scene.manager.destructible(scene.hubs[0])->volume.solidCount();
    queueImpacts(scene);
    scene.manager.step(scene.reg, kDt, streams);
    const std::vector<DebrisSpawn> debris = scene.manager.lastDebris();
    const f32 voxelMass = 0.1f * 0.1f * 0.1f * 1000.f;
    bool massExact = true;
    bool meshesClosed = true;
    u32 detached = 0;
    for (const DebrisSpawn& d : debris) {
        massExact = massExact && std::fabs(d.mass - static_cast<f32>(d.voxelCount) * voxelMass) < 1e-5f &&
                    std::fabs(scene.reg.get<fuse::ecs::RigidBody>(d.entity)->mass - d.mass) < 1e-6f;
        u32 open = 0;
        meshesClosed = meshesClosed && watertight(d.mesh.verts, d.mesh.indices, open);
        detached += d.voxelCount;
    }
    const u32 after = scene.manager.destructible(scene.hubs[0])->volume.solidCount();
    std::printf("debris: %zu pieces, voxel counts", debris.size());
    for (const DebrisSpawn& d : debris) {
        std::printf(" %u", d.voxelCount);
    }
    std::printf(", volume %u -> %u voxels\n", before, after);
    expectTrue(debris.size() == 5u, "carving the pillar top frees the five blocks");
    expectTrue(massExact, "debris mass = voxel count x voxel volume x density (component matches)");
    expectTrue(meshesClosed, "every debris mesh is watertight");
    expectTrue(after + detached < before, "carved and detached voxels leave the volume");

    // Debris falls and settles on the ground plane without sinking or tunnelling.
    for (int frame = 0; frame < 240; ++frame) {
        scene.manager.step(scene.reg, kDt, streams);
    }
    f32 worstRest = 0.f;
    for (const DebrisSpawn& d : debris) {
        const fuse::ecs::Collider* c = scene.reg.get<fuse::ecs::Collider>(d.entity);
        const f32 y = scene.reg.get<fuse::ecs::Transform>(d.entity)->position.y;
        worstRest = std::max(worstRest, std::fabs(y - c->params.y));
    }
    std::printf("debris after 4 s: worst |y - half height| %.4f m\n", worstRest);
    expectTrue(worstRest < 0.02f, "debris bodies land and rest on the ground");
}

void testSimultaneousImpactsBudget(u32 hubCount, u32 piecesPerHub, f32 budgetMs, const char* label) {
    HubScene scene;
    buildHubScene(scene, hubCount, piecesPerHub);
    PhysicsStreamManager streams{};
    scene.manager.step(scene.reg, kDt, streams); // warm up
    queueImpacts(scene);
    auto start = std::chrono::steady_clock::now();
    scene.manager.step(scene.reg, kDt, streams);
    const double eventFrameMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const usize spawned = scene.manager.lastDebris().size();
    double worstAfter = 0.0;
    for (int frame = 0; frame < 90; ++frame) { // debris joins the simulation and falls
        start = std::chrono::steady_clock::now();
        scene.manager.step(scene.reg, kDt, streams);
        worstAfter = std::max(worstAfter,
                              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::printf("%s: %zu debris, destruction frame %.3f ms, worst following frame %.3f ms\n", label, spawned,
                eventFrameMs, worstAfter);
    expectTrue(spawned == static_cast<usize>(hubCount) * piecesPerHub, "every impact frees its pieces");
#if defined(NDEBUG)
    if (fuse::core::timingBudgetsEnforcedNoted()) {
        expectTrue(eventFrameMs < budgetMs && worstAfter < budgetMs, "destruction stays within its frame budget");
    }
#else
    (void)budgetMs;
#endif
}

} // namespace

int main() {
    fuse::core::initialize();
    testCarveExact();
    testDualContouringWatertight();
    testDebrisMassAndCollision();
    testSimultaneousImpactsBudget(10u, 5u, 10.f, "10 impacts x 5 debris (no spike > 10 ms)");
    testSimultaneousImpactsBudget(5u, 10u, 16.f, "5 events x 10 debris (< 16 ms)");
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b4_destruction_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b4_destruction_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
