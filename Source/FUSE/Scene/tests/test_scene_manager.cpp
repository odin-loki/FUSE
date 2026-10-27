#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/scene/scene_manager.hpp>

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

void testSceneInitOwnsSpatialStructures() {
    fuse::scene::SceneManager scene;
    fuse::scene::SceneManagerDesc desc{};
    desc.name = "TestLevel";
    desc.worldSize = 128.f;
    desc.svoDepth = 4;
    desc.hasVoxels = true;

    scene.init(desc);

    expectTrue(scene.isInitialized(), "scene manager initializes");
    expectTrue(scene.bvh().isInitialized(), "scene manager owns BVH placeholder");
    expectTrue(scene.svo().isInitialized(), "scene manager owns SVO when voxels enabled");
    expectTrue(scene.desc().worldSize == 128.f, "scene manager retains descriptor");
}

void testSceneWithoutVoxels() {
    fuse::scene::SceneManager scene;
    fuse::scene::SceneManagerDesc desc{};
    desc.hasVoxels = false;
    scene.init(desc);

    expectTrue(scene.isInitialized(), "voxel-less scene manager initializes");
    expectTrue(!scene.svo().isInitialized(), "SVO skipped when hasVoxels=false");
}

void testSceneCameraAndUpdate() {
    fuse::scene::SceneManager scene;
    fuse::scene::SceneManagerDesc desc{};
    desc.worldSize = 64.f;
    desc.svoDepth = 3;
    scene.init(desc);

    const fuse::ecs::EntityID camera = scene.createCamera(75.f, true);
    expectTrue(camera.valid(), "createCamera returns valid entity");
    expectTrue(scene.registry().alive(camera), "createCamera entity alive in fuse_ecs registry");
    expectTrue(scene.registry().has<fuse::ecs::Camera>(camera), "createCamera adds Camera component");
    expectTrue(scene.activeCamera().index == camera.index, "active camera tracked");

    scene.update(1.f / 60.f);
    expectTrue(scene.frameIndex() == 1u, "update advances frame index");
}

void testSceneRayCastUsesBvhAndSvo() {
    fuse::scene::SceneManager scene;
    fuse::scene::SceneManagerDesc desc{};
    desc.worldSize = 64.f;
    desc.svoDepth = 3;
    scene.init(desc);

    scene.svo().set({2, 2, 2}, 1u);

    fuse::ecs::EntityID hit{};
    fuse::scene::f32 hitDistance = 0.f;
    const fuse::scene::vec3 origin{-100.f, 0.f, 0.f};
    const fuse::scene::vec3 direction{1.f, 0.f, 0.f};

    const bool bvhHit = scene.rayCast(origin, direction, 500.f, hit, hitDistance);
    expectTrue(bvhHit, "scene manager ray cast hits world bounds or voxels");
    expectTrue(hitDistance > 0.f, "scene manager ray cast reports distance");
}

void testSceneRayCastHitsMeshEntity() {
    fuse::scene::SceneManager scene;
    fuse::scene::SceneManagerDesc desc{};
    desc.worldSize = 64.f;
    desc.hasVoxels = false;
    scene.init(desc);

    const fuse::ecs::EntityID meshEntity = scene.registry().create();
    fuse::ecs::Transform transform{};
    transform.position = {10.f, 0.f, 0.f, 1.f};
    transform.local_to_world = fuse::ecs::mat4::identity();
    transform.local_to_world.data[12] = 10.f;
    transform.dirty = false;
    scene.registry().add<fuse::ecs::Transform>(meshEntity, transform);

    fuse::ecs::Mesh mesh{};
    mesh.aabb_min = {-1.f, -1.f, -1.f, 0.f};
    mesh.aabb_max = {1.f, 1.f, 1.f, 0.f};
    scene.registry().add<fuse::ecs::Mesh>(meshEntity, mesh);

    scene.update(1.f / 60.f);

    fuse::ecs::EntityID hit{};
    fuse::scene::f32 hitDistance = 0.f;
    const fuse::scene::vec3 origin{0.f, 0.f, 0.f};
    const fuse::scene::vec3 direction{1.f, 0.f, 0.f};
    const bool meshHit = scene.rayCast(origin, direction, 100.f, hit, hitDistance);
    expectTrue(meshHit, "ray cast hits mesh entity through spatial BVH");
    expectTrue(hit.index == meshEntity.index, "ray cast returns the mesh entity id");
    expectTrue(hitDistance >= 8.f && hitDistance <= 10.f, "ray cast distance matches translated mesh AABB");
}

void testSceneQuerySphereFindsMeshEntity() {
    fuse::scene::SceneManager scene;
    fuse::scene::SceneManagerDesc desc{};
    desc.worldSize = 64.f;
    desc.hasVoxels = false;
    scene.init(desc);

    const fuse::ecs::EntityID meshEntity = scene.registry().create();
    fuse::ecs::Transform transform{};
    transform.position = {5.f, 0.f, 0.f, 1.f};
    transform.local_to_world = fuse::ecs::mat4::identity();
    transform.local_to_world.data[12] = 5.f;
    transform.dirty = false;
    scene.registry().add<fuse::ecs::Transform>(meshEntity, transform);

    fuse::ecs::Mesh mesh{};
    mesh.aabb_min = {-1.f, -1.f, -1.f, 0.f};
    mesh.aabb_max = {1.f, 1.f, 1.f, 0.f};
    scene.registry().add<fuse::ecs::Mesh>(meshEntity, mesh);

    scene.update(1.f / 60.f);

    std::vector<fuse::ecs::EntityID> results;
    scene.querySphere({5.f, 0.f, 0.f}, 2.f, results);
    expectTrue(results.size() == 1u, "querySphere returns one mesh entity");
    expectTrue(results[0].index == meshEntity.index, "querySphere entity id matches");
}

void testSceneQuerySphereFindsVoxels() {
    fuse::scene::SceneManager scene;
    fuse::scene::SceneManagerDesc desc{};
    desc.worldSize = 64.f;
    desc.svoDepth = 3;
    scene.init(desc);

    scene.svo().set({1, 1, 1}, 1u);
    const fuse::scene::vec3 center = scene.svo().desc().origin + fuse::scene::vec3(8.f, 8.f, 8.f);

    std::vector<fuse::ecs::EntityID> results;
    scene.querySphere(center, 12.f, results);
    expectTrue(!results.empty(), "querySphere reports voxel hits in radius");
}

} // namespace

int main() {
    testSceneInitOwnsSpatialStructures();
    testSceneWithoutVoxels();
    testSceneCameraAndUpdate();
    testSceneRayCastUsesBvhAndSvo();
    testSceneRayCastHitsMeshEntity();
    testSceneQuerySphereFindsMeshEntity();
    testSceneQuerySphereFindsVoxels();

    if (g_failures == 0) {
        std::printf("fuse scene manager tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse scene manager tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
