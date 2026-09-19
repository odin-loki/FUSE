#include <fuse/core/init.hpp>
#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/feature_pane_bridge.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testHostDrainsOnGameTick() {
    fuse::editor::EditorHost host;

    fuse::editor::EditorCommand cmd;
    cmd.kind = fuse::editor::CommandKind::SetProperty;
    cmd.propertyName = "project";
    cmd.propertyValue = "demo_3d_empty";
    host.postFromUi(std::move(cmd));

    expectTrue(host.commandQueue().pendingCount() == 1u, "command pending after UI post");
    expectTrue(host.gameTickCount() == 0u, "no game ticks before drain");

    host.gameTick();

    expectTrue(host.gameTickCount() == 1u, "game tick recorded");
    expectTrue(host.commandQueue().pendingCount() == 0u, "queue drained on game thread");
    expectTrue(host.commandQueue().appliedCount() == 1u, "command applied");
    expectTrue(host.loadedProject() == "demo_3d_empty", "project property applied on game thread");
    expectTrue(host.commandsAppliedLastTick() == 1u, "one command applied last tick");
}

void testHostMultipleTicks() {
    fuse::editor::EditorHost host;

    fuse::editor::EditorCommand deleteCmd;
    deleteCmd.kind = fuse::editor::CommandKind::DeleteObject;
    deleteCmd.target = fuse::Handle<fuse::Object>(3u, 1u);
    host.postFromUi(std::move(deleteCmd));

    host.gameTick();
    host.gameTick();

    expectTrue(host.gameTickCount() == 2u, "two game ticks");
    expectTrue(host.commandQueue().appliedCount() == 1u, "single command applied once");
}

void testHostPieStartStopViaQueue() {
    fuse::editor::EditorHost host;

    fuse::editor::EditorCommand start;
    start.kind = fuse::editor::CommandKind::StartPlay;
    host.postFromUi(std::move(start));

    host.gameTick();
    expectTrue(host.editorState().playing, "PIE start applied on game thread");
    expectTrue(host.playSession().isActive(), "play session active after start command");

    fuse::editor::EditorCommand stop;
    stop.kind = fuse::editor::CommandKind::StopPlay;
    host.postFromUi(std::move(stop));

    host.gameTick();
    expectTrue(!host.editorState().playing, "PIE stop applied on game thread");
    expectTrue(!host.playSession().isActive(), "play session stopped");
}

void testHostPieTicksOnGameThread() {
    fuse::editor::EditorHost host;
    const fuse::ecs::EntityID entity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Transform>(entity);

    fuse::editor::EditorCommand start;
    start.kind = fuse::editor::CommandKind::StartPlay;
    host.postFromUi(std::move(start));
    host.gameTick();

    const fuse::u32 ticksBefore = host.playSession().sessionTickCount();
    host.gameTick();
    host.gameTick();
    expectTrue(host.playSession().sessionTickCount() > ticksBefore,
               "PIE session advances only through gameTick while playing");
}

void testCrossThreadUiGameQueue() {
    fuse::editor::EditorHost host;
    std::atomic<bool> uiDone{false};
    constexpr fuse::u32 kPosts = 32u;

    std::thread uiThread([&]() {
        for (fuse::u32 i = 0; i < kPosts; ++i) {
            fuse::editor::EditorCommand cmd;
            cmd.kind = fuse::editor::CommandKind::SetProperty;
            cmd.propertyName = "project";
            cmd.propertyValue = "thread_" + std::to_string(i);
            host.postFromUi(std::move(cmd));
        }
        uiDone.store(true, std::memory_order_release);
    });

    std::thread gameThread([&]() {
        while (!uiDone.load(std::memory_order_acquire) || host.commandQueue().pendingCount() > 0u) {
            host.gameTick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    uiThread.join();
    gameThread.join();

    expectTrue(host.commandQueue().appliedCount() == kPosts,
               "all UI-thread posts drained on game thread");
    expectTrue(host.loadedProject() == "thread_" + std::to_string(kPosts - 1u),
               "last posted project value wins on game thread");
}

void testFeaturePaneBridgePostsPlayCommands() {
    fuse::editor::EditorHost host;
    fuse::editor::FeaturePaneBridge bridge(host);

    bridge.postPlayRequested();
    host.gameTick();
    expectTrue(host.editorState().playing, "feature pane play posts StartPlay command");

    bridge.postStopRequested();
    host.gameTick();
    expectTrue(!host.editorState().playing, "feature pane stop posts StopPlay command");
}

void testFeaturePaneBridgeSelectEntity() {
    fuse::editor::EditorHost host;
    fuse::editor::FeaturePaneBridge bridge(host);

    const fuse::ecs::EntityID entity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Transform>(entity);

    bridge.postSelectEntity(entity);
    host.gameTick();

    expectTrue(host.editorState().primarySelection == entity, "select entity applied on game thread");
    bridge.syncPropertyPane();
    expectTrue(bridge.propertyInspector().hasSelection(), "property pane syncs after selection");
}

void testHostDeleteObjectViaQueue() {
    fuse::editor::EditorHost host;
    const fuse::ecs::EntityID entity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Transform>(entity);

    fuse::editor::EditorCommand deleteCmd;
    deleteCmd.kind = fuse::editor::CommandKind::DeleteObject;
    deleteCmd.target = fuse::Handle<fuse::Object>(entity.index, entity.generation);
    host.postFromUi(std::move(deleteCmd));
    host.gameTick();

    expectTrue(!host.editorScene().registry().alive(entity), "delete command destroys entity on game thread");
    expectTrue(host.editorState().primarySelection != entity, "selection cleared after delete");
}

void testHostReparentObjectViaQueue() {
    fuse::editor::EditorHost host;
    const fuse::ecs::EntityID parent = host.editorScene().registry().create();
    const fuse::ecs::EntityID child = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Transform>(parent);
    host.editorScene().registry().add<fuse::ecs::Transform>(child);

    fuse::editor::EditorCommand reparent;
    reparent.kind = fuse::editor::CommandKind::ReparentObject;
    reparent.target = fuse::Handle<fuse::Object>(child.index, child.generation);
    reparent.parent = fuse::Handle<fuse::Object>(parent.index, parent.generation);
    host.postFromUi(std::move(reparent));
    host.gameTick();

    const fuse::ecs::Transform* childTransform = host.editorScene().registry().get<fuse::ecs::Transform>(child);
    expectTrue(childTransform != nullptr, "child transform still alive");
    expectTrue(childTransform->parent == parent, "reparent command updates transform parent on game thread");
}

void testHostSetPropertyTransformViaQueue() {
    fuse::editor::EditorHost host;
    fuse::editor::FeaturePaneBridge bridge(host);

    const fuse::ecs::EntityID entity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Transform>(entity);
    bridge.postSelectEntity(entity);
    host.gameTick();

    bridge.postSetProperty(entity, "transform.position", "1,2,3");
    host.gameTick();

    const fuse::ecs::Transform* transform = host.editorScene().registry().get<fuse::ecs::Transform>(entity);
    expectTrue(transform != nullptr, "transform present");
    expectTrue(transform->position.x == 1.f && transform->position.y == 2.f && transform->position.z == 3.f,
               "transform.position applied through command queue");
    expectTrue(host.editorState().sceneModified, "scene marked modified after property edit");
}

void testHostSetPropertyMeshMaterialViaQueue() {
    fuse::editor::EditorHost host;
    fuse::editor::FeaturePaneBridge bridge(host);

    const fuse::ecs::EntityID entity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Transform>(entity);
    host.editorScene().registry().add<fuse::ecs::Mesh>(entity);
    bridge.postSelectEntity(entity);
    host.gameTick();

    bridge.postSetProperty(entity, "mesh.material_id", "7");
    host.gameTick();

    const fuse::ecs::Mesh* mesh = host.editorScene().registry().get<fuse::ecs::Mesh>(entity);
    expectTrue(mesh != nullptr, "mesh component present");
    expectTrue(mesh->material_id == 7u, "mesh.material_id applied through command queue");
}

void testHostSetPropertySdfBlendViaQueue() {
    fuse::editor::EditorHost host;
    fuse::editor::FeaturePaneBridge bridge(host);

    const fuse::ecs::EntityID entity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Transform>(entity);
    host.editorScene().registry().add<fuse::ecs::SDFObject>(entity);
    bridge.postSelectEntity(entity);
    host.gameTick();

    bridge.postSetProperty(entity, "sdf.blend_alpha", "0.75");
    host.gameTick();

    const fuse::ecs::SDFObject* sdf = host.editorScene().registry().get<fuse::ecs::SDFObject>(entity);
    expectTrue(sdf != nullptr, "sdf component present");
    expectTrue(sdf->blend_alpha == 0.75f, "sdf.blend_alpha applied through command queue");
}

void testInspectorSectionsThroughBridge() {
    fuse::editor::EditorHost host;
    fuse::editor::FeaturePaneBridge bridge(host);

    const fuse::ecs::EntityID entity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Transform>(entity);
    host.editorScene().registry().add<fuse::ecs::Mesh>(entity);
    host.editorScene().registry().add<fuse::ecs::SDFObject>(entity);

    bridge.postSelectEntity(entity);
    host.gameTick();
    bridge.syncPropertyPane();

    expectTrue(bridge.propertyInspector().sections().size() >= 3u,
               "inspector exposes transform/mesh/sdf sections for selection");
}

void testHostUndoDeleteViaQueue() {
    fuse::editor::EditorHost host;
    fuse::editor::FeaturePaneBridge bridge(host);

    const fuse::ecs::EntityID entity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Transform>(entity);

    auto countTransforms = [&]() {
        fuse::u32 count = 0;
        host.editorScene().registry().each_query<fuse::ecs::Transform>(
            [&](fuse::ecs::EntityID /*id*/, fuse::ecs::Transform& /*transform*/) { ++count; });
        return count;
    };

    const fuse::u32 beforeDelete = countTransforms();

    bridge.postDeleteEntity(entity);
    host.gameTick();
    expectTrue(countTransforms() + 1u == beforeDelete, "delete removed one entity");

    bridge.postUndoRequested();
    host.gameTick();
    expectTrue(countTransforms() == beforeDelete, "undo restores deleted entity through queue");
    expectTrue(host.undoStack().canRedo(), "redo available after undo");
}

void testRuntimeViewportLoadsProjectRoot() {
    const std::string projectRoot = "/tmp/fuse_editor_viewport_project";
    std::filesystem::create_directories(projectRoot + "/worlds");
    {
        std::ofstream manifest(projectRoot + "/project.json");
        manifest << R"({
  "schemaVersion": 1,
  "name": "viewport_test",
  "dimensions": { "enable3D": true, "enable2D": false, "enableUI": false },
  "modules": { "ai": false, "cinematics": false, "fx": false, "mechanics": false, "adventure": false },
  "defaultWorld3D": "worlds/test.fuselevel",
  "defaultWorld2D": ""
})";
        std::ofstream world(projectRoot + "/worlds/test.fuselevel", std::ios::binary);
        world << "invalid";
    }

    fuse::editor::EditorHost host;

    fuse::editor::EditorCommand rootCmd;
    rootCmd.kind = fuse::editor::CommandKind::SetProperty;
    rootCmd.propertyName = "project.root";
    rootCmd.propertyValue = projectRoot;
    host.postFromUi(std::move(rootCmd));

    fuse::editor::EditorCommand projectCmd;
    projectCmd.kind = fuse::editor::CommandKind::SetProperty;
    projectCmd.propertyName = "project";
    projectCmd.propertyValue = "viewport_test";
    host.postFromUi(std::move(projectCmd));

    host.gameTick();
    host.gameTick();

    expectTrue(host.runtimeViewport().projectRoot() == projectRoot, "project root stored on viewport hook");
    expectTrue(host.runtimeViewport().embedSession().headlessPresentTicks >= 1u,
               "headless present stub ticks while embedded");
}

void testRuntimeViewportHookTicksWithProject() {
    fuse::editor::EditorHost host;

    fuse::editor::EditorCommand projectCmd;
    projectCmd.kind = fuse::editor::CommandKind::SetProperty;
    projectCmd.propertyName = "project";
    projectCmd.propertyValue = "demo_viewport";
    host.postFromUi(std::move(projectCmd));

    fuse::editor::EditorCommand widthCmd;
    widthCmd.kind = fuse::editor::CommandKind::SetProperty;
    widthCmd.propertyName = "viewport.width";
    widthCmd.propertyValue = "1280";
    host.postFromUi(std::move(widthCmd));

    fuse::editor::EditorCommand heightCmd;
    heightCmd.kind = fuse::editor::CommandKind::SetProperty;
    heightCmd.propertyName = "viewport.height";
    heightCmd.propertyValue = "720";
    host.postFromUi(std::move(heightCmd));

    host.gameTick();

    expectTrue(host.runtimeViewport().isEmbedded(), "runtime viewport embedded after project load");
    expectTrue(host.runtimeViewport().panel().width() == 1280u, "viewport width applied on game thread");
    expectTrue(host.runtimeViewport().panel().height() == 720u, "viewport height applied on game thread");
    expectTrue(host.runtimeViewport().runtimeTickCount() == 1u, "runtime viewport ticked on game thread");
    expectTrue(host.runtimeScene().name() == "demo_viewport", "runtime scene label synced from project");
}

} // namespace

int main() {
    fuse::core::initialize();
    testHostDrainsOnGameTick();
    testHostMultipleTicks();
    testHostPieStartStopViaQueue();
    testHostPieTicksOnGameThread();
    testCrossThreadUiGameQueue();
    testFeaturePaneBridgePostsPlayCommands();
    testFeaturePaneBridgeSelectEntity();
    testHostDeleteObjectViaQueue();
    testHostReparentObjectViaQueue();
    testHostSetPropertyTransformViaQueue();
    testHostSetPropertyMeshMaterialViaQueue();
    testHostSetPropertySdfBlendViaQueue();
    testInspectorSectionsThroughBridge();
    testHostUndoDeleteViaQueue();
    testRuntimeViewportLoadsProjectRoot();
    testRuntimeViewportHookTicksWithProject();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_host_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_host_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
