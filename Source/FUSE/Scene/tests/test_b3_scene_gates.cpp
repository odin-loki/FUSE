// B3.9 scene gate rows (master plan), on SceneManager (B3.6):
//  - 1000 mesh + SDF entities: render data built in < 1 ms (optimised builds)
//  - active camera frustum culls out-of-view entities: draw counts == brute force, < total
//  - adding/removing entities mid-frame does not corrupt the BVH or draw list
//  - 10k entity transform update < 1 ms on all cores via each_parallel (optimised builds)
//  - full scene build (cull -> draw list -> SDF object buffer) < 2 ms for 1k entities
// Timing budgets are only enforced in optimised, uninstrumented runs
// (fuse::core::timingBudgetsEnforced(): off under sanitizers and valgrind); timings are always printed.
#include <fuse/core/sanitizer.hpp>
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/systems/culling_system.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/scene/scene_manager.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

using fuse::ecs::EntityID;

/// Wall-clock budgets only hold in optimised builds that are not instrumented.
bool enforceBudgets() {
#if defined(NDEBUG)
    static const bool enforced = [] {
        const bool on = fuse::core::timingBudgetsEnforced();
        if (!on) {
            std::printf("timing budgets not enforced (sanitizer build or instrumented run)\n");
        }
        return on;
    }();
    return enforced;
#else
    return false;
#endif
}

double millisSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void spawn(fuse::scene::SceneManager& scene, std::mt19937& rng, fuse::u32 count, float spread) {
    std::uniform_real_distribution<float> pos(-spread, spread);
    auto& reg = scene.registry();
    for (fuse::u32 i = 0; i < count; ++i) {
        const EntityID id = reg.create();
        fuse::ecs::Transform t{};
        t.position = {pos(rng), pos(rng), pos(rng), 1.f};
        t.dirty = true;
        reg.add(id, t);
        if ((i & 1u) == 0u) {
            fuse::ecs::Mesh mesh{};
            mesh.index_count = 36;
            mesh.aabb_min = {-0.5f, -0.5f, -0.5f, 1.f};
            mesh.aabb_max = {0.5f, 0.5f, 0.5f, 1.f};
            reg.add(id, mesh);
        } else {
            fuse::ecs::SDFObject sdf{};
            sdf.params = {0.75f, 0.f, 0.f, 0.f};
            reg.add(id, sdf);
        }
    }
}

EntityID addCamera(fuse::scene::SceneManager& scene) {
    const EntityID camera = scene.createCamera(70.f, true);
    fuse::ecs::Transform* t = scene.registry().get<fuse::ecs::Transform>(camera);
    t->position = {0.f, 0.f, -60.f, 1.f}; // looking down +Z at the origin
    t->dirty = true;
    return camera;
}

/// O(n) reference: exact per-object frustum tests over every entity.
void bruteForceCounts(fuse::scene::SceneManager& scene, std::size_t& meshes, std::size_t& sdfs) {
    const fuse::ecs::Camera& camera = *scene.registry().get<fuse::ecs::Camera>(scene.activeCamera());
    meshes = 0;
    sdfs = 0;
    scene.registry().each<fuse::ecs::Mesh, fuse::ecs::Transform>(
        [&](EntityID, fuse::ecs::Mesh& mesh, fuse::ecs::Transform& transform) {
            const fuse::spatial::AABB b = fuse::ecs::CullingSystem::world_bounds(transform, mesh);
            meshes += fuse::ecs::CullingSystem::test_aabb_frustum(camera.frustum, b.min, b.max) ? 1u : 0u;
        });
    scene.registry().each<fuse::ecs::SDFObject, fuse::ecs::Transform>(
        [&](EntityID, fuse::ecs::SDFObject& sdf, fuse::ecs::Transform& transform) {
            const fuse::ecs::vec3 c{transform.local_to_world.data[12], transform.local_to_world.data[13],
                                    transform.local_to_world.data[14], 1.f};
            sdfs += fuse::ecs::CullingSystem::test_sphere_frustum(camera.frustum, c, sdf.params.x) ? 1u : 0u;
        });
}

void testBuildAndCull() {
    fuse::scene::SceneManager scene;
    fuse::scene::SceneManagerDesc desc{};
    desc.hasVoxels = false;
    scene.init(desc);
    std::mt19937 rng(1000u);

    // 1000 mesh + SDF entities; the first frame builds the BVH and the render data.
    spawn(scene, rng, 1000u, 80.f);
    addCamera(scene);
    const auto firstStart = std::chrono::steady_clock::now();
    scene.update(1.f / 60.f);
    fuse::ecs::CullResult cull{};
    fuse::ecs::SceneData data = scene.buildFrame(&cull);
    const double firstBuildMs = millisSince(firstStart);

    std::size_t expectedMeshes = 0;
    std::size_t expectedSdfs = 0;
    bruteForceCounts(scene, expectedMeshes, expectedSdfs);
    std::printf("scene: %zu draw items, %zu sdf objects (brute force %zu / %zu), culled %u, first build %.3f ms\n",
                data.draw_items.size(), data.sdf_objects.size(), expectedMeshes, expectedSdfs, cull.culled_count,
                firstBuildMs);
    expectTrue(data.draw_items.size() == expectedMeshes, "draw items == brute-force visible meshes");
    expectTrue(data.sdf_objects.size() == expectedSdfs, "SDF objects == brute-force visible SDFs");
    expectTrue(data.draw_items.size() + data.sdf_objects.size() < 1000u, "frustum culls out-of-view entities");
    expectTrue(!data.draw_items.empty() && !data.sdf_objects.empty(), "camera sees part of the scene");

    // Steady state: full scene build (cull -> draw list -> SDF buffer) per frame.
    std::vector<double> samples;
    for (int i = 0; i < 51; ++i) {
        const auto start = std::chrono::steady_clock::now();
        scene.update(1.f / 60.f);
        data = scene.buildFrame();
        samples.push_back(millisSince(start));
    }
    std::sort(samples.begin(), samples.end());
    std::printf("scene: steady update+build median %.3f ms, BVH rebuilds %u\n", samples[25],
                scene.spatialBvhRebuildCount());
    expectTrue(scene.spatialBvhRebuildCount() == 1u, "steady frames refit instead of rebuilding");
    if (enforceBudgets()) {
        // The first build is a single cold sample; take the best of it and two more fresh scenes so one
        // preempted run on a loaded machine does not fail the budget (typical cost is about half of it).
        double bestFirstBuildMs = firstBuildMs;
        for (int rep = 0; rep < 2; ++rep) {
            fuse::scene::SceneManager fresh;
            fresh.init(desc);
            std::mt19937 freshRng(1000u);
            spawn(fresh, freshRng, 1000u, 80.f);
            addCamera(fresh);
            const auto freshStart = std::chrono::steady_clock::now();
            fresh.update(1.f / 60.f);
            (void)fresh.buildFrame();
            bestFirstBuildMs = std::min(bestFirstBuildMs, millisSince(freshStart));
        }
        expectTrue(bestFirstBuildMs < 1.0, "1000 mesh+SDF entities build render data (incl. BVH build) < 1 ms");
        expectTrue(samples[25] < 2.0, "full scene build for 1k entities < 2 ms");
    }

    // Mid-frame edits: destroy a third, spawn new ones, move some — BVH and draw list stay exact.
    std::vector<EntityID> all;
    scene.registry().each<fuse::ecs::Transform>([&](EntityID id, fuse::ecs::Transform&) { all.push_back(id); });
    for (std::size_t i = 0; i < all.size(); i += 3) {
        if (all[i] != scene.activeCamera()) {
            scene.registry().destroy_entity(all[i]);
        }
    }
    spawn(scene, rng, 200u, 40.f);
    for (std::size_t i = 1; i < all.size(); i += 7) {
        if (fuse::ecs::Transform* t = scene.registry().get<fuse::ecs::Transform>(all[i])) {
            t->position.x = -t->position.x;
            t->dirty = true;
        }
    }
    scene.update(1.f / 60.f);
    data = scene.buildFrame(&cull);
    bruteForceCounts(scene, expectedMeshes, expectedSdfs);
    expectTrue(data.draw_items.size() == expectedMeshes && data.sdf_objects.size() == expectedSdfs,
               "after mid-frame add/remove/move, draw list == brute force");
    bool allAlive = true;
    for (const auto& item : data.draw_items) {
        allAlive = allAlive && scene.registry().alive(item.entity);
    }
    expectTrue(allAlive, "draw list never references destroyed entities");
    expectTrue(scene.spatialBvhRebuildCount() == 2u, "entity set change triggers exactly one rebuild");
    scene.destroy();
}

void testTransformUpdate10k() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(4u);

    fuse::ecs::Registry reg;
    reg.init(16384);
    for (fuse::u32 i = 0; i < 10'000u; ++i) {
        const EntityID id = reg.create();
        fuse::ecs::Transform t{};
        t.position = {static_cast<float>(i), 0.f, 0.f, 1.f};
        reg.add(id, t);
    }
    std::vector<double> samples;
    for (int i = 0; i < 21; ++i) {
        reg.each<fuse::ecs::Transform>([](EntityID, fuse::ecs::Transform& t) { t.dirty = true; });
        const auto start = std::chrono::steady_clock::now();
        fuse::ecs::TransformSystem::update(reg);
        samples.push_back(millisSince(start));
    }
    std::sort(samples.begin(), samples.end());
    bool allClean = true;
    reg.each<fuse::ecs::Transform>([&](EntityID, fuse::ecs::Transform& t) { allClean = allClean && !t.dirty; });
    std::printf("transform update 10k (4 workers): median %.3f ms\n", samples[10]);
    expectTrue(allClean, "every dirty transform recomputed");
    if (enforceBudgets()) {
        expectTrue(samples[10] < 1.0, "10k entity transform update < 1 ms via each_parallel");
    }
    scheduler.shutdown();
}

} // namespace

int main() {
    testBuildAndCull();
    testTransformUpdate10k();

    if (g_failures == 0) {
        std::printf("fuse_b3_scene_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b3_scene_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
