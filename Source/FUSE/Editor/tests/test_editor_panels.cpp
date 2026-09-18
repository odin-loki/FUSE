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
    expectTrue(panel.isCatalogEmpty(), "empty catalog helper reports empty");
    expectTrue(!panel.hasSelectedMaterial(), "empty catalog has no selection");
    expectTrue(panel.selectedMaterialId() == fuse::editor::MaterialEditorPanel::kInvalidMaterialId,
               "empty catalog has no selection id");
    expectTrue(!panel.propertyBinding().isBound(), "empty catalog keeps binding unbound");
    expectTrue(!panel.propertyBinding().canPostProperty(), "empty catalog cannot post properties");

    fuse::editor::CommandStack cmds;
    expectTrue(!panel.selectMaterial(0u), "select fails on empty catalog");
    expectTrue(!panel.setRoughness(0.5f, cmds), "edit fails without selection");
    expectTrue(cmds.appliedCount() == 0u, "empty catalog posts no commands");
}

void testMaterialSlotValidationHelpers() {
    expectTrue(fuse::editor::isMaterialCatalogEmpty(0u), "zero catalog count is empty");
    expectTrue(!fuse::editor::isMaterialCatalogEmpty(3u), "non-zero catalog is not empty");

    expectTrue(fuse::editor::isMaterialSlotValid(0u, 3u), "first slot valid in three-material catalog");
    expectTrue(fuse::editor::isMaterialSlotValid(2u, 3u), "last slot valid in three-material catalog");
    expectTrue(!fuse::editor::isMaterialSlotValid(3u, 3u), "out-of-range slot rejected");
    expectTrue(!fuse::editor::isMaterialSlotValid(0u, 0u), "any slot invalid in empty catalog");

    expectTrue(fuse::editor::isInvalidMaterialSlot(99u, 2u), "far out-of-range slot is invalid");
    expectTrue(!fuse::editor::isInvalidMaterialSlot(1u, 2u), "in-range slot is not invalid");

    expectTrue(fuse::editor::canBindMaterialSlot(0u, 2u), "first slot can bind");
    expectTrue(!fuse::editor::canBindMaterialSlot(2u, 2u), "out-of-range slot cannot bind");
    expectTrue(fuse::editor::kInvalidMaterialSlot == fuse::editor::MaterialPropertyBinding::kInvalidMaterialId,
               "invalid slot sentinel matches binding sentinel");
}

void testMaterialEditorPanelInvalidSlotSelection() {
    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 2u);

    expectTrue(!panel.isCatalogEmpty(), "two-material catalog is not empty");
    expectTrue(!panel.hasSelectedMaterial(), "panel starts without selection");

    expectTrue(!panel.selectMaterial(2u), "select rejects out-of-range slot");
    expectTrue(!panel.hasSelectedMaterial(), "invalid select leaves panel unselected");
    expectTrue(!panel.propertyBinding().isBound(), "invalid select keeps binding unbound");

    fuse::editor::CommandStack cmds;
    expectTrue(!panel.setRoughness(0.5f, cmds), "edit fails after invalid select");
    expectTrue(cmds.appliedCount() == 0u, "invalid slot posts no commands");

    expectTrue(panel.selectMaterial(1u), "valid select succeeds");
    expectTrue(panel.hasSelectedMaterial(), "valid select marks material selected");
    expectTrue(panel.propertyBinding().canPostProperty(), "bound panel can post properties");
}

void testMaterialPropertyBindingTryGuards() {
    fuse::editor::MaterialPropertyBinding binding;
    fuse::f32 value = 0.f;
    fuse::f32 r = 0.f;
    fuse::f32 g = 0.f;
    fuse::f32 b = 0.f;
    fuse::editor::CommandStack cmds;

    expectTrue(!binding.canPostProperty(), "unbound binding cannot post");
    expectTrue(!binding.tryGetProperty(fuse::editor::MaterialPropertyId::Roughness, value),
               "tryGetProperty fails when unbound");
    expectTrue(!binding.trySetProperty(fuse::editor::MaterialPropertyId::Roughness, 0.5f, cmds),
               "trySetProperty fails when unbound");
    expectTrue(!binding.tryGetPropertyVec3(fuse::editor::MaterialPropertyId::BaseColor, r, g, b),
               "tryGetPropertyVec3 fails when unbound");
    expectTrue(!binding.trySetPropertyVec3(fuse::editor::MaterialPropertyId::BaseColor, 0.1f, 0.2f, 0.3f,
                                          cmds),
               "trySetPropertyVec3 fails when unbound");
    expectTrue(cmds.appliedCount() == 0u, "try guards post no commands when unbound");

    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 1u);
    panel.selectMaterial(0u);
    fuse::editor::MaterialPropertyBinding& bound = panel.propertyBinding();

    expectTrue(bound.canPostProperty(), "bound panel binding can post");
    expectTrue(bound.trySetProperty(fuse::editor::MaterialPropertyId::Roughness, 0.33f, cmds),
               "trySetProperty succeeds when bound");
    expectTrue(bound.tryGetProperty(fuse::editor::MaterialPropertyId::Roughness, value),
               "tryGetProperty succeeds when bound");
    expectTrue(value == 0.33f, "tryGetProperty returns edited value");

    expectTrue(!bound.tryGetProperty(fuse::editor::MaterialPropertyId::BaseColor, value),
               "tryGetProperty rejects vec3 property id");
    expectTrue(!bound.trySetProperty(fuse::editor::MaterialPropertyId::BaseColor, 0.5f, cmds),
               "trySetProperty rejects vec3 property id");
}

void testMaterialPropertyIdValidation() {
    expectTrue(fuse::editor::isMaterialPropertyIdValid(fuse::editor::MaterialPropertyId::Roughness),
               "roughness id is valid");
    expectTrue(fuse::editor::isMaterialPropertyIdValid(fuse::editor::MaterialPropertyId::Metallic),
               "metallic id is valid");
    expectTrue(fuse::editor::isMaterialPropertyIdValid(fuse::editor::MaterialPropertyId::BaseColor),
               "base color id is valid");
    expectTrue(fuse::editor::isMaterialPropertyIdValid(fuse::editor::MaterialPropertyId::ShadingModel),
               "shading model id is valid");
    expectTrue(!fuse::editor::isInvalidMaterialPropertyId(fuse::editor::MaterialPropertyId::Roughness),
               "valid roughness id is not invalid");
}

void testMaterialSentinelSlotHelpers() {
    expectTrue(fuse::editor::isSentinelMaterialSlot(fuse::editor::kInvalidMaterialSlot),
               "invalid slot sentinel recognized");
    expectTrue(!fuse::editor::isSentinelMaterialSlot(0u), "zero is not the sentinel");
    expectTrue(!fuse::editor::canBindMaterialSlot(fuse::editor::kInvalidMaterialSlot, 3u),
               "sentinel slot cannot bind");
}

void testMaterialPropertyDescriptorAt() {
    const fuse::editor::MaterialPropertyDescriptor roughness =
        fuse::editor::materialPropertyDescriptorAt(0u);
    expectTrue(roughness.id == fuse::editor::MaterialPropertyId::Roughness,
               "descriptor at index 0 is roughness");
    expectTrue(roughness.label != nullptr && roughness.label[0] != '\0',
               "descriptor at index 0 has label");

    const fuse::editor::MaterialPropertyDescriptor invalid =
        fuse::editor::materialPropertyDescriptorAt(99u);
    expectTrue(invalid.label != nullptr && invalid.label[0] == '\0',
               "out-of-range descriptor returns empty label");
}

void testMaterialPropertyBindingTryBind() {
    fuse::editor::MaterialEditState state{};
    fuse::editor::MaterialPropertyBinding binding;

    expectTrue(!binding.tryBind(0u, 0u, state), "tryBind rejects empty catalog");
    expectTrue(!binding.isBound(), "failed tryBind leaves binding unbound");

    expectTrue(!binding.tryBind(2u, 2u, state), "tryBind rejects out-of-range slot");
    expectTrue(!binding.isBound(), "out-of-range tryBind leaves binding unbound");

    expectTrue(binding.tryBind(1u, 2u, state), "tryBind accepts valid slot");
    expectTrue(binding.isBound(), "successful tryBind marks binding bound");
    expectTrue(binding.boundMaterialId() == 1u, "tryBind records material id");
    expectTrue(binding.canPostProperty(), "tryBind enables property posting");
}

void testMaterialEditorPanelCanSelectMaterial() {
    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 2u);

    expectTrue(panel.canSelectMaterial(0u), "first slot selectable");
    expectTrue(panel.canSelectMaterial(1u), "last slot selectable");
    expectTrue(!panel.canSelectMaterial(2u), "out-of-range slot not selectable");
    expectTrue(!panel.canSelectMaterial(fuse::editor::kInvalidMaterialSlot),
               "sentinel slot not selectable");

    panel.sync(state, 0u);
    expectTrue(!panel.canSelectMaterial(0u), "empty catalog rejects all slots");
}

void testMaterialEditorPanelCatalogShrink() {
    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 3u);
    expectTrue(panel.selectMaterial(2u), "select last slot in three-material catalog");
    expectTrue(panel.hasSelectedMaterial(), "selection active before shrink");
    expectTrue(panel.propertyBinding().isBound(), "binding active before shrink");

    panel.sync(state, 1u);
    expectTrue(panel.catalogCount() == 1u, "catalog shrinks to one material");
    expectTrue(!panel.hasSelectedMaterial(), "shrink clears out-of-range selection");
    expectTrue(panel.selectedMaterialId() == fuse::editor::MaterialEditorPanel::kInvalidMaterialId,
               "shrink resets selection id to sentinel");
    expectTrue(!panel.propertyBinding().isBound(), "shrink unbinds property binding");

    fuse::editor::CommandStack cmds;
    expectTrue(!panel.setRoughness(0.5f, cmds), "edit fails after catalog shrink");
    expectTrue(cmds.appliedCount() == 0u, "shrink posts no commands");
}

void testMaterialPropertyInspectEnumeration() {
    expectTrue(fuse::editor::materialPropertyCount() == 4u, "four bindable material properties");

    expectTrue(fuse::editor::materialPropertyIdAt(0u) == fuse::editor::MaterialPropertyId::Roughness,
               "index 0 is roughness");
    expectTrue(fuse::editor::materialPropertyIdAt(1u) == fuse::editor::MaterialPropertyId::Metallic,
               "index 1 is metallic");
    expectTrue(fuse::editor::materialPropertyIdAt(2u) == fuse::editor::MaterialPropertyId::BaseColor,
               "index 2 is base color");
    expectTrue(fuse::editor::materialPropertyIdAt(3u) == fuse::editor::MaterialPropertyId::ShadingModel,
               "index 3 is shading model");

    expectTrue(fuse::editor::isMaterialPropertyIndexValid(0u), "index 0 is valid");
    expectTrue(fuse::editor::isMaterialPropertyIndexValid(3u), "last index is valid");
    expectTrue(!fuse::editor::isMaterialPropertyIndexValid(4u), "out-of-range index rejected");
    expectTrue(!fuse::editor::isMaterialPropertyIndexValid(99u), "far out-of-range index rejected");

    expectTrue(fuse::editor::materialPropertyIndexOf(fuse::editor::MaterialPropertyId::Roughness) == 0u,
               "roughness index lookup");
    expectTrue(fuse::editor::materialPropertyIndexOf(fuse::editor::MaterialPropertyId::Metallic) == 1u,
               "metallic index lookup");
    expectTrue(fuse::editor::materialPropertyIndexOf(fuse::editor::MaterialPropertyId::BaseColor) == 2u,
               "base color index lookup");
    expectTrue(fuse::editor::materialPropertyIndexOf(fuse::editor::MaterialPropertyId::ShadingModel) == 3u,
               "shading model index lookup");

    for (fuse::u32 i = 0u; i < fuse::editor::materialPropertyCount(); ++i) {
        const fuse::editor::MaterialPropertyId id = fuse::editor::materialPropertyIdAt(i);
        expectTrue(fuse::editor::materialPropertyIndexOf(id) == i, "index/id round-trip");
        const fuse::editor::MaterialPropertyDescriptor desc =
            fuse::editor::materialPropertyDescriptor(id);
        expectTrue(desc.id == id, "descriptor id matches enumeration index");
        expectTrue(desc.label != nullptr && desc.label[0] != '\0', "descriptor label populated");
        expectTrue(desc.isVec3 == fuse::editor::materialPropertyIsVec3(id),
                   "descriptor vec3 flag matches helper");
    }

    expectTrue(fuse::editor::materialPropertyIsVec3(fuse::editor::MaterialPropertyId::BaseColor),
               "base color is vec3");
    expectTrue(!fuse::editor::materialPropertyIsVec3(fuse::editor::MaterialPropertyId::Roughness),
               "roughness is scalar");
}

void testMaterialPropertyInspectScalarClamp() {
    expectTrue(fuse::editor::clampMaterialPropertyScalar(fuse::editor::MaterialPropertyId::Roughness,
                                                           -0.5f) == 0.f,
               "scalar clamp pins roughness low");
    expectTrue(fuse::editor::clampMaterialPropertyScalar(fuse::editor::MaterialPropertyId::Roughness,
                                                           1.5f) == 1.f,
               "scalar clamp pins roughness high");
    expectTrue(fuse::editor::clampMaterialPropertyScalar(fuse::editor::MaterialPropertyId::Metallic,
                                                           2.f) == 1.f,
               "scalar clamp pins metallic");
    expectTrue(fuse::editor::clampMaterialPropertyScalar(fuse::editor::MaterialPropertyId::BaseColor,
                                                           -0.1f) == 0.f,
               "scalar clamp pins base color channel low");
    expectTrue(fuse::editor::clampMaterialPropertyScalar(fuse::editor::MaterialPropertyId::ShadingModel,
                                                           99.f) == 5.f,
               "scalar clamp pins shading model");
}

void testMaterialEditStateBulkClamp() {
    fuse::editor::MaterialEditState state{};
    state.roughness = -0.25f;
    state.metallic = 2.f;
    state.baseColorR = 1.5f;
    state.baseColorG = -0.1f;
    state.baseColorB = 0.5f;
    state.shadingModel = 99u;

    fuse::editor::clampMaterialEditState(state);

    expectTrue(state.roughness == 0.f, "bulk clamp pins roughness low");
    expectTrue(state.metallic == 1.f, "bulk clamp pins metallic high");
    expectTrue(state.baseColorR == 1.f && state.baseColorG == 0.f && state.baseColorB == 0.5f,
               "bulk clamp pins base color channels");
    expectTrue(state.shadingModel == 5u, "bulk clamp pins shading model");
}

void testMaterialPropertyBindingRefreshClamp() {
    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 1u);
    panel.selectMaterial(0u);

    fuse::editor::MaterialEditState badState{};
    badState.roughness = 3.f;
    badState.metallic = -1.f;
    badState.baseColorR = 2.f;
    badState.baseColorG = -0.5f;
    badState.baseColorB = 0.25f;
    badState.shadingModel = 8u;

    panel.propertyBinding().refreshFromEditState(badState);

    expectTrue(panel.editState().roughness == 1.f, "refresh clamps roughness");
    expectTrue(panel.editState().metallic == 0.f, "refresh clamps metallic");
    expectTrue(panel.editState().baseColorR == 1.f, "refresh clamps base color r");
    expectTrue(panel.editState().baseColorG == 0.f, "refresh clamps base color g");
    expectTrue(panel.editState().baseColorB == 0.25f, "refresh keeps in-range base color b");
    expectTrue(panel.editState().shadingModel == 5u, "refresh clamps shading model");
    expectTrue(!panel.needsPanelRefresh(), "refresh clears panel dirty flags");
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

void testPropertyInspectorMeshMaterialEmptyCatalog() {
    fuse::editor::EditorScene scene;
    scene.init();

    const fuse::ecs::EntityID entity = scene.registry().create();
    scene.registry().add<fuse::ecs::Transform>(entity);
    fuse::ecs::Mesh mesh{};
    mesh.material_id = 0u;
    scene.registry().add<fuse::ecs::Mesh>(entity, mesh);

    fuse::editor::EditorState state;
    state.primarySelection = entity;

    fuse::editor::PropertyInspector inspector;
    inspector.sync(state, scene);

    fuse::u32 materialId = 0u;
    expectTrue(!inspector.tryGetMeshMaterialId(scene, 0u, materialId),
               "tryGet rejects mesh slot when catalog empty");
    expectTrue(inspector.getMeshMaterialId(scene, materialId),
               "raw get still reads mesh slot when catalog empty");
    expectTrue(materialId == 0u, "raw get returns stored mesh slot");

    fuse::editor::CommandStack cmds;
    expectTrue(!inspector.trySetMeshMaterialId(0u, 0u, scene, cmds),
               "trySet rejects edit when catalog empty");
    expectTrue(cmds.appliedCount() == 0u, "empty catalog posts no command");

    scene.destroy();
}

void testPropertyInspectorMeshMaterialTryGet() {
    fuse::editor::EditorScene scene;
    scene.init();

    const fuse::ecs::EntityID entity = scene.registry().create();
    scene.registry().add<fuse::ecs::Transform>(entity);
    fuse::ecs::Mesh mesh{};
    mesh.material_id = 1u;
    scene.registry().add<fuse::ecs::Mesh>(entity, mesh);

    fuse::editor::EditorState state;
    state.primarySelection = entity;

    fuse::editor::PropertyInspector inspector;
    inspector.sync(state, scene);

    fuse::u32 materialId = 0u;
    expectTrue(inspector.tryGetMeshMaterialId(scene, 2u, materialId),
               "tryGet accepts in-range mesh slot");
    expectTrue(materialId == 1u, "tryGet returns mesh material id");

    expectTrue(!inspector.tryGetMeshMaterialId(scene, 1u, materialId),
               "tryGet rejects out-of-range mesh slot");

    scene.destroy();
}

void testPropertyInspectorMeshMaterialInvalidSlot() {
    fuse::editor::EditorScene scene;
    scene.init();

    const fuse::ecs::EntityID entity = scene.registry().create();
    scene.registry().add<fuse::ecs::Transform>(entity);
    fuse::ecs::Mesh mesh{};
    mesh.material_id = 0u;
    scene.registry().add<fuse::ecs::Mesh>(entity, mesh);

    fuse::editor::EditorState state;
    state.primarySelection = entity;

    fuse::editor::PropertyInspector inspector;
    inspector.sync(state, scene);

    fuse::editor::CommandStack cmds;
    expectTrue(!inspector.trySetMeshMaterialId(3u, 2u, scene, cmds),
               "invalid mesh slot rejected against catalog");
    expectTrue(scene.registry().get<fuse::ecs::Mesh>(entity)->material_id == 0u,
               "invalid slot edit leaves mesh material id unchanged");
    expectTrue(cmds.appliedCount() == 0u, "invalid slot posts no command");

    expectTrue(inspector.trySetMeshMaterialId(1u, 2u, scene, cmds),
               "valid mesh slot accepted against catalog");
    expectTrue(scene.registry().get<fuse::ecs::Mesh>(entity)->material_id == 1u,
               "valid slot updates mesh material id");
    expectTrue(cmds.appliedCount() == 1u, "valid slot posts one command");

    scene.destroy();
}

void testMaterialPropertyDirtyMaskHelpers() {
    expectTrue(fuse::editor::materialPropertyDirtyBit(fuse::editor::MaterialPropertyId::Roughness) == 1u,
               "roughness dirty bit is bit 0");
    expectTrue(fuse::editor::materialPropertyDirtyBit(fuse::editor::MaterialPropertyId::Metallic) == 2u,
               "metallic dirty bit is bit 1");
    expectTrue(fuse::editor::materialPropertyDirtyBit(fuse::editor::MaterialPropertyId::BaseColor) == 4u,
               "base color dirty bit is bit 2");
    expectTrue(fuse::editor::materialPropertyDirtyBit(fuse::editor::MaterialPropertyId::ShadingModel) == 8u,
               "shading model dirty bit is bit 3");

    const fuse::u32 roughnessAndMetallic =
        fuse::editor::materialPropertyDirtyBit(fuse::editor::MaterialPropertyId::Roughness) |
        fuse::editor::materialPropertyDirtyBit(fuse::editor::MaterialPropertyId::Metallic);
    expectTrue(fuse::editor::materialPropertyDirtyCount(roughnessAndMetallic) == 2u,
               "dirty count tracks two property bits");
    expectTrue(!fuse::editor::isMaterialPropertyDirtyMaskEmpty(roughnessAndMetallic),
               "non-zero dirty mask is not empty");
    expectTrue(fuse::editor::isMaterialPropertyDirtyMaskEmpty(0u), "zero dirty mask is empty");
}

void testMaterialPropertyBindingDirtyMaskGuards() {
    fuse::editor::MaterialPropertyBinding binding;
    expectTrue(!binding.hasAnyPropertyDirty(), "unbound binding has no dirty properties");
    expectTrue(binding.propertyDirtyMask() == 0u, "unbound dirty mask is zero");
    expectTrue(binding.dirtyPropertyMask() == 0u, "unbound dirtyPropertyMask is zero");
    expectTrue(binding.dirtyPropertyCount() == 0u, "unbound dirty count is zero");
    expectTrue(!binding.canClearPropertyDirty(fuse::editor::MaterialPropertyId::Roughness),
               "cannot clear dirty on clean binding");
    expectTrue(!binding.tryClearPropertyDirty(fuse::editor::MaterialPropertyId::Roughness),
               "tryClear rejects clean binding");
    expectTrue(!binding.canMarkPanelRefreshed(), "clean binding cannot mark panel refreshed");
    expectTrue(!binding.tryMarkPanelRefreshed(), "tryMarkPanelRefreshed rejects clean binding");

    fuse::editor::MaterialEditState state{};
    fuse::editor::CommandStack cmds;
    binding.tryBind(0u, 1u, state);
    expectTrue(binding.setRoughness(0.4f, cmds), "roughness edit marks dirty");
    expectTrue(binding.hasAnyPropertyDirty(), "edit sets dirty mask");
    expectTrue(binding.dirtyPropertyCount() == 1u, "one property dirty after first edit");
    expectTrue(binding.tryIsPropertyDirty(fuse::editor::MaterialPropertyId::Roughness),
               "tryIsPropertyDirty reports roughness dirty");
    expectTrue(binding.canClearPropertyDirty(fuse::editor::MaterialPropertyId::Roughness),
               "can clear dirty roughness");
    expectTrue(binding.tryClearPropertyDirty(fuse::editor::MaterialPropertyId::Roughness),
               "tryClear clears dirty roughness");
    expectTrue(!binding.hasAnyPropertyDirty(), "clearing last dirty bit empties mask");
    expectTrue(!binding.needsPanelRefresh(), "clearing last dirty bit clears panel refresh");

    expectTrue(binding.setMetallic(0.5f, cmds), "metallic edit marks dirty");
    expectTrue(binding.setBaseColor(0.1f, 0.2f, 0.3f, cmds), "base color edit marks dirty");
    expectTrue(binding.dirtyPropertyCount() == 2u, "two distinct properties dirty");
    expectTrue(binding.propertyDirtyMask() ==
                   (fuse::editor::materialPropertyDirtyBit(fuse::editor::MaterialPropertyId::Metallic) |
                    fuse::editor::materialPropertyDirtyBit(fuse::editor::MaterialPropertyId::BaseColor)),
               "dirty mask encodes metallic and base color bits");
    expectTrue(binding.canMarkPanelRefreshed(), "dirty binding can mark panel refreshed");
    expectTrue(binding.tryMarkPanelRefreshed(), "tryMarkPanelRefreshed drains dirty state");
    expectTrue(binding.propertyDirtyMask() == 0u, "panel refresh clears dirty mask");
}

void testMaterialPropertyBindingRefreshGuards() {
    fuse::editor::MaterialPropertyBinding binding;
    fuse::editor::MaterialEditState state{};
    state.roughness = 0.8f;

    expectTrue(!binding.canRefreshFromEditState(), "unbound binding cannot refresh");
    expectTrue(!binding.tryRefreshFromEditState(state), "tryRefresh rejects unbound binding");

    binding.tryBind(0u, 1u, state);
    expectTrue(binding.canRefreshFromEditState(), "bound binding can refresh");
    fuse::editor::MaterialEditState incoming{};
    incoming.roughness = 2.f;
    incoming.metallic = -1.f;
    expectTrue(binding.tryRefreshFromEditState(incoming), "tryRefresh accepts bound binding");
    expectTrue(state.roughness == 1.f, "tryRefresh clamps incoming roughness");
    expectTrue(state.metallic == 0.f, "tryRefresh clamps incoming metallic");
    expectTrue(!binding.needsPanelRefresh(), "refresh clears panel dirty flags");
}

void testMaterialEditorPanelRefreshGuards() {
    fuse::editor::EditorState editorState;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(editorState, 1u);
    panel.selectMaterial(0u);
    panel.clearPreviewDirty();

    expectTrue(!panel.canRefreshPanel(), "clean panel cannot refresh");
    expectTrue(!panel.tryRefreshPanel(), "tryRefreshPanel rejects clean panel");
    expectTrue(!panel.hasAnyPropertyDirty(), "clean panel has no dirty properties");
    expectTrue(panel.propertyDirtyMask() == 0u, "clean panel dirty mask is zero");

    fuse::editor::CommandStack cmds;
    expectTrue(panel.setRoughness(0.35f, cmds), "roughness edit dirties panel");
    expectTrue(panel.canRefreshPanel(), "dirty panel can refresh");
    expectTrue(panel.hasAnyPropertyDirty(), "dirty panel reports dirty mask");
    expectTrue(panel.tryRefreshPanel(), "tryRefreshPanel drains dirty panel");
    expectTrue(!panel.needsPanelRefresh(), "tryRefreshPanel clears refresh flag");
    expectTrue(!panel.previewDirty(), "tryRefreshPanel clears preview dirty");
    expectTrue(!panel.tryRefreshPanel(), "second tryRefreshPanel is a no-op");
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

    expectTrue(!binding.getProperty(fuse::editor::MaterialPropertyId::Roughness, value),
               "unbound generic get roughness fails");
    expectTrue(!binding.getProperty(fuse::editor::MaterialPropertyId::Metallic, value),
               "unbound generic get metallic fails");
    expectTrue(!binding.getProperty(fuse::editor::MaterialPropertyId::ShadingModel, value),
               "unbound generic get shading model fails");
    expectTrue(!binding.getPropertyVec3(fuse::editor::MaterialPropertyId::BaseColor, r, g, b),
               "unbound generic get base color fails");

    fuse::editor::CommandStack cmds;
    expectTrue(!binding.setRoughness(0.1f, cmds), "unbound set roughness fails");
    expectTrue(!binding.setMetallic(0.2f, cmds), "unbound set metallic fails");
    expectTrue(!binding.setBaseColor(0.3f, 0.4f, 0.5f, cmds), "unbound set base color fails");
    expectTrue(!binding.setShadingModel(1u, cmds), "unbound set shading model fails");
    expectTrue(!binding.setProperty(fuse::editor::MaterialPropertyId::Roughness, 0.1f, cmds),
               "unbound generic set roughness fails");
    expectTrue(!binding.setProperty(fuse::editor::MaterialPropertyId::Metallic, 0.2f, cmds),
               "unbound generic set metallic fails");
    expectTrue(!binding.setProperty(fuse::editor::MaterialPropertyId::ShadingModel, 1.f, cmds),
               "unbound generic set shading model fails");
    expectTrue(!binding.setPropertyVec3(fuse::editor::MaterialPropertyId::BaseColor, 0.1f, 0.2f, 0.3f,
                                        cmds),
               "unbound generic set base color fails");
    expectTrue(cmds.appliedCount() == 0u, "unbound set posts no commands");

    expectTrue(!binding.isPropertyDirty(fuse::editor::MaterialPropertyId::Roughness),
               "unbound property is not dirty");
    expectTrue(!binding.needsPanelRefresh(), "unbound binding does not request refresh");

    fuse::editor::MaterialEditState externalState{};
    externalState.roughness = 0.75f;
    binding.refreshFromEditState(externalState);
    expectTrue(!binding.isBound(), "refresh on unbound binding stays unbound");

    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 1u);
    expectTrue(!panel.propertyBinding().isBound(), "panel without selection stays unbound");
    expectTrue(!panel.setRoughness(0.2f, cmds), "panel set without selection fails");
    expectTrue(!panel.setMetallic(0.3f, cmds), "panel metallic set without selection fails");
    expectTrue(!panel.setBaseColor(0.1f, 0.2f, 0.3f, cmds), "panel base color set without selection fails");
    expectTrue(!panel.setShadingModel(2u, cmds), "panel shading model set without selection fails");
}

void testMaterialEditEarlyOutHelpers() {
    expectTrue(fuse::editor::shouldEarlyOutMaterialEdit(0u, 0u),
               "empty catalog early-outs any slot");
    expectTrue(fuse::editor::shouldEarlyOutMaterialEdit(2u, 2u),
               "out-of-range slot early-outs");
    expectTrue(!fuse::editor::shouldEarlyOutMaterialEdit(3u, 1u),
               "in-range slot does not early-out");

    expectTrue(fuse::editor::canBindMaterialProperty(fuse::editor::MaterialPropertyId::Roughness, 0u, 2u),
               "roughness binds on valid slot");
    expectTrue(!fuse::editor::canBindMaterialProperty(fuse::editor::MaterialPropertyId::Roughness, 2u, 2u),
               "roughness rejects invalid slot");
    expectTrue(fuse::editor::canTryMaterialPropertyScalar(fuse::editor::MaterialPropertyId::Metallic),
               "metallic is scalar try property");
    expectTrue(!fuse::editor::canTryMaterialPropertyScalar(fuse::editor::MaterialPropertyId::BaseColor),
               "base color is not scalar try property");
    expectTrue(fuse::editor::canTryMaterialPropertyVec3(fuse::editor::MaterialPropertyId::BaseColor),
               "base color is vec3 try property");
    expectTrue(!fuse::editor::canTryMaterialPropertyVec3(fuse::editor::MaterialPropertyId::Roughness),
               "roughness is not vec3 try property");
}

void testMaterialInspectorRefreshHelpers() {
    fuse::editor::MaterialInspectorRefreshInfo idle{};
    expectTrue(!fuse::editor::shouldRefreshMaterialInspector(idle),
               "idle refresh info does not request redraw");

    fuse::editor::MaterialInspectorRefreshInfo pending{.pending = true};
    expectTrue(fuse::editor::shouldRefreshMaterialInspector(pending),
               "pending flag requests redraw");

    fuse::editor::MaterialInspectorRefreshInfo dirty{.dirtyMask = 1u};
    expectTrue(fuse::editor::shouldRefreshMaterialInspector(dirty),
               "dirty mask requests redraw");

    expectTrue(fuse::editor::shouldSkipMaterialInspectorRefresh(false),
               "unbound binding skips refresh");
    expectTrue(!fuse::editor::shouldSkipMaterialInspectorRefresh(true),
               "bound binding does not skip refresh");

    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 1u);
    expectTrue(panel.shouldRefreshPanel(), "sync marks preview dirty");

    panel.selectMaterial(0u);
    expectTrue(panel.canApplyPanelRefresh(), "selected panel can apply refresh");
    expectTrue(!panel.shouldSkipPropertyEdit(), "selected panel does not skip edits");

    fuse::editor::CommandStack cmds;
    expectTrue(panel.setRoughness(0.25f, cmds), "edit accepted on selected panel");
    expectTrue(panel.shouldRefreshPanel(), "property edit requests panel refresh");

    const fuse::editor::MaterialInspectorRefreshInfo info = panel.propertyBinding().refreshInfo();
    expectTrue(fuse::editor::shouldRefreshMaterialInspector(info),
               "binding refresh info reports pending redraw");
    expectTrue(info.coalescedDirtyCount == 0u, "first dirty edit has zero coalesced count");

    panel.refreshPanel();
    expectTrue(!panel.shouldRefreshPanel(), "refreshPanel clears refresh request");
    expectTrue(!panel.propertyBinding().hasAnyPropertyDirty(),
               "refreshPanel clears dirty property mask");
}

void testMaterialPropertyBindingScalarVec3Guards() {
    fuse::editor::MaterialPropertyBinding binding;
    expectTrue(!binding.canTryGetPropertyScalar(fuse::editor::MaterialPropertyId::Roughness),
               "unbound scalar get guard fails");
    expectTrue(!binding.canTrySetPropertyScalar(fuse::editor::MaterialPropertyId::Roughness),
               "unbound scalar set guard fails");
    expectTrue(!binding.canTryGetPropertyVec3(fuse::editor::MaterialPropertyId::BaseColor),
               "unbound vec3 get guard fails");
    expectTrue(!binding.canTrySetPropertyVec3(fuse::editor::MaterialPropertyId::BaseColor),
               "unbound vec3 set guard fails");
    expectTrue(!binding.canRefreshFromEditState(), "unbound refresh guard fails");

    fuse::editor::MaterialEditState state{};
    expectTrue(binding.tryBind(0u, 1u, state), "binding attaches to valid slot");
    expectTrue(binding.canTryGetPropertyScalar(fuse::editor::MaterialPropertyId::Roughness),
               "bound scalar get guard passes");
    expectTrue(binding.canTrySetPropertyScalar(fuse::editor::MaterialPropertyId::Metallic),
               "bound scalar set guard passes");
    expectTrue(!binding.canTryGetPropertyScalar(fuse::editor::MaterialPropertyId::BaseColor),
               "base color rejected by scalar get guard");
    expectTrue(!binding.canTrySetPropertyScalar(fuse::editor::MaterialPropertyId::BaseColor),
               "base color rejected by scalar set guard");
    expectTrue(binding.canTryGetPropertyVec3(fuse::editor::MaterialPropertyId::BaseColor),
               "bound vec3 get guard passes");
    expectTrue(binding.canTrySetPropertyVec3(fuse::editor::MaterialPropertyId::BaseColor),
               "bound vec3 set guard passes");
    expectTrue(!binding.canTryGetPropertyVec3(fuse::editor::MaterialPropertyId::Roughness),
               "roughness rejected by vec3 get guard");
    expectTrue(binding.canRefreshFromEditState(), "bound refresh guard passes");
}

void testMaterialEditorPanelSkipPropertyEdit() {
    fuse::editor::EditorState state;
    fuse::editor::MaterialEditorPanel panel;
    panel.sync(state, 0u);

    expectTrue(panel.shouldSkipPropertyEdit(), "empty catalog skips property edits");
    expectTrue(!panel.canApplyPanelRefresh(), "empty catalog cannot apply refresh");

    panel.sync(state, 2u);
    expectTrue(panel.shouldSkipPropertyEdit(), "unselected panel skips property edits");

    fuse::editor::CommandStack cmds;
    expectTrue(!panel.setRoughness(0.5f, cmds), "edit blocked when shouldSkipPropertyEdit");
    expectTrue(cmds.appliedCount() == 0u, "skipped edit posts no commands");
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
    testMaterialPropertyInspectEnumeration();
    testMaterialPropertyInspectScalarClamp();
    testMaterialEditStateBulkClamp();
    testMaterialPropertyBindingRefreshClamp();
    testMaterialEditorPanelEmptyCatalog();
    testMaterialSlotValidationHelpers();
    testMaterialEditorPanelInvalidSlotSelection();
    testMaterialPropertyBindingTryGuards();
    testMaterialPropertyIdValidation();
    testMaterialSentinelSlotHelpers();
    testMaterialPropertyDescriptorAt();
    testMaterialPropertyBindingTryBind();
    testMaterialEditorPanelCanSelectMaterial();
    testMaterialEditorPanelCatalogShrink();
    testMaterialPropertyBindingClamp();
    testMaterialPropertyBindingDirtyCoalesce();
    testMaterialPropertyDirtyMaskHelpers();
    testMaterialPropertyBindingDirtyMaskGuards();
    testMaterialPropertyBindingRefreshGuards();
    testMaterialEditorPanelRefreshGuards();
    testMaterialPropertyBindingUnbound();
    testMaterialEditEarlyOutHelpers();
    testMaterialInspectorRefreshHelpers();
    testMaterialPropertyBindingScalarVec3Guards();
    testMaterialEditorPanelSkipPropertyEdit();
    testPropertyInspectorMeshMaterialId();
    testPropertyInspectorMeshMaterialEmptyCatalog();
    testPropertyInspectorMeshMaterialTryGet();
    testPropertyInspectorMeshMaterialInvalidSlot();
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
