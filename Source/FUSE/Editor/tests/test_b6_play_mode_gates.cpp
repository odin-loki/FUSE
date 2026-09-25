// B6.12 / B6.13 gates — play mode snapshot on Play, exact restore on Stop, pause/resume.
#include <fuse/core/init.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/play_mode_controller.hpp>
#include <fuse/editor/play_session.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry_serialiser.hpp>
#include <fuse/scene/scene.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

std::vector<char> serialise(const fuse::ecs::Registry& registry, const char* tag) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / (std::string("fuse_b6_play_gate_") + tag + ".fecs");
    const fuse::ecs::RegistrySerialiseResult result = fuse::ecs::RegistrySerialiser::save(registry, path.string());
    expectTrue(result.ok, "RegistrySerialiser::save succeeds");
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return bytes;
}

/// Minimal stand-in for the physics step: integrates velocity into position.
void simulate(fuse::editor::EditorScene& scene, float dt) {
    scene.registry().each<fuse::ecs::Transform, fuse::ecs::RigidBody>(
        [dt](fuse::ecs::EntityID, fuse::ecs::Transform& t, fuse::ecs::RigidBody& body) {
            body.velocity.y -= 9.81f * dt;
            t.position.x += body.velocity.x * dt;
            t.position.y += body.velocity.y * dt;
            t.position.z += body.velocity.z * dt;
            t.dirty = true;
        });
}

struct World {
    fuse::editor::EditorScene scene;
    fuse::ecs::EntityID ball;
    fuse::ecs::EntityID crate;
    fuse::ecs::EntityID ground;
};

void buildWorld(World& world) {
    world.scene.init(64);
    fuse::ecs::Registry& r = world.scene.registry();
    world.ground = r.create();
    fuse::ecs::Transform groundT{};
    groundT.dirty = false;
    r.add(world.ground, groundT);
    r.add(world.ground, fuse::ecs::TagStatic{});

    world.ball = r.create();
    fuse::ecs::Transform ballT{};
    ballT.position = {0.25f, 10.f, -1.5f, 1.f};
    ballT.parent = world.ground;
    ballT.dirty = false;
    r.add(world.ball, ballT);
    fuse::ecs::RigidBody ballBody{};
    ballBody.velocity = {1.f, 0.f, 0.5f, 0.f};
    r.add(world.ball, ballBody);

    world.crate = r.create();
    fuse::ecs::Transform crateT{};
    crateT.position = {-3.f, 1.f, 2.f, 1.f};
    crateT.dirty = false;
    r.add(world.crate, crateT);
    fuse::ecs::RigidBody crateBody{};
    crateBody.velocity = {0.f, 4.f, 0.f, 0.f};
    crateBody.mass = 3.f;
    r.add(world.crate, crateBody);
    fuse::ecs::Mesh mesh{};
    mesh.material_id = 5u;
    r.add(world.crate, mesh);
}

// Gates: enter play takes a snapshot and initialises physics; stop restores the scene exactly
// (positions, velocities) — proven byte-for-byte through RegistrySerialiser.
void testStopRestoresExactly() {
    World world;
    buildWorld(world);
    fuse::scene::Scene runtime("PlayGate");
    fuse::editor::EditorState state;
    state.sceneModified = true;
    fuse::editor::PlayModePhysicsState physics;
    fuse::editor::PlaySession session;

    const std::vector<char> before = serialise(world.scene.registry(), "before");
    fuse::ecs::Transform& ballRef = *world.scene.registry().get<fuse::ecs::Transform>(world.ball);

    session.start(world.scene, runtime, state, physics);
    expectTrue(session.isPlaying() && state.playing && !state.paused, "enter play switches to Playing");
    expectTrue(session.hasRegistrySnapshot() && session.hasWorldSnapshot(), "snapshot taken on Play");
    expectTrue(physics.simulationActive && physics.stepCount == 0u, "physics initialised on Play");

    for (int frame = 0; frame < 120; ++frame) {
        session.tick(1.f / 60.f, world.scene, physics);
        simulate(world.scene, 1.f / 60.f);
    }
    expectTrue(physics.stepCount == 120u, "simulation stepped while playing");
    expectTrue(ballRef.position.y < 10.f &&
                   world.scene.registry().get<fuse::ecs::RigidBody>(world.ball)->velocity.y < 0.f,
               "play changed positions and velocities");

    session.stop(world.scene, runtime, state, physics);
    expectTrue(!session.isActive() && !state.playing && !physics.simulationActive, "stop returns to edit mode");
    expectTrue(serialise(world.scene.registry(), "after") == before,
               "stop restores the registry byte-for-byte (RegistrySerialiser image identical)");
    expectTrue(ballRef.position.y == 10.f && ballRef.position.x == 0.25f,
               "value-only play restores in place — component references stay valid");
    expectTrue(world.scene.registry().get<fuse::ecs::RigidBody>(world.crate)->velocity.y == 4.f,
               "velocity reset to the edit-time value");
    expectTrue(state.sceneModified, "edit-time dirty flag restored");
    expectTrue(!session.hasRegistrySnapshot(), "snapshot released after stop");
    world.scene.destroy();
}

// Stop also undoes structural play-time changes: spawned entities, destroyed entities (same id),
// added / removed components.
void testStopUndoesStructuralChanges() {
    World world;
    buildWorld(world);
    fuse::scene::Scene runtime("PlayGate");
    fuse::editor::EditorState state;
    fuse::editor::PlayModePhysicsState physics;
    fuse::editor::PlaySession session;
    const std::vector<char> before = serialise(world.scene.registry(), "struct_before");

    session.start(world.scene, runtime, state, physics);
    fuse::ecs::Registry& r = world.scene.registry();
    const fuse::ecs::EntityID projectile = r.create();
    r.add(projectile, fuse::ecs::Transform{});
    r.add(projectile, fuse::ecs::RigidBody{});
    r.destroy_entity(world.crate);
    r.add(world.ball, fuse::ecs::TagDestroy{});
    r.remove<fuse::ecs::TagStatic>(world.ground);
    simulate(world.scene, 0.5f);
    session.stop(world.scene, runtime, state, physics);

    fuse::ecs::Registry& after = world.scene.registry();
    expectTrue(!after.alive(projectile), "entity spawned during play is gone");
    expectTrue(after.alive(world.crate) && after.get<fuse::ecs::Mesh>(world.crate)->material_id == 5u,
               "entity destroyed during play is back with the same id and components");
    expectTrue(!after.has<fuse::ecs::TagDestroy>(world.ball), "component added during play removed");
    expectTrue(after.has<fuse::ecs::TagStatic>(world.ground), "component removed during play restored");
    expectTrue(after.count() == 3u, "entity count restored");
    expectTrue(serialise(after, "struct_after") == before, "structural restore is byte-exact");
    world.scene.destroy();
}

// Gate: pause/resume halts and continues the simulation without state corruption.
void testPauseResume() {
    World world;
    buildWorld(world);
    fuse::scene::Scene runtime("PlayGate");
    fuse::editor::EditorState state;
    fuse::editor::PlayModePhysicsState physics;
    fuse::editor::PlaySession session;
    const std::vector<char> before = serialise(world.scene.registry(), "pause_before");

    session.start(world.scene, runtime, state, physics);
    for (int i = 0; i < 10; ++i) {
        session.tick(1.f / 60.f, world.scene, physics);
    }
    const fuse::u32 stepsBeforePause = physics.stepCount;
    const float accumulatorBeforePause = session.tickAccumulator();

    session.pause(runtime, state, physics);
    expectTrue(session.isPaused() && state.paused && !physics.simulationActive, "pause halts simulation");
    for (int i = 0; i < 30; ++i) {
        session.tick(1.f / 60.f, world.scene, physics);
        expectTrue(session.consumeFixedSteps(1.f / 120.f, world.scene, physics) == 0u,
                   "no fixed steps drained while paused");
    }
    expectTrue(physics.stepCount == stepsBeforePause && session.tickAccumulator() == accumulatorBeforePause,
               "paused ticks do not advance steps or the accumulator");
    expectTrue(session.skippedInactiveTickCount() == 30u, "paused ticks counted as skipped");

    session.pause(runtime, state, physics);
    expectTrue(session.isPaused(), "double pause is harmless");
    session.resume(runtime, state, physics);
    expectTrue(session.isPlaying() && !state.paused && physics.simulationActive, "resume continues");
    session.tick(1.f / 60.f, world.scene, physics);
    expectTrue(physics.stepCount == stepsBeforePause + 1u, "simulation continues from where it paused");
    expectTrue(session.hasRegistrySnapshot(), "pause/resume keeps the edit-time snapshot");

    session.stop(world.scene, runtime, state, physics);
    expectTrue(serialise(world.scene.registry(), "pause_after") == before, "stop after pause restores exactly");
    world.scene.destroy();
}

// Play drives a real fuse::physics::PhysicsManager against the play registry; Stop restores the
// exact pre-play state, and a second Play replays the same fall bit-for-bit.
void testPlayDrivesPhysicsManager() {
    fuse::editor::EditorScene scene;
    scene.init(64);
    fuse::ecs::Registry& r = scene.registry();
    const fuse::ecs::EntityID ground = r.create();
    r.add(ground, fuse::ecs::Transform{});
    fuse::ecs::RigidBody groundBody{};
    groundBody.is_static = true;
    r.add(ground, groundBody);
    fuse::ecs::Collider plane{};
    plane.shape = fuse::ecs::Collider::Plane;
    plane.params = {0.f, 1.f, 0.f, 0.f};
    r.add(ground, plane);

    const fuse::ecs::EntityID ball = r.create();
    fuse::ecs::Transform ballT{};
    ballT.position = {0.5f, 5.f, -2.f, 1.f};
    ballT.dirty = false;
    r.add(ball, ballT);
    r.add(ball, fuse::ecs::RigidBody{});
    fuse::ecs::Collider sphere{};
    sphere.shape = fuse::ecs::Collider::Sphere;
    sphere.params = {0.5f, 0.f, 0.f, 0.f};
    r.add(ball, sphere);
    const fuse::ecs::EntityID prop = r.create(); // no physics components
    r.add(prop, fuse::ecs::Transform{});

    fuse::scene::Scene runtime("PhysicsPlay");
    fuse::editor::EditorState state;
    fuse::editor::PlayModePhysicsState physics;
    fuse::editor::PlaySession session;
    const std::vector<char> before = serialise(scene.registry(), "physics_before");

    auto runPlay = [&](int frames) {
        session.start(scene, runtime, state, physics);
        std::vector<float> heights;
        for (int i = 0; i < frames; ++i) {
            session.tick(1.f / 60.f, scene, physics);
            heights.push_back(scene.registry().get<fuse::ecs::Transform>(ball)->position.y);
        }
        return heights;
    };

    const std::vector<float> first = runPlay(90);
    expectTrue(session.physicsWorldLive(), "Play creates a live PhysicsManager");
    expectTrue(session.physicsWorld().stepCount() == 90u, "PhysicsManager stepped once per play tick");
    expectTrue(session.physicsWorld().bodies().count() == 2u, "Transform+RigidBody+Collider entities are bodies");
    bool falling = first.size() == 90u && first[0] < 5.f;
    for (std::size_t i = 1; i < 20u && falling; ++i) {
        falling = first[i] < first[i - 1];
    }
    expectTrue(falling, "body falls under gravity during Play");
    expectTrue(scene.registry().get<fuse::ecs::RigidBody>(ball)->velocity.y != 0.f ||
                   first.back() < 1.f,
               "physics wrote velocities / resting pose back into the play registry");
    expectTrue(first.back() > 0.25f && first.back() < 1.f, "ground plane stops the ball (rests on radius)");
    expectTrue(scene.registry().get<fuse::ecs::Transform>(ground)->position.y == 0.f, "static ground did not move");

    session.stop(scene, runtime, state, physics);
    expectTrue(!session.physicsWorldLive(), "Stop tears the physics world down");
    expectTrue(serialise(scene.registry(), "physics_after") == before,
               "Stop restores the exact pre-play registry (byte-identical image)");
    expectTrue(scene.registry().get<fuse::ecs::Transform>(ball)->position.y == 5.f &&
                   scene.registry().get<fuse::ecs::RigidBody>(ball)->velocity.y == 0.f,
               "ball back at its edit-time pose with zero velocity");

    const std::vector<float> second = runPlay(90);
    expectTrue(second == first, "second Play replays the identical fall (no stale physics state)");
    session.stop(scene, runtime, state, physics);
    expectTrue(serialise(scene.registry(), "physics_after2") == before, "second Stop restores exactly");

    // Fixed-step frames advance physics only in the fixed slices (no double integration).
    session.start(scene, runtime, state, physics);
    const fuse::u32 fixedSteps = session.tickFixedStep(1.f / 30.f, 1.f / 60.f, scene, physics);
    expectTrue(fixedSteps == 2u && session.physicsWorld().stepCount() == 2u,
               "tickFixedStep steps physics once per fixed slice");
    session.stop(scene, runtime, state, physics);

    // Injectable hook replaces the built-in manager.
    fuse::u32 hookCalls = 0;
    physics.stepHook = [&](fuse::ecs::Registry& registry, float dt) {
        ++hookCalls;
        registry.get<fuse::ecs::Transform>(ball)->position.y -= dt;
    };
    session.start(scene, runtime, state, physics);
    session.tick(0.5f, scene, physics);
    expectTrue(!session.physicsWorldLive() && hookCalls == 1u &&
                   scene.registry().get<fuse::ecs::Transform>(ball)->position.y == 4.5f,
               "step hook replaces the built-in PhysicsManager");
    session.stop(scene, runtime, state, physics);
    expectTrue(serialise(scene.registry(), "physics_after3") == before, "Stop after hook-driven play restores exactly");
    scene.destroy();
}

// Play pressed twice must not overwrite the edit-time snapshot with simulated state.
void testReenterPlayKeepsSnapshot() {
    fuse::scene::Scene scene("Controller");
    scene.addEntity("Hero");
    scene.entityAt(0)->transform.positionX = 1.f;

    fuse::editor::PlayModeController controller;
    fuse::editor::PlayModePhysicsState physics;
    controller.enterPlay(scene, physics);
    scene.entityAt(0)->transform.positionX = 50.f;
    physics.stepCount = 7u;
    controller.enterPlay(scene, physics); // ignored while already playing
    expectTrue(physics.stepCount == 7u, "re-entering play does not reset the running simulation");
    controller.stop(scene, physics);
    expectTrue(scene.entityAt(0)->transform.positionX == 1.f, "stop restores the original edit-time scene");
    expectTrue(controller.state() == fuse::editor::PlayModeController::State::Stopped, "stopped");
}

} // namespace

int main() {
    fuse::core::initialize();

    testStopRestoresExactly();
    testStopUndoesStructuralChanges();
    testPauseResume();
    testReenterPlayKeepsSnapshot();
    testPlayDrivesPhysicsManager();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_b6_play_mode_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_editor_b6_play_mode_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
