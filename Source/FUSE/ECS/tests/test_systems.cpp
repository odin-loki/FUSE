#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/system_scheduler.hpp>
#include <fuse/ecs/systems/camera_system.hpp>
#include <fuse/ecs/systems/culling_system.hpp>
#include <fuse/ecs/systems/scene_build_system.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/spatial/bvh.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testTransformHierarchy() {
    fuse::ecs::Registry registry;
    registry.init();

    const fuse::ecs::EntityID parent = registry.create();
    const fuse::ecs::EntityID child = registry.create();

    fuse::ecs::Transform parent_transform{};
    parent_transform.position = {1.f, 0.f, 0.f, 1.f};
    parent_transform.dirty = true;
    registry.add(parent, parent_transform);

    fuse::ecs::Transform child_transform{};
    child_transform.position = {0.f, 2.f, 0.f, 1.f};
    child_transform.parent = parent;
    child_transform.dirty = true;
    registry.add(child, child_transform);

    fuse::ecs::TransformSystem::update(registry);

    const fuse::ecs::Transform* updated = registry.get<fuse::ecs::Transform>(child);
    expectTrue(updated != nullptr, "child transform exists");
    expectTrue(updated->local_to_world.data[12] == 1.f, "child inherits parent translation on X");
    expectTrue(updated->local_to_world.data[13] == 2.f, "child keeps local translation on Y");
}

void testScenePipeline() {
    fuse::ecs::Registry registry;
    registry.init();

    const fuse::ecs::EntityID camera_entity = registry.create();
    fuse::ecs::Transform camera_transform{};
    camera_transform.position = {0.f, 0.f, -5.f, 1.f};
    camera_transform.dirty = true;
    registry.add(camera_entity, camera_transform);

    fuse::ecs::Camera camera{};
    camera.is_active = true;
    registry.add(camera_entity, camera);

    const fuse::ecs::EntityID mesh_entity = registry.create();
    fuse::ecs::Transform mesh_transform{};
    mesh_transform.position = {0.f, 0.f, 0.f, 1.f};
    mesh_transform.dirty = true;
    registry.add(mesh_entity, mesh_transform);

    fuse::ecs::Mesh mesh{};
    mesh.visible = true;
    mesh.index_count = 12;
    mesh.aabb_min = {-1.f, -1.f, -1.f, 0.f};
    mesh.aabb_max = {1.f, 1.f, 1.f, 0.f};
    registry.add(mesh_entity, mesh);

    fuse::jobs::JobScheduler::instance().shutdown();
    fuse::jobs::JobScheduler::instance().initialize(0);

    fuse::ecs::SystemScheduler scheduler;
    scheduler.register_system(
        {"TransformSystem", [&]() { fuse::ecs::TransformSystem::update(registry); }, {}});
    scheduler.register_system(
        {"CameraSystem", [&]() { fuse::ecs::CameraSystem::update(registry); }, {"TransformSystem"}});
    scheduler.run_all();

    std::vector<fuse::spatial::BVHLeaf> leaves;
    leaves.push_back({fuse::spatial::BVHLeafType::Mesh,
                      mesh_entity,
                      0,
                      {{-1.f, -1.f, -1.f, 0.f}, {1.f, 1.f, 1.f, 0.f}}});

    fuse::spatial::BVH bvh;
    bvh.build(leaves);

    const fuse::ecs::Camera* active_camera = registry.get<fuse::ecs::Camera>(camera_entity);
    expectTrue(active_camera != nullptr, "camera component exists");
    const fuse::ecs::CullResult visible =
        fuse::ecs::CullingSystem::cull(registry, *active_camera, bvh);
    const fuse::ecs::SceneData scene = fuse::ecs::SceneBuildSystem::build(registry, visible);

    expectTrue(!scene.draw_items.empty(), "scene build emits draw items for visible mesh");
    fuse::jobs::JobScheduler::instance().shutdown();
}

} // namespace

int main() {
    testTransformHierarchy();
    testScenePipeline();

    if (g_failures == 0) {
        std::printf("fuse_ecs systems tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs systems tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
