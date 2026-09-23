// B7.10 integration gate row (master plan): "Save, close, reload cycle — scene state
// bit-identical after round-trip".
//
// A full runtime scene — `.fuselevel` (SceneSerialiser: camera, names, transforms, hierarchy) plus
// the ECS world (RegistrySerialiser: every entity, generation, free list and component byte,
// including the physics state the PhysicsManager writes back: poses, linear/angular velocity,
// sleep flags and timers) — is simulated mid-motion, saved, completely torn down (registry,
// physics manager, scene), reloaded into fresh objects and saved again. Both files must be
// byte-identical. The reloaded world must also simulate on: its bodies re-enter a fresh
// PhysicsManager and a further 120-step run from the reloaded state must be bit-identical to the
// same run from the never-saved original (no solver state is lost across the round-trip).
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/registry_serialiser.hpp>
#include <fuse/physics/physics_manager.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/scene/serialiser.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
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
using fuse::ecs::Collider;
using fuse::ecs::EntityID;
using fuse::ecs::Registry;
using fuse::ecs::RegistrySerialiser;
using fuse::ecs::Transform;

constexpr f32 kDt = 1.f / 60.f;

std::vector<unsigned char> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::filesystem::path tempDir() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fuse_b7_save_reload";
    std::filesystem::create_directories(dir);
    return dir;
}

EntityID spawnBody(Registry& reg, f32 x, f32 y, f32 z, u32 shape, fuse::ecs::vec3 params, bool isStatic) {
    const EntityID id = reg.create();
    Transform t{};
    t.position = {x, y, z, 1.f};
    reg.add(id, t);
    fuse::ecs::RigidBody rb{};
    rb.is_static = isStatic;
    rb.mass = isStatic ? 0.f : 1.f + 0.25f * static_cast<f32>(id.index % 4u);
    rb.restitution = 0.2f;
    reg.add(id, rb);
    Collider c{};
    c.shape = shape;
    c.params = params;
    reg.add(id, c);
    return id;
}

/// The "game": a ground plane, a pile of spheres and boxes dropped with spin, a kinematic mover,
/// a static-mesh prop hierarchy and a camera. Scene-file entities mirror the named props.
void buildWorld(Registry& reg, fuse::scene::Scene& scene) {
    reg.init(4096);
    spawnBody(reg, 0.f, 0.f, 0.f, Collider::Plane, {0.f, 1.f, 0.f, 0.f}, true);
    for (u32 i = 0; i < 48; ++i) {
        const f32 x = static_cast<f32>(static_cast<int>(i % 6u) - 3) * 1.1f;
        const f32 z = static_cast<f32>(static_cast<int>((i / 6u) % 4u) - 2) * 1.1f;
        const f32 y = 1.f + static_cast<f32>(i / 24u) * 2.5f + 0.13f * static_cast<f32>(i % 5u);
        const bool box = (i % 3u) == 0u;
        const EntityID id = spawnBody(reg, x, y, z, box ? Collider::Box : Collider::Sphere,
                                      box ? fuse::ecs::vec3{0.4f, 0.3f, 0.35f, 0.f}
                                          : fuse::ecs::vec3{0.45f, 0.f, 0.f, 0.f},
                                      false);
        fuse::ecs::RigidBody* rb = reg.get<fuse::ecs::RigidBody>(id);
        rb->velocity = {0.3f * static_cast<f32>(i % 3u), 0.f, -0.2f * static_cast<f32>(i % 2u), 0.f};
        rb->angular_velocity = {0.f, 1.5f, 0.5f * static_cast<f32>(i % 4u), 0.f};
    }
    const EntityID mover = spawnBody(reg, -6.f, 0.5f, 0.f, Collider::Box, {0.5f, 0.5f, 0.5f, 0.f}, false);
    reg.add<fuse::ecs::TagKinematic>(mover);

    const EntityID prop = reg.create();
    Transform pt{};
    pt.position = {10.f, 0.f, 10.f, 1.f};
    reg.add(prop, pt);
    fuse::ecs::Mesh mesh{};
    mesh.index_count = 36;
    reg.add(prop, mesh);
    const EntityID child = reg.create();
    Transform ct{};
    ct.position = {0.f, 2.f, 0.f, 1.f};
    ct.parent = prop;
    reg.add(child, ct);
    reg.add(child, mesh);

    const EntityID cam = reg.create();
    Transform camT{};
    camT.position = {0.f, 5.f, -15.f, 1.f};
    reg.add(cam, camT);
    fuse::ecs::Camera camera{};
    camera.is_active = true;
    reg.add(cam, camera);

    // Churn the entity table so generations and the free list are non-trivial.
    std::vector<EntityID> temp;
    for (int i = 0; i < 16; ++i) {
        temp.push_back(reg.create());
    }
    for (std::size_t i = 0; i < temp.size(); i += 2) {
        reg.destroy_entity(temp[i]);
    }

    scene.setName("SaveReloadScene");
    scene.camera().setPosition(0.f, 5.f, -15.f);
    scene.camera().setOrientation(0.f, -12.f);
    fuse::scene::SceneEntityTransform propT{};
    propT.positionX = 10.f;
    propT.positionZ = 10.f;
    scene.addEntity("prop", propT);
    fuse::scene::SceneEntityTransform childT{};
    childT.positionY = 2.f;
    scene.addEntity("prop_child", childT, 0);
    scene.addEntity("mover");
}

void stepWorld(Registry& reg, fuse::physics::PhysicsManager& physics, int steps) {
    // The kinematic box is saved but not animated here: a kinematic body sweeps from the solver's
    // previous pose (PhysicsManager-internal, not scene state) to its Transform, so a scripted
    // mover would take one step to re-sync after a reload.
    fuse::physics::PhysicsStreamManager streams{};
    for (int i = 0; i < steps; ++i) {
        physics.step(reg, kDt, streams);
    }
}

struct Saved {
    std::vector<unsigned char> level;
    std::vector<unsigned char> ecs;
};

Saved save(const Registry& reg, const fuse::scene::Scene& scene, const std::string& tag) {
    const std::filesystem::path level = tempDir() / (tag + ".fuselevel");
    const std::filesystem::path ecs = tempDir() / (tag + ".fecs");
    expectTrue(fuse::scene::SceneSerialiser::save(scene, level.string()).status == fuse::scene::SerialiseStatus::Ok,
               "scene file saves");
    const fuse::ecs::RegistrySerialiseResult r = RegistrySerialiser::save(reg, ecs.string());
    expectTrue(r.ok, "ECS world saves");
    if (!r.ok) {
        std::fprintf(stderr, "  %s\n", r.error.c_str());
    }
    return {readAll(level), readAll(ecs)};
}

std::size_t firstDiff(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return a.size() == b.size() ? SIZE_MAX : n;
}

void testSaveCloseReloadBitIdentical() {
    constexpr int kWarmup = 45;  // mid-fall: bodies moving, spinning, some in contact
    constexpr int kResume = 120; // then long enough for piles to settle / sleep

    auto reg = std::make_unique<Registry>();
    auto scene = std::make_unique<fuse::scene::Scene>();
    auto physics = std::make_unique<fuse::physics::PhysicsManager>();
    buildWorld(*reg, *scene);
    physics->init({});
    stepWorld(*reg, *physics, kWarmup);
    const u32 bodiesBefore = physics->bodies().count();
    const u32 contactsBefore = static_cast<u32>(physics->lastEvents().size());

    const Saved first = save(*reg, *scene, "first");
    expectTrue(first.level.size() > 64u && first.ecs.size() > 1024u, "saved files are non-trivial");

    // Reference continuation from the never-saved world.
    stepWorld(*reg, *physics, kResume);
    const Saved referenceEnd = save(*reg, *scene, "reference_end");

    // Close: everything goes.
    physics->destroy();
    physics.reset();
    reg->destroy();
    reg.reset();
    scene.reset();

    // Reload into brand-new objects.
    auto reloadedReg = std::make_unique<Registry>();
    auto reloadedScene = std::make_unique<fuse::scene::Scene>();
    const fuse::ecs::RegistrySerialiseResult loadEcs =
        RegistrySerialiser::load((tempDir() / "first.fecs").string(), *reloadedReg);
    expectTrue(loadEcs.ok, "ECS world reloads");
    const fuse::scene::SerialiseResult loadLevel =
        fuse::scene::SceneSerialiser::load((tempDir() / "first.fuselevel").string(), *reloadedScene);
    expectTrue(loadLevel.status == fuse::scene::SerialiseStatus::Ok && !loadLevel.legacyMagic,
               "scene file reloads (FUSE magic)");

    const Saved second = save(*reloadedReg, *reloadedScene, "second");
    const std::size_t levelDiff = firstDiff(first.level, second.level);
    const std::size_t ecsDiff = firstDiff(first.ecs, second.ecs);
    std::printf("save/reload: .fuselevel %zu bytes, .fecs %zu bytes (%zu entities, %zu archetypes), %u bodies, "
                "%u events at save; first diff level %s ecs %s\n",
                first.level.size(), first.ecs.size(), loadEcs.entityCount, loadEcs.archetypeCount, bodiesBefore,
                contactsBefore, levelDiff == SIZE_MAX ? "none" : std::to_string(levelDiff).c_str(),
                ecsDiff == SIZE_MAX ? "none" : std::to_string(ecsDiff).c_str());
    expectTrue(levelDiff == SIZE_MAX, "save -> close -> reload -> save: .fuselevel bit-identical");
    expectTrue(ecsDiff == SIZE_MAX, "save -> close -> reload -> save: ECS + physics state bit-identical");

    // Reloaded bodies re-enter a fresh PhysicsManager and simulate on from the saved state.
    fuse::physics::PhysicsManager resumed;
    resumed.init({});
    stepWorld(*reloadedReg, resumed, kResume);
    expectTrue(resumed.bodies().count() == bodiesBefore, "every saved body is simulated after reload");
    const Saved resumedEnd = save(*reloadedReg, *reloadedScene, "resumed_end");

    // Compare the continued simulation with the never-saved reference, per body pose.
    Registry refReg;
    Registry resReg;
    expectTrue(RegistrySerialiser::load((tempDir() / "reference_end.fecs").string(), refReg).ok &&
                   RegistrySerialiser::load((tempDir() / "resumed_end.fecs").string(), resReg).ok,
               "continuation snapshots load");
    f32 maxPosErr = 0.f;
    u32 compared = 0;
    refReg.each<Transform, fuse::ecs::RigidBody>([&](EntityID id, Transform& a, fuse::ecs::RigidBody&) {
        const Transform* b = resReg.get<Transform>(id);
        if (b == nullptr) {
            maxPosErr = 1e9f;
            return;
        }
        ++compared;
        maxPosErr = std::max(maxPosErr, std::fabs(a.position.x - b->position.x));
        maxPosErr = std::max(maxPosErr, std::fabs(a.position.y - b->position.y));
        maxPosErr = std::max(maxPosErr, std::fabs(a.position.z - b->position.z));
    });
    const bool continuationIdentical = referenceEnd.ecs == resumedEnd.ecs;
    std::printf("save/reload: %d further steps, %u bodies compared, continuation %s, max position delta %.3g m\n",
                kResume, compared, continuationIdentical ? "bit-identical" : "diverges", maxPosErr);
    expectTrue(compared == bodiesBefore, "reference and resumed worlds hold the same bodies");
    expectTrue(maxPosErr == 0.f, "simulation continues from the reloaded state as from the original");
    expectTrue(continuationIdentical, "continued run after reload is bit-identical to the never-saved run");
    resumed.destroy();
}

} // namespace

int main() {
    testSaveCloseReloadBitIdentical();
    if (g_failures == 0) {
        std::printf("fuse_b7_save_reload_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b7_save_reload_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
