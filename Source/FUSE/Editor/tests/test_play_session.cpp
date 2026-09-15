#include <fuse/core/init.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/play_session.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/scene/scene.hpp>

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

void expectNear(float actual, float expected, float epsilon, const char* message) {
    if (actual < expected - epsilon || actual > expected + epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %.6f expected %.6f)\n", message, actual, expected);
        ++g_failures;
    }
}

void testPlaySessionStartStopAndDirtyRestore() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();

    const fuse::ecs::EntityID player = editorScene.registry().create();
    fuse::ecs::Transform& playerTransform = editorScene.registry().add<fuse::ecs::Transform>(player);
    playerTransform.dirty = false;

    const fuse::ecs::EntityID camera = editorScene.registry().create();
    fuse::ecs::Transform& cameraTransform = editorScene.registry().add<fuse::ecs::Transform>(camera);
    cameraTransform.dirty = true;

    fuse::scene::Scene scene("EditorScene");
    scene.addObjectName("Player");
    scene.camera().setPosition(1.f, 2.f, 3.f);

    fuse::editor::EditorState state;
    state.sceneModified = true;

    fuse::editor::PlaySession session;
    fuse::editor::PlayModePhysicsState physics;

    session.start(editorScene, scene, state, physics);
    expectTrue(session.isActive(), "play session starts active");
    expectTrue(session.isPlaying(), "play session enters playing state");
    expectTrue(session.hasWorldSnapshot(), "play session captures world snapshot on start");
    expectTrue(state.playing, "editor state marks playing on start");
    expectTrue(!state.paused, "editor state clears paused on start");
    expectTrue(physics.simulationActive, "play session enables physics on start");
    expectTrue(session.tickAccumulator() == 0.f, "tick accumulator starts at zero");

    scene.addObjectName("RuntimeSpawn");
    scene.setName("Mutated");
    state.sceneModified = false;

    session.tick(1.f / 60.f, editorScene, physics);
    expectTrue(session.sessionTickCount() == 1u, "play session ticks while playing");
    expectTrue(session.tickAccumulator() > 0.f, "tick accumulator advances on tick");
    expectTrue(physics.stepCount == 1u, "physics step count advances on tick");
    const fuse::ecs::Transform* playerAfterTick =
        editorScene.registry().get<fuse::ecs::Transform>(player);
    expectTrue(playerAfterTick != nullptr && playerAfterTick->dirty,
               "play tick marks transforms dirty");

    session.stop(editorScene, scene, state, physics);
    expectTrue(!session.isActive(), "play session stops");
    expectTrue(!session.hasWorldSnapshot(), "play session clears world snapshot on stop");
    expectTrue(!state.playing, "editor state clears playing on stop");
    expectTrue(state.sceneModified, "play session restores scene modified flag");
    const fuse::ecs::Transform* playerAfterStop =
        editorScene.registry().get<fuse::ecs::Transform>(player);
    const fuse::ecs::Transform* cameraAfterStop =
        editorScene.registry().get<fuse::ecs::Transform>(camera);
    expectTrue(playerAfterStop != nullptr && !playerAfterStop->dirty,
               "play session restores transform dirty flags");
    expectTrue(cameraAfterStop != nullptr && cameraAfterStop->dirty,
               "play session restores per-entity dirty flags");
    expectTrue(scene.name() == "EditorScene", "play session restores scene snapshot");
    expectTrue(scene.objectCount() == 1u, "play session restores scene object table");
    expectTrue(session.sessionTickCount() == 0u, "play session resets tick count on stop");
    expectTrue(session.tickAccumulator() == 0.f, "play session resets tick accumulator on stop");

    editorScene.destroy();
}

void testPlaySessionPauseSkipsTick() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();

    const fuse::ecs::EntityID entity = editorScene.registry().create();
    editorScene.registry().add<fuse::ecs::Transform>(entity);

    fuse::scene::Scene scene("PauseTest");
    fuse::editor::EditorState state;
    fuse::editor::PlaySession session;
    fuse::editor::PlayModePhysicsState physics;

    session.start(editorScene, scene, state, physics);
    session.tick(1.f / 60.f, editorScene, physics);
    expectTrue(session.sessionTickCount() == 1u, "first tick recorded before pause");

    session.pause(scene, state, physics);
    expectTrue(session.isPaused(), "play session pauses");
    expectTrue(state.paused, "editor state marks paused");
    expectTrue(!physics.simulationActive, "pause disables physics simulation");

    session.tick(1.f / 60.f, editorScene, physics);
    expectTrue(session.sessionTickCount() == 1u, "paused session does not tick");
    expectTrue(physics.stepCount == 1u, "paused session does not advance physics steps");

    session.resume(scene, state, physics);
    expectTrue(session.isPlaying(), "play session resumes");
    expectTrue(!state.paused, "editor state clears paused on resume");
    expectTrue(physics.simulationActive, "resume re-enables physics simulation");

    session.tick(1.f / 60.f, editorScene, physics);
    expectTrue(session.sessionTickCount() == 2u, "resumed session ticks again");
    expectTrue(physics.stepCount == 2u, "resumed session advances physics steps");

    session.stop(editorScene, scene, state, physics);
    editorScene.destroy();
}

void testPlaySessionWorldSnapshotRoundtrip() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();

    const fuse::ecs::EntityID entity = editorScene.registry().create();
    fuse::ecs::Transform& transform = editorScene.registry().add<fuse::ecs::Transform>(entity);
    transform.position.x = 3.f;
    transform.position.y = 4.f;
    transform.position.z = 5.f;
    transform.dirty = false;

    fuse::editor::PlaySession session;
    const fuse::editor::PlayWorldSnapshot captured = session.captureWorldSnapshot(editorScene);
    expectTrue(captured.entities.size() == 1u, "world snapshot captures entity count");

    transform.position.x = 99.f;
    transform.position.y = 88.f;
    transform.position.z = 77.f;

    session.restoreWorldSnapshot(editorScene, captured);
    const fuse::ecs::Transform* restored = editorScene.registry().get<fuse::ecs::Transform>(entity);
    expectTrue(restored != nullptr, "world snapshot restore keeps entity alive");
    expectNear(restored->position.x, 3.f, 1e-4f, "world snapshot restores position x");
    expectNear(restored->position.y, 4.f, 1e-4f, "world snapshot restores position y");
    expectNear(restored->position.z, 5.f, 1e-4f, "world snapshot restores position z");

    editorScene.destroy();
}

void testPlaySessionEmptyWorld() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();

    fuse::scene::Scene scene("EmptyWorld");
    fuse::editor::EditorState state;
    fuse::editor::PlaySession session;
    fuse::editor::PlayModePhysicsState physics;

    const fuse::editor::PlayWorldSnapshot emptyCapture = session.captureWorldSnapshot(editorScene);
    expectTrue(emptyCapture.entities.empty(), "empty world snapshot has no entities");

    session.start(editorScene, scene, state, physics);
    expectTrue(session.hasWorldSnapshot(), "empty world play session captures snapshot on start");
    expectTrue(session.worldSnapshot().entities.empty(), "empty world play snapshot stays empty");
    expectTrue(session.isPlaying(), "empty world play session enters playing");

    session.tick(0.016f, editorScene, physics);
    expectTrue(session.sessionTickCount() == 1u, "empty world session still ticks");
    expectTrue(session.coalescedDirtyCount() == 0u, "empty world has no dirty coalesce events");

    scene.setName("MutatedEmpty");
    session.stop(editorScene, scene, state, physics);
    expectTrue(!session.isActive(), "empty world play session stops cleanly");
    expectTrue(scene.name() == "EmptyWorld", "empty world restores scene snapshot on stop");
    expectTrue(!session.hasWorldSnapshot(), "empty world clears snapshot on stop");

    session.restoreWorldSnapshot(editorScene, emptyCapture);
    expectTrue(session.captureWorldSnapshot(editorScene).entities.empty(),
               "empty world roundtrip leaves registry empty");

    editorScene.destroy();
}

void testPlaySessionDirtyCoalesceAndClearsOnStop() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();

    const fuse::ecs::EntityID entity = editorScene.registry().create();
    fuse::ecs::Transform& transform = editorScene.registry().add<fuse::ecs::Transform>(entity);
    transform.dirty = false;
    transform.position.x = 1.f;

    fuse::scene::Scene scene("CoalesceTest");
    fuse::editor::EditorState state;
    state.sceneModified = false;

    fuse::editor::PlaySession session;
    fuse::editor::PlayModePhysicsState physics;

    session.start(editorScene, scene, state, physics);
    session.tick(1.f / 60.f, editorScene, physics);
    expectTrue(transform.dirty, "first play tick marks transform dirty");
    expectTrue(session.coalescedDirtyCount() == 0u, "first dirty mark is not coalesced");

    session.tick(1.f / 60.f, editorScene, physics);
    expectTrue(session.coalescedDirtyCount() == 1u, "second tick coalesces already-dirty transform");
    expectTrue(session.sessionTickCount() == 2u, "second tick still advances session counter");

    transform.position.x = 42.f;
    session.stop(editorScene, scene, state, physics);

    expectTrue(!transform.dirty, "dirty clears on stop when pre-play was clean");
    expectNear(transform.position.x, 1.f, 1e-4f, "world snapshot restores mutated transform on stop");
    expectTrue(!state.sceneModified, "mark clean after stop preserves pre-play clean document flag");
    expectTrue(session.coalescedDirtyCount() == 0u, "coalesce counter resets on stop");

    editorScene.destroy();
}

void testPlaySessionIdempotentStartStop() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();

    const fuse::ecs::EntityID entity = editorScene.registry().create();
    editorScene.registry().add<fuse::ecs::Transform>(entity);

    fuse::scene::Scene scene("IdempotentTest");
    fuse::editor::EditorState state;
    fuse::editor::PlaySession session;
    fuse::editor::PlayModePhysicsState physics;

    session.start(editorScene, scene, state, physics);
    session.start(editorScene, scene, state, physics);
    expectTrue(session.isPlaying(), "second start is a no-op while active");
    expectTrue(session.sessionTickCount() == 0u, "idempotent start does not advance ticks");

    session.stop(editorScene, scene, state, physics);
    session.stop(editorScene, scene, state, physics);
    expectTrue(!session.isActive(), "second stop is a no-op while stopped");
    expectTrue(!state.playing, "idempotent stop leaves editor idle");

    editorScene.destroy();
}

void testPlaySessionConsumeFixedSteps() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();

    const fuse::ecs::EntityID entity = editorScene.registry().create();
    editorScene.registry().add<fuse::ecs::Transform>(entity);

    fuse::scene::Scene scene("FixedStepTest");
    fuse::editor::EditorState state;
    fuse::editor::PlaySession session;
    fuse::editor::PlayModePhysicsState physics;

    constexpr float kFixedDt = 1.f / 60.f;

    session.start(editorScene, scene, state, physics);
    session.tick(kFixedDt * 0.5f, editorScene, physics);
    expectTrue(session.consumeFixedSteps(kFixedDt, editorScene, physics) == 0u,
               "fixed steps wait until accumulator reaches threshold");

    session.tick(kFixedDt * 0.5f, editorScene, physics);
    expectTrue(session.consumeFixedSteps(kFixedDt, editorScene, physics) == 1u,
               "fixed steps drain one slice from accumulator");
    expectTrue(session.tickAccumulator() < kFixedDt, "fixed steps leave remainder in accumulator");
    expectTrue(session.sessionTickCount() == 3u,
               "fixed steps add to per-frame ticks from variable timestep stub");
    expectTrue(physics.stepCount == 3u, "fixed steps add to physics steps from variable stub");

    session.tick(kFixedDt * 3.f, editorScene, physics);
    expectTrue(session.consumeFixedSteps(kFixedDt, editorScene, physics) == 3u,
               "fixed steps drain multiple slices in one call");
    expectTrue(session.sessionTickCount() == 7u, "fixed steps accumulate with per-frame ticks");

    session.stop(editorScene, scene, state, physics);
    expectTrue(session.consumeFixedSteps(kFixedDt, editorScene, physics) == 0u,
               "fixed steps are skipped while stopped");

    editorScene.destroy();
}

void testPlaySessionFullTransformSnapshotRoundtrip() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();

    const fuse::ecs::EntityID first = editorScene.registry().create();
    fuse::ecs::Transform& firstTransform = editorScene.registry().add<fuse::ecs::Transform>(first);
    firstTransform.position.x = 1.f;
    firstTransform.rotation.y = 0.707f;
    firstTransform.rotation.w = 0.707f;
    firstTransform.scale.x = 2.f;
    firstTransform.scale.y = 2.f;
    firstTransform.scale.z = 2.f;

    const fuse::ecs::EntityID second = editorScene.registry().create();
    fuse::ecs::Transform& secondTransform = editorScene.registry().add<fuse::ecs::Transform>(second);
    secondTransform.position.z = 9.f;
    secondTransform.scale.x = 0.5f;

    fuse::editor::PlaySession session;
    const fuse::editor::PlayWorldSnapshot captured = session.captureWorldSnapshot(editorScene);
    expectTrue(captured.entityCount() == 2u, "full snapshot captures all entities");
    expectTrue(!captured.empty(), "full snapshot is not empty");
    expectTrue(captured.entities.front().first.index <= captured.entities.back().first.index,
               "full snapshot stores entities in stable id order");

    firstTransform.position.x = 99.f;
    firstTransform.rotation.x = 1.f;
    firstTransform.rotation.w = 0.f;
    firstTransform.scale.x = 5.f;
    secondTransform.position.z = 77.f;

    session.restoreWorldSnapshot(editorScene, captured);
    const fuse::ecs::Transform* restoredFirst =
        editorScene.registry().get<fuse::ecs::Transform>(first);
    const fuse::ecs::Transform* restoredSecond =
        editorScene.registry().get<fuse::ecs::Transform>(second);
    expectTrue(restoredFirst != nullptr && restoredSecond != nullptr,
               "full snapshot restore keeps entities alive");
    expectNear(restoredFirst->position.x, 1.f, 1e-4f, "full snapshot restores position");
    expectNear(restoredFirst->rotation.y, 0.707f, 1e-3f, "full snapshot restores rotation");
    expectNear(restoredFirst->scale.x, 2.f, 1e-4f, "full snapshot restores scale");
    expectNear(restoredSecond->position.z, 9.f, 1e-4f, "full snapshot restores second entity");

    editorScene.destroy();
}

void testPlaySessionTickFixedStep() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();

    const fuse::ecs::EntityID entity = editorScene.registry().create();
    editorScene.registry().add<fuse::ecs::Transform>(entity);

    fuse::scene::Scene scene("TickFixedStepTest");
    fuse::editor::EditorState state;
    fuse::editor::PlaySession session;
    fuse::editor::PlayModePhysicsState physics;

    constexpr float kFixedDt = 1.f / 60.f;

    session.start(editorScene, scene, state, physics);
    expectTrue(session.tickFixedStep(kFixedDt, kFixedDt, editorScene, physics) == 1u,
               "tickFixedStep drains one fixed slice per frame at target dt");
    expectTrue(session.sessionTickCount() == 2u,
               "tickFixedStep runs variable tick plus one fixed slice");
    expectTrue(session.tickAccumulator() < kFixedDt,
               "tickFixedStep leaves sub-fixed remainder in accumulator");

    session.stop(editorScene, scene, state, physics);
    expectTrue(session.tickFixedStep(kFixedDt, kFixedDt, editorScene, physics) == 0u,
               "tickFixedStep is guarded while stopped");

    editorScene.destroy();
}

void testPlaySessionDrainWorldSnapshot() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();

    const fuse::ecs::EntityID entity = editorScene.registry().create();
    fuse::ecs::Transform& transform = editorScene.registry().add<fuse::ecs::Transform>(entity);
    transform.position.x = 2.f;
    transform.position.y = 3.f;

    fuse::scene::Scene scene("DrainWorldTest");
    fuse::editor::EditorState state;
    fuse::editor::PlaySession session;
    fuse::editor::PlayModePhysicsState physics;

    session.start(editorScene, scene, state, physics);
    expectTrue(session.hasWorldSnapshot(), "drain test captures world snapshot on start");

    transform.position.x = 99.f;
    expectTrue(session.drainWorldSnapshot(editorScene),
               "drainWorldSnapshot applies stored snapshot");
    expectTrue(!session.hasWorldSnapshot(), "drainWorldSnapshot clears snapshot flag");
    expectNear(transform.position.x, 2.f, 1e-4f, "drainWorldSnapshot restores captured position");
    expectTrue(!session.drainWorldSnapshot(editorScene),
               "second drainWorldSnapshot is guarded when empty");

    session.stop(editorScene, scene, state, physics);
    editorScene.destroy();
}

void testPlaySessionDirtyFlagRestoreAndDrain() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();

    const fuse::ecs::EntityID entity = editorScene.registry().create();
    fuse::ecs::Transform& transform = editorScene.registry().add<fuse::ecs::Transform>(entity);
    transform.dirty = false;

    fuse::scene::Scene scene("DirtyDrainTest");
    fuse::editor::EditorState state;
    state.sceneModified = true;

    fuse::editor::PlaySession session;
    fuse::editor::PlayModePhysicsState physics;

    session.start(editorScene, scene, state, physics);
    expectTrue(session.hasDirtySnapshot(), "dirty snapshot captured on start");

    transform.dirty = true;
    state.sceneModified = false;

    session.restoreDirtyFlags(editorScene, state);
    expectTrue(!transform.dirty, "restoreDirtyFlags restores pre-play transform dirty flag");
    expectTrue(state.sceneModified, "restoreDirtyFlags restores pre-play scene modified flag");
    expectTrue(session.hasDirtySnapshot(), "restoreDirtyFlags keeps snapshot until drained");

    transform.dirty = true;
    state.sceneModified = false;
    expectTrue(session.drainDirtySnapshot(editorScene, state),
               "drainDirtySnapshot restores captured dirty flags");
    expectTrue(!session.hasDirtySnapshot(), "drainDirtySnapshot clears dirty snapshot flag");
    expectTrue(!transform.dirty, "drainDirtySnapshot restores transform dirty after replay");
    expectTrue(state.sceneModified, "drainDirtySnapshot restores scene modified after replay");
    expectTrue(!session.drainDirtySnapshot(editorScene, state),
               "second drainDirtySnapshot is guarded when empty");

    session.stop(editorScene, scene, state, physics);
    editorScene.destroy();
}

void testPlaySessionEmptySessionGuards() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();

    fuse::scene::Scene scene("GuardTest");
    fuse::editor::EditorState state;
    fuse::editor::PlaySession session;
    fuse::editor::PlayModePhysicsState physics;

    session.tick(0.016f, editorScene, physics);
    expectTrue(session.skippedInactiveTickCount() == 1u,
               "tick increments skipped counter while session inactive");
    expectTrue(session.sessionTickCount() == 0u, "inactive tick does not simulate");

    fuse::editor::PlayWorldSnapshot emptySnapshot;
    session.restoreWorldSnapshot(editorScene, emptySnapshot);
    expectTrue(!session.hasWorldSnapshot(), "empty restoreWorldSnapshot is a no-op guard");
    expectTrue(!session.drainWorldSnapshot(editorScene),
               "drainWorldSnapshot guarded with no captured snapshot");
    expectTrue(!session.hasDirtySnapshot(), "no dirty snapshot before start");
    session.restoreDirtyFlags(editorScene, state);
    expectTrue(!session.drainDirtySnapshot(editorScene, state),
               "drainDirtySnapshot guarded before capture");

    editorScene.destroy();
}

void testPlaySessionStartStopCycle() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();
    const fuse::ecs::EntityID entity = editorScene.registry().create();
    editorScene.registry().add<fuse::ecs::Transform>(entity);

    fuse::scene::Scene scene("CycleTest");
    scene.addObjectName("Prop");
    fuse::editor::EditorState state;
    fuse::editor::PlaySession session;
    fuse::editor::PlayModePhysicsState physics;

    for (int cycle = 0; cycle < 3; ++cycle) {
        session.start(editorScene, scene, state, physics);
        expectTrue(session.isPlaying(), "start/stop cycle enters playing");
        session.tick(0.016f, editorScene, physics);
        scene.addObjectName("Runtime");
        session.stop(editorScene, scene, state, physics);
        expectTrue(!session.isActive(), "start/stop cycle returns to stopped");
        expectTrue(scene.objectCount() == 1u, "start/stop cycle restores scene each iteration");
        expectTrue(session.tickAccumulator() == 0.f, "start/stop cycle resets tick accumulator");
    }

    editorScene.destroy();
}

} // namespace

int main() {
    fuse::core::initialize();
    testPlaySessionStartStopAndDirtyRestore();
    testPlaySessionPauseSkipsTick();
    testPlaySessionWorldSnapshotRoundtrip();
    testPlaySessionEmptyWorld();
    testPlaySessionDirtyCoalesceAndClearsOnStop();
    testPlaySessionIdempotentStartStop();
    testPlaySessionConsumeFixedSteps();
    testPlaySessionTickFixedStep();
    testPlaySessionDrainWorldSnapshot();
    testPlaySessionDirtyFlagRestoreAndDrain();
    testPlaySessionEmptySessionGuards();
    testPlaySessionStartStopCycle();
    testPlaySessionFullTransformSnapshotRoundtrip();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_play_session_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_play_session_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
