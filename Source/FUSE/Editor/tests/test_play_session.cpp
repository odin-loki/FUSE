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
    expectTrue(state.playing, "editor state marks playing on start");
    expectTrue(!state.paused, "editor state clears paused on start");
    expectTrue(physics.simulationActive, "play session enables physics on start");

    scene.addObjectName("RuntimeSpawn");
    scene.setName("Mutated");
    state.sceneModified = false;

    session.tick(1.f / 60.f, editorScene, physics);
    expectTrue(session.sessionTickCount() == 1u, "play session ticks while playing");
    expectTrue(physics.stepCount == 1u, "physics step count advances on tick");
    const fuse::ecs::Transform* playerAfterTick =
        editorScene.registry().get<fuse::ecs::Transform>(player);
    expectTrue(playerAfterTick != nullptr && playerAfterTick->dirty,
               "play tick marks transforms dirty");

    session.stop(editorScene, scene, state, physics);
    expectTrue(!session.isActive(), "play session stops");
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

} // namespace

int main() {
    fuse::core::initialize();
    testPlaySessionStartStopAndDirtyRestore();
    testPlaySessionPauseSkipsTick();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_play_session_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_play_session_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
