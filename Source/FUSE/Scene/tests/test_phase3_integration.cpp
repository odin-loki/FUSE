#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/system_scheduler.hpp>
#include <fuse/ecs/systems/camera_system.hpp>
#include <fuse/ecs/systems/culling_system.hpp>
#include <fuse/ecs/systems/scene_build_system.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/scene/scene_manager.hpp>
#include <fuse/spatial/bvh.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool containsEntity(const std::vector<fuse::ecs::EntityID>& ids, fuse::ecs::EntityID id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

fuse::spatial::AABB makeWorldAabb(const fuse::ecs::Transform& transform, const fuse::ecs::Mesh& mesh) {
    const fuse::ecs::vec3 corners[2] = {mesh.aabb_min, mesh.aabb_max};
    fuse::ecs::vec3 world_min{1e30f, 1e30f, 1e30f, 0.f};
    fuse::ecs::vec3 world_max{-1e30f, -1e30f, -1e30f, 0.f};

    for (const fuse::ecs::vec3& corner : corners) {
        const fuse::ecs::vec3 world = fuse::ecs::transform_point(transform.local_to_world, corner);
        world_min.x = std::min(world_min.x, world.x);
        world_min.y = std::min(world_min.y, world.y);
        world_min.z = std::min(world_min.z, world.z);
        world_max.x = std::max(world_max.x, world.x);
        world_max.y = std::max(world_max.y, world.y);
        world_max.z = std::max(world_max.z, world.z);
    }

    return {world_min, world_max};
}

fuse::ecs::EntityID createMeshEntity(fuse::ecs::Registry& registry, const fuse::ecs::vec3& position,
                                     fuse::u32 index_count) {
    const fuse::ecs::EntityID entity = registry.create();
    fuse::ecs::Transform transform{};
    transform.position = position;
    transform.dirty = true;
    registry.add(entity, transform);

    fuse::ecs::Mesh mesh{};
    mesh.visible = true;
    mesh.index_count = index_count;
    mesh.aabb_min = {-1.f, -1.f, -1.f, 0.f};
    mesh.aabb_max = {1.f, 1.f, 1.f, 0.f};
    registry.add(entity, mesh);
    return entity;
}

void testPhase3EndToEndPipeline() {
    fuse::scene::SceneManager scene;
    fuse::scene::SceneManagerDesc desc{};
    desc.name = "Phase3Integration";
    desc.worldSize = 128.f;
    desc.hasVoxels = false;
    scene.init(desc);

    expectTrue(scene.isInitialized(), "scene manager initializes for phase-3 integration");
    expectTrue(scene.bvh().isInitialized(), "scene manager BVH stub is initialized");

    fuse::ecs::Registry& registry = scene.registry();
    const fuse::ecs::EntityID camera_entity = scene.createCamera(60.f, true);
    expectTrue(camera_entity.valid(), "scene manager createCamera returns valid entity");
    expectTrue(scene.activeCamera().index == camera_entity.index, "active camera tracked by scene manager");

    fuse::ecs::Transform camera_transform{};
    camera_transform.position = {0.f, 0.f, -10.f, 1.f};
    camera_transform.dirty = true;
    registry.add(camera_entity, camera_transform);

    const fuse::ecs::EntityID visible_mesh = createMeshEntity(registry, {0.f, 0.f, 0.f, 1.f}, 36);
    const fuse::ecs::EntityID culled_mesh = createMeshEntity(registry, {100.f, 0.f, 0.f, 1.f}, 12);

    fuse::jobs::JobScheduler::instance().shutdown();
    fuse::jobs::JobScheduler::instance().initialize(0);

    fuse::ecs::SystemScheduler scheduler;
    scheduler.register_system(
        {"TransformSystem", [&]() { fuse::ecs::TransformSystem::update(registry); }, {}});
    scheduler.register_system(
        {"CameraSystem", [&]() { fuse::ecs::CameraSystem::update(registry); }, {"TransformSystem"}});
    scheduler.run_all();

    const fuse::ecs::Transform* visible_transform = registry.get<fuse::ecs::Transform>(visible_mesh);
    const fuse::ecs::Transform* culled_transform = registry.get<fuse::ecs::Transform>(culled_mesh);
    const fuse::ecs::Mesh* visible_mesh_comp = registry.get<fuse::ecs::Mesh>(visible_mesh);
    const fuse::ecs::Mesh* culled_mesh_comp = registry.get<fuse::ecs::Mesh>(culled_mesh);
    expectTrue(visible_transform != nullptr && culled_transform != nullptr, "mesh transforms exist");
    expectTrue(visible_mesh_comp != nullptr && culled_mesh_comp != nullptr, "mesh components exist");

    std::vector<fuse::spatial::BVHLeaf> leaves;
    leaves.push_back({fuse::spatial::BVHLeafType::Mesh,
                      visible_mesh,
                      0,
                      makeWorldAabb(*visible_transform, *visible_mesh_comp)});
    leaves.push_back({fuse::spatial::BVHLeafType::Mesh,
                      culled_mesh,
                      0,
                      makeWorldAabb(*culled_transform, *culled_mesh_comp)});

    fuse::spatial::BVH bvh;
    bvh.build(leaves);
    expectTrue(bvh.node_count() > 0u, "spatial BVH builds from ECS mesh bounds");

    const fuse::ecs::Camera* camera = registry.get<fuse::ecs::Camera>(camera_entity);
    expectTrue(camera != nullptr, "active camera component exists");
    expectTrue(camera->view_projection.data[0] != 0.f || camera->view_projection.data[5] != 0.f,
               "camera system fills view_projection");

    const fuse::ecs::CullResult visible =
        fuse::ecs::CullingSystem::cull(registry, *camera, bvh);
    const fuse::ecs::SceneData scene_data = fuse::ecs::SceneBuildSystem::build(registry, visible);

    expectTrue(containsEntity(visible.visible_meshes, visible_mesh),
               "in-frustum mesh survives BVH + frustum cull");
    expectTrue(!containsEntity(visible.visible_meshes, culled_mesh),
               "off-frustum mesh is culled from visible set");
    expectTrue(scene_data.draw_items.size() == 1u, "scene build emits one draw item for visible mesh");
    expectTrue(scene_data.draw_items[0].entity.index == visible_mesh.index,
               "draw item references the visible mesh entity");

    scene.update(1.f / 60.f);
    expectTrue(scene.frameIndex() == 1u, "scene manager stub update advances frame index");

    fuse::jobs::JobScheduler::instance().shutdown();
}

} // namespace

int main() {
    testPhase3EndToEndPipeline();

    if (g_failures == 0) {
        std::printf("fuse_ecs phase3 integration tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs phase3 integration tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
