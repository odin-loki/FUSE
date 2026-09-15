#include <fuse/core/init.hpp>
#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/material_editor_panel.hpp>
#include <fuse/editor/material_property_binding.hpp>
#include <fuse/editor/material_property_inspect.hpp>
#include <fuse/editor/property_inspector.hpp>
#include <fuse/editor/sdf_sculpt_panel.hpp>
#include <fuse/ecs/components/mesh.hpp>
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

void testMaterialPropertyBindingGetSet() {
    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 1u);
    panel.selectMaterial(0u);

    fuse::editor::MaterialPropertyBinding& binding = panel.propertyBinding();
    expectTrue(binding.isBound(), "selection binds property table");
    expectTrue(binding.boundMaterialId() == 0u, "binding tracks material id");

    fuse::f32 roughness = 0.f;
    expectTrue(binding.getRoughness(roughness), "get roughness succeeds when bound");
    expectTrue(roughness == 0.5f, "get roughness returns edit-state default");

    fuse::editor::CommandStack cmds;
    expectTrue(binding.setMetallic(0.42f, cmds), "set metallic via binding succeeds");
    fuse::f32 metallic = 0.f;
    expectTrue(binding.getMetallic(metallic), "get metallic round-trips");
    expectTrue(metallic == 0.42f, "binding get reflects set");
    expectTrue(panel.editState().metallic == 0.42f, "panel edit state mirrors binding set");
}

void testMaterialPropertyBindingDirtyCoalesce() {
    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 1u);
    panel.selectMaterial(0u);

    fuse::editor::CommandStack cmds;
    expectTrue(panel.setRoughness(0.3f, cmds), "first roughness edit accepted");
    expectTrue(panel.needsPanelRefresh(), "panel refresh pending after property edit");
    expectTrue(panel.propertyBinding().isPropertyDirty(fuse::editor::MaterialPropertyId::Roughness),
               "roughness property marked dirty");

    expectTrue(panel.setRoughness(0.4f, cmds), "second roughness edit accepted");
    expectTrue(panel.setRoughness(0.5f, cmds), "third roughness edit accepted");
    expectTrue(panel.coalescedPropertyDirtyCount() == 2u,
               "repeat roughness edits coalesce dirty notifications");

    panel.refreshPanel();
    expectTrue(!panel.needsPanelRefresh(), "refreshPanel clears panel refresh flag");
    expectTrue(!panel.propertyBinding().isPropertyDirty(fuse::editor::MaterialPropertyId::Roughness),
               "refreshPanel clears property dirty flags");
    expectTrue(panel.coalescedPropertyDirtyCount() == 0u,
               "refreshPanel resets coalesced dirty counter");
}

void testMaterialPropertyBindingRoundtrip() {
    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 1u);
    panel.selectMaterial(0u);

    fuse::editor::MaterialPropertyBinding& binding = panel.propertyBinding();
    fuse::editor::CommandStack cmds;

    expectTrue(binding.setProperty(fuse::editor::MaterialPropertyId::Roughness, 0.25f, cmds),
               "set roughness via generic property api");
    fuse::f32 roughness = 0.f;
    expectTrue(binding.getProperty(fuse::editor::MaterialPropertyId::Roughness, roughness),
               "get roughness via generic property api");
    expectTrue(roughness == 0.25f, "roughness round-trips");

    expectTrue(binding.setProperty(fuse::editor::MaterialPropertyId::Metallic, 0.9f, cmds),
               "set metallic via generic property api");
    fuse::f32 metallic = 0.f;
    expectTrue(binding.getProperty(fuse::editor::MaterialPropertyId::Metallic, metallic),
               "get metallic via generic property api");
    expectTrue(metallic == 0.9f, "metallic round-trips");

    expectTrue(binding.setPropertyVec3(fuse::editor::MaterialPropertyId::BaseColor, 0.2f, 0.4f, 0.6f,
                                       cmds),
               "set base color via generic vec3 api");
    fuse::f32 r = 0.f;
    fuse::f32 g = 0.f;
    fuse::f32 b = 0.f;
    expectTrue(binding.getPropertyVec3(fuse::editor::MaterialPropertyId::BaseColor, r, g, b),
               "get base color via generic vec3 api");
    expectTrue(r == 0.2f && g == 0.4f && b == 0.6f, "base color round-trips");

    expectTrue(binding.setProperty(fuse::editor::MaterialPropertyId::ShadingModel, 4.f, cmds),
               "set shading model via generic property api");
    fuse::f32 shadingModel = 0.f;
    expectTrue(binding.getProperty(fuse::editor::MaterialPropertyId::ShadingModel, shadingModel),
               "get shading model via generic property api");
    expectTrue(shadingModel == 4.f, "shading model round-trips");

    expectTrue(cmds.appliedCount() == 4u, "roundtrip edits post four commands");
    expectTrue(fuse::editor::materialPropertyCount() == 4u, "four inspector properties exposed");
}

void testMaterialEditorPanelEmptyCatalog() {
    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 0u);

    expectTrue(panel.catalogCount() == 0u, "empty catalog reports zero materials");
    expectTrue(panel.selectedMaterialId() == fuse::editor::MaterialEditorPanel::kInvalidMaterialId,
               "empty catalog has no selection");
    expectTrue(!panel.propertyBinding().isBound(), "empty catalog keeps binding unbound");

    fuse::editor::CommandStack cmds;
    expectTrue(!panel.selectMaterial(0u), "select fails on empty catalog");
    expectTrue(!panel.setRoughness(0.5f, cmds), "edit fails without selection");
    expectTrue(cmds.appliedCount() == 0u, "empty catalog posts no commands");
}

void testMaterialPropertyBindingClamp() {
    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 1u);
    panel.selectMaterial(0u);

    fuse::editor::MaterialPropertyBinding& binding = panel.propertyBinding();
    fuse::editor::CommandStack cmds;

    expectTrue(binding.setRoughness(-0.5f, cmds), "negative roughness accepted");
    expectTrue(panel.editState().roughness == 0.f, "roughness clamped to zero");

    expectTrue(binding.setRoughness(1.5f, cmds), "overshoot roughness accepted");
    expectTrue(panel.editState().roughness == 1.f, "roughness clamped to one");

    expectTrue(binding.setMetallic(-1.f, cmds), "negative metallic accepted");
    expectTrue(panel.editState().metallic == 0.f, "metallic clamped to zero");

    expectTrue(binding.setBaseColor(2.f, -0.25f, 0.5f, cmds), "out-of-range base color accepted");
    expectTrue(panel.editState().baseColorR == 1.f, "base color r clamped");
    expectTrue(panel.editState().baseColorG == 0.f, "base color g clamped");
    expectTrue(panel.editState().baseColorB == 0.5f, "base color b unchanged");

    expectTrue(binding.setShadingModel(99u, cmds), "overshoot shading model accepted");
    expectTrue(panel.editState().shadingModel == 5u, "shading model clamped to cloth");

    const fuse::editor::MaterialPropertyDescriptor roughnessDesc =
        fuse::editor::materialPropertyDescriptor(fuse::editor::MaterialPropertyId::Roughness);
    expectTrue(roughnessDesc.minValue == 0.f && roughnessDesc.maxValue == 1.f,
               "roughness descriptor exposes unit range");
}

void testPropertyInspectorMeshMaterialId() {
    fuse::editor::EditorScene scene;
    scene.init();

    const fuse::ecs::EntityID entity = scene.registry().create();
    scene.registry().add<fuse::ecs::Transform>(entity);
    fuse::ecs::Mesh mesh{};
    mesh.material_id = 2u;
    scene.registry().add<fuse::ecs::Mesh>(entity, mesh);

    fuse::editor::EditorState state;
    state.primarySelection = entity;

    fuse::editor::PropertyInspector inspector;
    inspector.sync(state, scene);

    fuse::u32 materialId = 0u;
    expectTrue(inspector.getMeshMaterialId(scene, materialId), "mesh material id readable");
    expectTrue(materialId == 2u, "mesh material id matches component");

    fuse::editor::CommandStack cmds;
    expectTrue(inspector.setMeshMaterialId(5u, scene, cmds), "mesh material id edit accepted");
    expectTrue(scene.registry().get<fuse::ecs::Mesh>(entity)->material_id == 5u,
               "mesh material id updated on component");
    expectTrue(cmds.appliedCount() == 1u, "mesh material id posts command");

    const fuse::editor::EditorCommand* last = cmds.lastApplied();
    expectTrue(last != nullptr && last->propertyName == "mesh.material_id",
               "mesh material id command property name");

    scene.destroy();
}

void testMaterialPropertyBindingUnbound() {
    fuse::editor::MaterialPropertyBinding binding;
    expectTrue(!binding.isBound(), "default binding is unbound");

    fuse::f32 value = 0.f;
    expectTrue(!binding.getRoughness(value), "unbound get roughness fails");
    expectTrue(!binding.getMetallic(value), "unbound get metallic fails");

    fuse::f32 r = 0.f;
    fuse::f32 g = 0.f;
    fuse::f32 b = 0.f;
    expectTrue(!binding.getBaseColor(r, g, b), "unbound get base color fails");

    fuse::u8 shadingModel = 0u;
    expectTrue(!binding.getShadingModel(shadingModel), "unbound get shading model fails");

    fuse::editor::CommandStack cmds;
    expectTrue(!binding.setRoughness(0.1f, cmds), "unbound set roughness fails");
    expectTrue(cmds.appliedCount() == 0u, "unbound set posts no commands");

    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 1u);
    expectTrue(!panel.propertyBinding().isBound(), "panel without selection stays unbound");
    expectTrue(!panel.setRoughness(0.2f, cmds), "panel set without selection fails");
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
    testMaterialPropertyBindingGetSet();
    testMaterialPropertyBindingRoundtrip();
    testMaterialEditorPanelEmptyCatalog();
    testMaterialPropertyBindingClamp();
    testMaterialPropertyBindingDirtyCoalesce();
    testMaterialPropertyBindingUnbound();
    testPropertyInspectorMeshMaterialId();
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
