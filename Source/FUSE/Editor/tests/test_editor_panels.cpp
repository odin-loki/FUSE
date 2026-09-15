#include <fuse/core/init.hpp>
#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/material_editor_panel.hpp>
#include <fuse/editor/property_inspector.hpp>
#include <fuse/editor/sdf_sculpt_panel.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>

#ifdef FUSE_VULKAN_BACKEND
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/material/material_system.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#endif

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
    expectTrue(!panel.editDirty(), "selection alone does not mark edit dirty");

    fuse::editor::CommandStack cmds;
    expectTrue(panel.setRoughness(0.2f, cmds), "roughness edit accepted");
    expectTrue(panel.editState().roughness == 0.2f, "roughness cached in edit state");
    expectTrue(cmds.appliedCount() == 1u, "roughness edit posts command");
    expectTrue(panel.editDirty(), "property edit marks edit dirty");
    expectTrue(cmds.isDirty(), "material edit marks command stack dirty");
}

void testMaterialEditorPanelPropertyBindings() {
    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 2u);
    panel.selectMaterial(0u);
    panel.clearPreviewDirty();
    panel.clearEditDirty();

    fuse::editor::CommandStack cmds;
    expectTrue(panel.setMetallic(0.75f, cmds), "metallic edit accepted");
    expectTrue(panel.editState().metallic == 0.75f, "metallic cached in edit state");

    expectTrue(panel.setBaseColor(0.1f, 0.2f, 0.3f, cmds), "base color edit accepted");
    expectTrue(panel.editState().baseColorR == 0.1f, "base color r cached");
    expectTrue(panel.editState().baseColorG == 0.2f, "base color g cached");
    expectTrue(panel.editState().baseColorB == 0.3f, "base color b cached");

    expectTrue(panel.setShadingModel(2u, cmds), "shading model edit accepted");
    expectTrue(panel.editState().shadingModel == 2u, "shading model cached");

    expectTrue(cmds.appliedCount() == 3u, "three distinct property edits posted");
    expectTrue(cmds.undoDepth() == 3u, "each property binding is its own undo step");

    const fuse::editor::EditorCommand* last = cmds.lastApplied();
    expectTrue(last != nullptr && last->propertyName == "material.shadingModel",
               "last binding targets shading model");
    expectTrue(last != nullptr && last->target.index() == 0u, "material id encoded in command target");
}

void testMaterialEditorPanelPropertyCoalescing() {
    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 1u);
    panel.selectMaterial(0u);

    fuse::editor::CommandStack cmds;
    expectTrue(panel.setRoughness(0.4f, cmds), "first roughness drag accepted");
    expectTrue(panel.setRoughness(0.5f, cmds), "second roughness drag accepted");
    expectTrue(panel.setRoughness(0.6f, cmds), "third roughness drag accepted");

    expectTrue(cmds.appliedCount() == 3u, "each drag posts to pending queue");
    expectTrue(cmds.undoDepth() == 1u, "roughness drags coalesce to one undo step");
    expectTrue(cmds.coalescedCount() == 2u, "two roughness drags coalesced");

    expectTrue(panel.editState().roughness == 0.6f, "coalesced edit state keeps latest roughness");
}

#ifdef FUSE_VULKAN_BACKEND
void testMaterialEditorPanelMaterialSystemBridge() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for material panel test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready");

    fuse::renderer::MaterialSystem materials;
    materials.init(resources);

    fuse::renderer::Material source{};
    source.baseColor = {0.2f, 0.4f, 0.6f};
    source.roughness = 0.35f;
    source.metallic = 0.8f;
    source.shadingModel = fuse::renderer::ShadingModel::Emissive;
    const fuse::u32 materialId = materials.registerMaterial(source);
    expectTrue(materialId == 0u, "material registered for panel bridge test");

    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.syncFromMaterialSystem(state, materials);

    expectTrue(panel.selectedMaterialId() == 0u, "sync selects first material");
    expectTrue(!panel.previewDirty(), "sync clears preview dirty");
    expectTrue(!panel.editDirty(), "sync clears edit dirty");
    expectTrue(panel.editState().roughness == 0.35f, "sync loads roughness");
    expectTrue(panel.editState().metallic == 0.8f, "sync loads metallic");
    expectTrue(panel.editState().shadingModel == 2u, "sync loads shading model");

    fuse::editor::CommandStack cmds;
    expectTrue(panel.setRoughness(0.1f, cmds), "panel roughness edit accepted");
    expectTrue(panel.pushToMaterialSystem(materials, cmds), "push writes edit state to material system");
    expectTrue(!panel.editDirty(), "push clears edit dirty");
    expectTrue(materials.get(0u).roughness == 0.1f, "material system reflects pushed roughness");
    expectTrue(materials.dirtyCount() == 1u, "push marks material row dirty");

    materials.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}
#endif

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
    testMaterialEditorPanelPropertyBindings();
    testMaterialEditorPanelPropertyCoalescing();
#ifdef FUSE_VULKAN_BACKEND
    testMaterialEditorPanelMaterialSystemBridge();
#endif
    testSdfSculptPanelStrokeSpacing();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_panels_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_panels_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
