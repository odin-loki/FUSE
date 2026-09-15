#include <fuse/core/init.hpp>
#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/material_editor_panel.hpp>
#include <fuse/editor/property_inspector.hpp>
#include <fuse/editor/sdf_sculpt_panel.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>

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

void testPropertyInspectorListsComponents() {
    fuse::editor::EditorScene scene;
    scene.init();

    const fuse::ecs::EntityID entity = scene.registry().create();
    scene.registry().add<fuse::ecs::Transform>(entity);
    scene.registry().add<fuse::ecs::SDFObject>(entity);

    fuse::editor::EditorState state;
    state.primarySelection = entity;

    fuse::editor::PropertyInspector inspector;
    inspector.sync(state, scene);

    expectTrue(inspector.hasSelection(), "inspector tracks primary selection");
    expectTrue(inspector.sections().size() == 2u, "transform + sdf sections exposed");

    fuse::editor::CommandStack cmds;
    fuse::ecs::vec3 newPos{3.f, 4.f, 5.f, 1.f};
    expectTrue(inspector.setTransformPosition(newPos, scene, cmds), "transform edit accepted");
    expectTrue(cmds.appliedCount() == 1u, "transform edit posts command");

    const fuse::ecs::Transform* transform = scene.registry().get<fuse::ecs::Transform>(entity);
    expectTrue(transform != nullptr && transform->position.x == 3.f, "transform position updated");

    expectTrue(inspector.setSdfBlendAlpha(0.25f, scene, cmds), "sdf alpha edit accepted");
    expectTrue(cmds.appliedCount() == 2u, "sdf alpha posts second command");

    scene.destroy();
}

void testMaterialEditorPanelSelection() {
    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 3u);

    expectTrue(panel.catalogCount() == 3u, "material catalog size tracked");
    expectTrue(panel.selectMaterial(1u), "material selection succeeds");
    expectTrue(panel.selectedMaterialId() == 1u, "selected material id stored");
    expectTrue(panel.previewDirty(), "selection marks preview dirty");

    fuse::editor::CommandStack cmds;
    expectTrue(panel.setRoughness(0.2f, cmds), "roughness edit accepted");
    expectTrue(panel.editState().roughness == 0.2f, "roughness cached in edit state");
    expectTrue(cmds.appliedCount() == 1u, "roughness edit posts command");
}

void testSdfSculptPanelStrokeSpacing() {
    fuse::editor::EditorScene scene;
    scene.init();

    const fuse::ecs::EntityID entity = scene.registry().create();
    scene.registry().add<fuse::ecs::SDFObject>(entity);

    fuse::editor::EditorState state;
    state.primarySelection = entity;

    fuse::editor::SdfSculptPanel panel;
    panel.sync(state, scene);
    panel.setBrushRadius(2.f);
    panel.setBlendAlpha(0.5f);
    panel.setBrushOperation(fuse::editor::SdfSculptPanel::BrushOp::Subtract);

    expectTrue(panel.sculptActive(), "sdf selection enables sculpt mode");
    expectTrue(panel.brush().radius == 2.f, "brush radius stored");
    expectTrue(panel.brush().op == fuse::editor::SdfSculptPanel::BrushOp::Subtract, "brush op stored");

    fuse::editor::CommandStack cmds;
    fuse::ecs::vec3 hitA{0.f, 0.f, 0.f, 1.f};
    fuse::ecs::vec3 hitB{0.1f, 0.f, 0.f, 1.f};
    fuse::ecs::vec3 normal{0.f, 1.f, 0.f, 0.f};

    expectTrue(panel.handleBrushStroke(hitA, normal, scene, cmds), "first stroke accepted");
    expectTrue(!panel.handleBrushStroke(hitB, normal, scene, cmds), "stroke spacing suppresses near hit");
    expectTrue(panel.strokeCount() == 1u, "single stroke recorded");

    fuse::ecs::vec3 hitC{1.f, 0.f, 0.f, 1.f};
    expectTrue(panel.handleBrushStroke(hitC, normal, scene, cmds), "spaced stroke accepted");
    expectTrue(panel.strokeCount() == 2u, "second stroke recorded");
    expectTrue(cmds.appliedCount() == 2u, "stroke commands posted");

    scene.destroy();
}

} // namespace

int main() {
    fuse::core::initialize();
    testPropertyInspectorListsComponents();
    testMaterialEditorPanelSelection();
    testSdfSculptPanelStrokeSpacing();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_panels_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_panels_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
