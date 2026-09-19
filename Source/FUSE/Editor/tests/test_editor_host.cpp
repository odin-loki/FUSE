#include <fuse/core/init.hpp>
#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/viewport_vulkan_surface.hpp>
#include <fuse/editor/ai_tree_profile_picker.hpp>
#include <fuse/editor/cinematics_seq_import.hpp>
#include <fuse/editor/feature_pane_bridge.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <fuse/renderer/vk/swapchain.hpp>
#endif
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

void testHostPropertyEditUndoRedo() {
    fuse::editor::EditorHost host;
    fuse::editor::FeaturePaneBridge bridge(host);

    const fuse::ecs::EntityID entity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Transform>(entity);

    bridge.postSelectEntity(entity);
    host.gameTick();

    const fuse::ecs::vec3 edited{1.f, 2.f, 3.f, 1.f};
    expectTrue(bridge.editTransformPosition(edited), "property edit records command stack");
    host.gameTick();

    const fuse::ecs::Transform* transform = host.editorScene().registry().get<fuse::ecs::Transform>(entity);
    expectTrue(transform != nullptr && transform->position.x == 1.f, "transform applied on game tick");
    expectTrue(host.commandStack().canUndo(), "property edit is undoable");

    bridge.undoPropertyEdit();
    expectTrue(transform->position.x == 0.f, "undo restores transform baseline");

    bridge.redoPropertyEdit();
    expectTrue(transform->position.x == 1.f, "redo reapplies transform edit");
    expectTrue(host.runtimeViewport().panel().tickCount() > 0u, "runtime viewport ticks with host");
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
    expectTrue(transform != nullptr && transform->position.x == 1.f && transform->position.y == 2.f &&
                   transform->position.z == 3.f,
               "transform.position applied through command queue");
    expectTrue(host.editorState().sceneModified, "scene marked modified after property edit");
    expectTrue(host.commandStack().canUndo(), "ui-thread property edit is undoable");
}

void testUiThreadPropertyCoalesceUndo() {
    fuse::editor::EditorHost host;
    fuse::editor::FeaturePaneBridge bridge(host);

    const fuse::ecs::EntityID entity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Transform>(entity);
    bridge.postSelectEntity(entity);
    host.gameTick();

    for (fuse::u32 step = 1; step <= 8; ++step) {
        bridge.postSetProperty(entity,
                               "transform.position",
                               std::to_string(step) + "," + std::to_string(step) + "," +
                                   std::to_string(step));
    }
    expectTrue(host.commandQueue().coalescedPostCount() >= 7u,
               "ui queue coalesces repeated transform.position posts");

    host.gameTick();

    const fuse::ecs::Transform* transform = host.editorScene().registry().get<fuse::ecs::Transform>(entity);
    expectTrue(transform != nullptr && transform->position.x == 8.f, "coalesced drag applies final value");
    expectTrue(host.commandStack().canUndo(), "coalesced ui edit records undo history");

    bridge.undoPropertyEdit();
    expectTrue(transform->position.x == 0.f, "undo restores pre-drag transform baseline");
}

void testHostSetPropertyMeshMaterialViaQueue() {
    fuse::editor::EditorHost host;
    fuse::editor::FeaturePaneBridge bridge(host);

    const fuse::ecs::EntityID entity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Mesh>(entity);
    bridge.postSelectEntity(entity);
    host.gameTick();

    bridge.postSetProperty(entity, "mesh.material_id", "7");
    host.gameTick();

    const fuse::ecs::Mesh* mesh = host.editorScene().registry().get<fuse::ecs::Mesh>(entity);
    expectTrue(mesh != nullptr && mesh->material_id == 7u,
               "mesh.material_id applied through command queue");
}

void testHostSetPropertySdfBlendViaQueue() {
    fuse::editor::EditorHost host;
    fuse::editor::FeaturePaneBridge bridge(host);

    const fuse::ecs::EntityID entity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::SDFObject>(entity);
    bridge.postSelectEntity(entity);
    host.gameTick();

    bridge.postSetProperty(entity, "sdf.blend_alpha", "0.75");
    host.gameTick();

    const fuse::ecs::SDFObject* sdf = host.editorScene().registry().get<fuse::ecs::SDFObject>(entity);
    expectTrue(sdf != nullptr && sdf->blend_alpha == 0.75f,
               "sdf.blend_alpha applied through command queue");
}

void testInspectorSectionsThroughBridge() {
    fuse::editor::EditorHost host;
    fuse::editor::FeaturePaneBridge bridge(host);

    const fuse::ecs::EntityID transformEntity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Transform>(transformEntity);
    bridge.postSelectEntity(transformEntity);
    host.gameTick();
    bridge.syncPropertyPane();
    expectTrue(bridge.propertyInspector().sections().size() == 1u,
               "inspector exposes transform section for selection");

    const fuse::ecs::EntityID meshEntity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Mesh>(meshEntity);
    bridge.postSelectEntity(meshEntity);
    host.gameTick();
    bridge.syncPropertyPane();
    expectTrue(bridge.propertyInspector().sections().size() == 1u,
               "inspector exposes mesh section for mesh-only entity");
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
    expectTrue(host.runtimeViewport().embedSession().wsiPresentPathTicks >= 1u ||
                   host.runtimeViewport().embedSession().headlessPresentTicks >= 1u,
               "present path ticks while embedded");
}

void testRuntimeViewportSurfaceHandoffStub() {
    fuse::editor::EditorHost host;

    host.runtimeViewport().setExternalSurfaceHandle(reinterpret_cast<void*>(0x2000), 960, 540);
    host.gameTick();

    expectTrue(host.runtimeViewport().swapchainHandoff().pending == false,
               "surface handoff consumed after game tick");
    expectTrue(host.runtimeViewport().embedSession().surfaceHandoffConsumed,
               "embed session records consumed handoff");
    expectTrue(host.runtimeViewport().embedSession().surfaceHandoffCount == 1u,
               "surface handoff counted once");
    expectTrue(host.runtimeViewport().swapchainHandoff().width == 960u,
               "handoff width preserved on stub path");
    expectTrue(host.runtimeViewport().swapchainHandoff().height == 540u,
               "handoff height preserved on stub path");
#if defined(FUSE_VULKAN_BACKEND)
    const fuse::renderer::SwapchainDesc desc = host.runtimeViewport().buildSwapchainDescHandoff();
    expectTrue(desc.width == 960u, "handoff swapchain width preserved");
    expectTrue(desc.height == 540u, "handoff swapchain height preserved");
    expectTrue(desc.surface.kind == fuse::renderer::SurfaceKind::External,
               "handoff surface kind external when handle provided");
#endif
}

void testViewportVulkanSurfaceBootstrapStub() {
    const fuse::editor::ViewportVulkanSurfaceResult result =
        fuse::editor::createViewportVulkanSurfaceFromWinId(4242u, 800u, 450u);

    expectTrue(result.valid, "viewport Vulkan bootstrap returns valid result on CI");
    expectTrue(result.stubPath, "headless bootstrap uses winId stub path");
    expectTrue(result.vkSurface == reinterpret_cast<void*>(static_cast<uintptr_t>(4242u)),
               "winId encoded as opaque surface handle");
    expectTrue(result.message != nullptr, "bootstrap exposes honest message");

    fuse::editor::ViewportVulkanSurfaceResult mutableResult = result;
    fuse::editor::destroyViewportVulkanSurface(mutableResult);
    expectTrue(!mutableResult.valid, "destroy clears bootstrap result");
}

void testRuntimeViewportQVulkanSurfaceHandoffCommand() {
    fuse::editor::EditorHost host;

    fuse::editor::EditorCommand stubCmd;
    stubCmd.kind = fuse::editor::CommandKind::SetProperty;
    stubCmd.propertyName = "viewport.vk_surface_qt_stub";
    stubCmd.propertyValue = "0";
    host.postFromUi(std::move(stubCmd));

    fuse::editor::EditorCommand widthCmd;
    widthCmd.kind = fuse::editor::CommandKind::SetProperty;
    widthCmd.propertyName = "viewport.width";
    widthCmd.propertyValue = "1024";
    host.postFromUi(std::move(widthCmd));

    fuse::editor::EditorCommand heightCmd;
    heightCmd.kind = fuse::editor::CommandKind::SetProperty;
    heightCmd.propertyName = "viewport.height";
    heightCmd.propertyValue = "768";
    host.postFromUi(std::move(heightCmd));

    host.gameTick();

    fuse::editor::EditorCommand surfaceCmd;
    surfaceCmd.kind = fuse::editor::CommandKind::SetProperty;
    surfaceCmd.propertyName = "viewport.vk_surface_handle";
    surfaceCmd.propertyValue = "9001";
    host.postFromUi(std::move(surfaceCmd));

    host.gameTick();

    expectTrue(host.runtimeViewport().swapchainHandoff().handoffSource != nullptr,
               "QVulkan handoff source string present");
    const std::string handoffSource(host.runtimeViewport().swapchainHandoff().handoffSource);
    expectTrue(handoffSource == "qt_winid_stub" || handoffSource == "qt_vulkan_instance",
               "handoff source tagged for stub or real Qt Vulkan path");
    if (handoffSource == "qt_vulkan_instance") {
        expectTrue(!host.runtimeViewport().swapchainHandoff().qtStubSurface,
                   "real Qt Vulkan path clears stub surface flag");
        expectTrue(host.runtimeViewport().swapchainHandoff().qtRealSurface,
                   "real Qt Vulkan path marks qtRealSurface");
    }
}

void testViewportVulkanBootstrapTeardownStress() {
    const fuse::editor::ViewportVulkanBootstrapStressResult stress =
        fuse::editor::stressViewportVulkanBootstrapTeardown(6u);

    expectTrue(stress.cyclesAttempted == 6u, "viewport teardown stress attempts all cycles");
    expectTrue(stress.cyclesCompleted == 6u, "viewport teardown stress completes all cycles");
#if defined(FUSE_HAS_QT_VULKAN)
    expectTrue(stress.stubPathCycles + stress.realSurfaceCycles == 6u,
               "Qt bootstrap stress records surface path per cycle");
#else
    expectTrue(stress.stubPathCycles == 6u, "headless CI uses winId stub path for every cycle");
#endif
}

void testRuntimeViewportQtSurfaceHandoffCommand() {
    fuse::editor::EditorHost host;

    fuse::editor::EditorCommand widthCmd;
    widthCmd.kind = fuse::editor::CommandKind::SetProperty;
    widthCmd.propertyName = "viewport.width";
    widthCmd.propertyValue = "800";
    host.postFromUi(std::move(widthCmd));

    fuse::editor::EditorCommand heightCmd;
    heightCmd.kind = fuse::editor::CommandKind::SetProperty;
    heightCmd.propertyName = "viewport.height";
    heightCmd.propertyValue = "450";
    host.postFromUi(std::move(heightCmd));

    host.gameTick();

    fuse::editor::EditorCommand surfaceCmd;
    surfaceCmd.kind = fuse::editor::CommandKind::SetProperty;
    surfaceCmd.propertyName = "viewport.vk_surface_handle";
    surfaceCmd.propertyValue = "4242";
    host.postFromUi(std::move(surfaceCmd));

    host.gameTick();

    expectTrue(host.runtimeViewport().swapchainHandoff().qtStubSurface,
               "Qt stub surface flag set on handoff");
    expectTrue(host.runtimeViewport().swapchainHandoff().handoffSource != nullptr,
               "handoff source string present");
    expectTrue(host.runtimeViewport().swapchainHandoff().nativeSurface ==
                   reinterpret_cast<void*>(static_cast<uintptr_t>(4242u)),
               "opaque Qt winId encoded as stub VkSurfaceKHR handle");
    expectTrue(host.runtimeViewport().swapchainHandoff().width == 800u,
               "Qt handoff preserves viewport width");
    expectTrue(host.runtimeViewport().swapchainHandoff().height == 450u,
               "Qt handoff preserves viewport height");
#if defined(FUSE_VULKAN_BACKEND)
    const fuse::renderer::SwapchainDesc desc = host.runtimeViewport().buildSwapchainDescHandoff();
    expectTrue(desc.surface.kind == fuse::renderer::SurfaceKind::External,
               "Qt stub handoff builds external swapchain desc");
#endif
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

void testAiTreeProfilePickerPostsCommand() {
    fuse::editor::EditorHost host;
    fuse::editor::AiTreeProfilePicker picker(host);

    expectTrue(picker.options().size() >= 4u, "tree profile options listed from UAISK hooks");
    picker.postSelectModule("aiBehaviors.cs");
    host.gameTick();
    expectTrue(host.selectedAiTreeProfileId() == 1u, "tree profile applied on game thread");
    expectTrue(picker.selectedUaiskModule() == "aiBehaviors.cs", "picker tracks UAISK module");
    expectTrue(picker.postCount() == 1u, "picker post counted");
}

void testAiAgentEntityBindingPostsCommand() {
    fuse::editor::EditorHost host;
    fuse::editor::AiTreeProfilePicker picker(host);

    const fuse::Handle<fuse::Object> entity(3, 1);
    picker.postBindAgentEntity(0, entity);
    host.gameTick();

    expectTrue(host.aiAgentEntityBindings().size() == 1u, "agent entity binding stored");
    expectTrue(host.aiAgentEntityBindings()[0].agentIndex == 0u, "agent index stored");
    expectTrue(host.aiAgentEntityBindings()[0].entity.index() == 3u, "entity index stored");
    expectTrue(host.aiAgentEntityBindings()[0].entity.generation() == 1u, "entity generation stored");
}

void testAiCodegenReloadPostsCommand() {
    fuse::editor::EditorHost host;
    fuse::editor::AiTreeProfilePicker picker(host);

    static const char* kCsText =
        "class PatrolSquad { behaviorTree = \"patrol_squad.bt\"; }\n";
    expectTrue(picker.postCodegenReload("aiBehaviors.cs", kCsText, 1u), "codegen reload posts command");
    host.gameTick();
    expectTrue(host.aiCodegenReloadCount() == 1u, "codegen reload applied on game thread");
}

void testAiAgentSelectionDeepen() {
    fuse::editor::EditorHost host;
    fuse::editor::AiTreeProfilePicker picker(host);

    picker.postSelectAgent(2u);
    host.gameTick();
    expectTrue(host.selectedAiAgentIndex() == 2u, "selected agent index applied");
    expectTrue(picker.selectedAgentIndex() == 2u, "picker tracks selected agent");
}

void testAiTreeFileReloadFromPicker() {
    namespace fs = std::filesystem;
    const fs::path tempPath = fs::temp_directory_path() / "fuse_picker_wave13.bt";
    {
        std::ofstream out(tempPath);
        out << "bb.action.set_flag flag=1\nroot=0\n";
    }

    fuse::editor::EditorHost host;
    fuse::editor::AiTreeProfilePicker picker(host);
    const fuse::ecs::EntityID entity = host.editorScene().registry().create();
    host.editorScene().registry().add<fuse::ecs::Transform>(entity);
    host.editorState().primarySelection = entity;

    expectTrue(picker.postBindSelectedEntityAndReloadTree(tempPath.string(), 1u),
               "picker binds entity and posts tree reload");
    host.gameTick();
    expectTrue(host.aiTreeFileReloadCount() == 1u, "tree file reload applied on game thread");
    expectTrue(host.aiAgentEntityBindings().size() == 1u, "entity binding stored with reload");

    fs::remove(tempPath);
}

void testCinematicsSeqScrubPreviewPostsSample() {
    fuse::editor::EditorHost host;
    fuse::editor::CinematicsSeqImport importer(host);

    expectTrue(importer.postImportEmbeddedOutpostIntro(), "seq import posts embedded asset");
    host.gameTick();
    expectTrue(importer.postScrubPreviewAtMs(2'500), "seq scrub preview posts scrub time");
    host.gameTick();
    expectTrue(host.cinematicsSeqScrubPreviewMs() == 2'500u, "seq scrub preview time on game thread");
    expectTrue(importer.scrubPreviewPostCount() == 1u, "seq scrub preview post counted");

    const fuse::editor::SeqPreviewPaneSample sample = importer.previewPaneSampleAtMs(2'500);
    expectTrue(sample.valid, "seq preview pane sample valid");
    expectTrue(sample.has_actor_events, "seq preview pane sees actor track");
    expectTrue(sample.mount_point == "cockpit", "seq preview pane mount point sampled");
    expectTrue(sample.mount_pitch_deg != 0.f, "seq preview pane mount pitch sampled");
}

void testCinematicsSeqImportPostsAsset() {
    fuse::editor::EditorHost host;
    fuse::editor::CinematicsSeqImport importer(host);

    expectTrue(importer.postImportEmbeddedOutpostIntro(), "seq import posts embedded asset");
    host.gameTick();
    expectTrue(!host.loadedCinematicsSeqAsset().empty(), "seq asset stored on game thread");
    expectTrue(host.loadedCinematicsSeqAsset().find("agent_3d mount") != std::string::npos,
               "seq asset contains actor mount line");
    expectTrue(importer.importCount() == 1u, "seq import counted");
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
    testHostPropertyEditUndoRedo();
    testFeaturePaneBridgeSelectEntity();
    testHostDeleteObjectViaQueue();
    testHostReparentObjectViaQueue();
    testHostSetPropertyTransformViaQueue();
    testUiThreadPropertyCoalesceUndo();
    testHostSetPropertyMeshMaterialViaQueue();
    testHostSetPropertySdfBlendViaQueue();
    testInspectorSectionsThroughBridge();
    testHostUndoDeleteViaQueue();
    testRuntimeViewportLoadsProjectRoot();
    testRuntimeViewportSurfaceHandoffStub();
    testRuntimeViewportQVulkanSurfaceHandoffCommand();
    testRuntimeViewportQtSurfaceHandoffCommand();
    testViewportVulkanSurfaceBootstrapStub();
    testViewportVulkanBootstrapTeardownStress();
    testRuntimeViewportHookTicksWithProject();
    testAiTreeProfilePickerPostsCommand();
    testAiAgentEntityBindingPostsCommand();
    testAiCodegenReloadPostsCommand();
    testAiAgentSelectionDeepen();
    testAiTreeFileReloadFromPicker();
    testCinematicsSeqScrubPreviewPostsSample();
    testCinematicsSeqImportPostsAsset();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_host_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_host_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
