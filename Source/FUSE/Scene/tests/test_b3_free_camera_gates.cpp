// B3.9 gate row (master plan): "Free camera moves through the scene with correct frustum culling
// visible in draw call count".
//
// A FreeCameraController flies the active SceneManager camera along a scripted path (forward
// flight, yaw sweep, pitch bob, strafe, climb) through 2000 mesh + SDF entities. Every step runs
// the real frame path (update -> BVH cull -> SceneBuildSystem) and checks:
//  - draw item / SDF counts == brute-force exhaustive frustum tests (and the same entity set),
//  - the frustum follows the controller: entities well inside the view cone are always drawn,
//    entities behind the camera or past the far plane never are (independent of the frustum code),
//  - the draw count visibly changes along the path and never reaches the full entity count.
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/systems/culling_system.hpp>
#include <fuse/scene/free_camera.hpp>
#include <fuse/scene/scene_manager.hpp>

#include <algorithm>
#include <cmath>
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

using fuse::f32;
using fuse::u32;
using fuse::ecs::EntityID;

constexpr u32 kEntities = 2000;
constexpr f32 kFar = 90.f;

struct Body {
    EntityID id;
    bool mesh;
};

std::vector<Body> spawn(fuse::scene::SceneManager& scene) {
    std::mt19937 rng(2744u);
    std::uniform_real_distribution<float> xz(-120.f, 120.f);
    std::uniform_real_distribution<float> y(-25.f, 25.f);
    std::vector<Body> bodies;
    auto& reg = scene.registry();
    for (u32 i = 0; i < kEntities; ++i) {
        const EntityID id = reg.create();
        fuse::ecs::Transform t{};
        t.position = {xz(rng), y(rng), xz(rng), 1.f};
        t.dirty = true;
        reg.add(id, t);
        const bool mesh = (i & 1u) == 0u;
        if (mesh) {
            fuse::ecs::Mesh m{};
            m.index_count = 36;
            m.aabb_min = {-0.5f, -0.5f, -0.5f, 1.f};
            m.aabb_max = {0.5f, 0.5f, 0.5f, 1.f};
            reg.add(id, m);
        } else {
            fuse::ecs::SDFObject sdf{};
            sdf.params = {0.5f, 0.f, 0.f, 0.f};
            reg.add(id, sdf);
        }
        bodies.push_back({id, mesh});
    }
    return bodies;
}

bool contains(const std::vector<EntityID>& sorted, EntityID id) {
    return std::binary_search(sorted.begin(), sorted.end(), id,
                              [](EntityID a, EntityID b) { return a.index < b.index; });
}

void testFreeCameraPathCulling() {
    fuse::scene::SceneManager scene;
    fuse::scene::SceneManagerDesc desc{};
    desc.hasVoxels = false;
    scene.init(desc);
    const std::vector<Body> bodies = spawn(scene);

    const EntityID cameraId = scene.createCamera(70.f, true);
    fuse::ecs::Camera* cam = scene.registry().get<fuse::ecs::Camera>(cameraId);
    cam->far_plane = kFar;
    cam->near_plane = 0.1f;
    cam->aspect_ratio = 16.f / 9.f;
    scene.registry().get<fuse::ecs::Transform>(cameraId)->position = {0.f, 0.f, -150.f, 1.f};

    fuse::scene::FreeCameraController controller;
    controller.moveSpeed = 12.f;
    controller.attach(scene.registry(), cameraId, 0.f, 0.f);

    const f32 dt = 1.f / 30.f;
    constexpr int kSteps = 360;
    std::size_t minDraws = SIZE_MAX;
    std::size_t maxDraws = 0;
    int countMismatches = 0;
    int setMismatches = 0;
    int coneMisses = 0;
    int behindDrawn = 0;
    int forwardMismatch = 0;
    std::size_t coneChecks = 0;

    for (int step = 0; step < kSteps; ++step) {
        fuse::scene::FreeCameraInput input{};
        const f32 t = static_cast<f32>(step) * dt;
        input.moveForward = 1.f;
        input.yawDeltaDeg = step < 120 ? 0.25f : (step < 240 ? 1.5f : -0.75f);
        input.pitchDeltaDeg = 0.6f * std::sin(t * 1.3f);
        input.moveRight = step >= 240 ? 0.5f * std::cos(t) : 0.f;
        input.moveUp = step >= 300 ? 0.4f : 0.f;
        input.boost = step >= 60 && step < 90;
        expectTrue(controller.update(scene.registry(), input, dt), "controller drives the camera entity");

        scene.update(dt);
        fuse::ecs::CullResult cull{};
        const fuse::ecs::SceneData data = scene.buildFrame(&cull);
        const fuse::ecs::Camera& camera = *scene.registry().get<fuse::ecs::Camera>(scene.activeCamera());
        const fuse::ecs::Transform& camT = *scene.registry().get<fuse::ecs::Transform>(cameraId);

        // The Transform the controller wrote is what CameraSystem looks along.
        const fuse::ecs::vec3 f = controller.forward();
        const f32 fdx = camT.local_to_world.data[8] - f.x;
        const f32 fdy = camT.local_to_world.data[9] - f.y;
        const f32 fdz = camT.local_to_world.data[10] - f.z;
        if (std::sqrt(fdx * fdx + fdy * fdy + fdz * fdz) > 1e-4f) {
            ++forwardMismatch;
        }

        // Brute force: exhaustive per-entity frustum tests.
        std::vector<EntityID> expectMeshes;
        std::vector<EntityID> expectSdfs;
        for (const Body& b : bodies) {
            const fuse::ecs::Transform& tr = *scene.registry().get<fuse::ecs::Transform>(b.id);
            if (b.mesh) {
                const fuse::spatial::AABB bb =
                    fuse::ecs::CullingSystem::world_bounds(tr, *scene.registry().get<fuse::ecs::Mesh>(b.id));
                if (fuse::ecs::CullingSystem::test_aabb_frustum(camera.frustum, bb.min, bb.max)) {
                    expectMeshes.push_back(b.id);
                }
            } else {
                const fuse::ecs::vec3 c{tr.local_to_world.data[12], tr.local_to_world.data[13],
                                        tr.local_to_world.data[14], 1.f};
                if (fuse::ecs::CullingSystem::test_sphere_frustum(
                        camera.frustum, c, scene.registry().get<fuse::ecs::SDFObject>(b.id)->params.x)) {
                    expectSdfs.push_back(b.id);
                }
            }
        }
        if (data.draw_items.size() != expectMeshes.size() || data.sdf_objects.size() != expectSdfs.size()) {
            ++countMismatches;
            if (countMismatches <= 3) {
                std::fprintf(stderr, "step %d: draws %zu (brute %zu), sdf %zu (brute %zu)\n", step,
                             data.draw_items.size(), expectMeshes.size(), data.sdf_objects.size(),
                             expectSdfs.size());
            }
        }

        std::vector<EntityID> drawn;
        for (const auto& item : data.draw_items) {
            drawn.push_back(item.entity);
        }
        for (const auto& item : data.sdf_objects) {
            drawn.push_back(item.entity);
        }
        std::vector<EntityID> expected = expectMeshes;
        expected.insert(expected.end(), expectSdfs.begin(), expectSdfs.end());
        const auto byIndex = [](EntityID a, EntityID b) { return a.index < b.index; };
        std::sort(drawn.begin(), drawn.end(), byIndex);
        std::sort(expected.begin(), expected.end(), byIndex);
        if (drawn != expected) {
            ++setMismatches;
        }

        // Independent geometric check against the controller pose (not the frustum planes).
        const fuse::ecs::vec3 eye = controller.position();
        const f32 coneCos = std::cos(25.f * 3.14159265f / 180.f); // inside 35 deg vertical half-FOV
        for (const Body& b : bodies) {
            const fuse::ecs::Transform& tr = *scene.registry().get<fuse::ecs::Transform>(b.id);
            const f32 dx = tr.position.x - eye.x;
            const f32 dy = tr.position.y - eye.y;
            const f32 dz = tr.position.z - eye.z;
            const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            const f32 along = dx * f.x + dy * f.y + dz * f.z;
            const bool isDrawn = contains(drawn, b.id);
            if (dist > 2.f && dist < kFar - 2.f && along > coneCos * dist) {
                ++coneChecks;
                coneMisses += isDrawn ? 0 : 1;
            }
            if ((along < -1.5f || along > kFar + 1.5f) && isDrawn) {
                ++behindDrawn;
            }
        }

        minDraws = std::min(minDraws, data.draw_items.size());
        maxDraws = std::max(maxDraws, data.draw_items.size());
    }

    const fuse::ecs::vec3 end = controller.position();
    std::printf("free camera: %d steps, end (%.1f, %.1f, %.1f) yaw %.1f pitch %.1f; mesh draws min %zu max %zu of %u; "
                "cone checks %zu, BVH rebuilds %u\n",
                kSteps, end.x, end.y, end.z, controller.yawDeg(), controller.pitchDeg(), minDraws, maxDraws,
                kEntities / 2u, coneChecks, scene.spatialBvhRebuildCount());
    expectTrue(forwardMismatch == 0, "camera Transform forward == controller forward every step");
    expectTrue(countMismatches == 0, "draw count == brute-force frustum count at every step");
    expectTrue(setMismatches == 0, "drawn entity set == brute-force visible set at every step");
    expectTrue(coneChecks > 1000u && coneMisses == 0, "entities inside the view cone are always drawn");
    expectTrue(behindDrawn == 0, "entities behind the camera or past the far plane are never drawn");
    expectTrue(maxDraws < kEntities / 2u, "frustum culling removes meshes (draw count < total)");
    expectTrue(minDraws + 20u < maxDraws, "draw count visibly changes as the camera moves");
    const f32 travelled = std::sqrt(end.x * end.x + end.y * end.y + (end.z + 150.f) * (end.z + 150.f));
    expectTrue(travelled > 50.f, "camera travelled along the path");
    scene.destroy();
}

void testControllerBasics() {
    fuse::ecs::Registry reg;
    reg.init(8);
    const EntityID cam = reg.create();
    reg.add(cam, fuse::ecs::Transform{});
    fuse::scene::FreeCameraController c;
    c.moveSpeed = 2.f;
    c.attach(reg, cam, 90.f, 0.f);
    fuse::scene::FreeCameraInput in{};
    in.moveForward = 1.f;
    expectTrue(c.update(reg, in, 0.5f), "update ok");
    expectTrue(std::fabs(c.position().x - 1.f) < 1e-5f && std::fabs(c.position().z) < 1e-5f,
               "yaw 90 flies along +X at moveSpeed");
    in = {};
    in.pitchDeltaDeg = 500.f;
    c.update(reg, in, 0.f);
    expectTrue(c.pitchDeg() == c.maxPitchDeg, "pitch clamps short of the pole");
    reg.destroy_entity(cam);
    expectTrue(!c.update(reg, in, 0.1f), "update refuses a destroyed camera entity");
}

} // namespace

int main() {
    testControllerBasics();
    testFreeCameraPathCulling();
    if (g_failures == 0) {
        std::printf("fuse_b3_free_camera_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b3_free_camera_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
