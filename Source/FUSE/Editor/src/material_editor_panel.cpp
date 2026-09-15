#include <fuse/editor/material_editor_panel.hpp>

#include <fuse/handle.hpp>
#include <fuse/object.hpp>

#ifdef FUSE_VULKAN_BACKEND
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/material/material_system.hpp>
#endif

#include <string>

namespace fuse::editor {

namespace {

Handle<Object> materialHandle(u32 materialId) {
    return Handle<Object>(materialId, 1u);
}

} // namespace

void MaterialEditorPanel::bindSelectedMaterial_() {
    if (m_selectedMatId != kInvalidMaterialId) {
        m_binding.bind(m_selectedMatId, m_editState);
    }
}

void MaterialEditorPanel::unbindSelectedMaterial_() {
    m_binding.unbind();
}

void MaterialEditorPanel::markEditDirty_() {
    m_previewDirty = true;
    m_editDirty = true;
}

void MaterialEditorPanel::sync(const EditorState& /*state*/, u32 materialCount) {
    m_catalogCount = materialCount;
    if (m_selectedMatId >= m_catalogCount) {
        m_selectedMatId = kInvalidMaterialId;
        unbindSelectedMaterial_();
    }
    m_previewDirty = true;
}

void MaterialEditorPanel::syncFromMaterialSystem(const EditorState& state,
                                                 renderer::MaterialSystem& materials) {
#ifdef FUSE_VULKAN_BACKEND
    sync(state, materials.materialCount());
    if (m_selectedMatId == kInvalidMaterialId && m_catalogCount > 0u) {
        selectMaterial(0u);
    }

    if (m_selectedMatId != kInvalidMaterialId && m_selectedMatId < materials.materialCount()) {
        const renderer::Material& material = materials.get(m_selectedMatId);
        m_editState.baseColorR = material.baseColor.x;
        m_editState.baseColorG = material.baseColor.y;
        m_editState.baseColorB = material.baseColor.z;
        m_editState.roughness = material.roughness;
        m_editState.metallic = material.metallic;
        m_editState.shadingModel = static_cast<u8>(material.shadingModel);
        m_binding.refreshFromEditState(m_editState);
        m_previewDirty = false;
        m_editDirty = false;
    }
#else
    (void)materials;
    sync(state, 0u);
#endif
}

bool MaterialEditorPanel::selectMaterial(u32 materialId) {
    if (materialId >= m_catalogCount) {
        return false;
    }

    m_selectedMatId = materialId;
    bindSelectedMaterial_();
    m_previewDirty = true;
    return true;
}

void MaterialEditorPanel::refreshPanel() {
    m_binding.markPanelRefreshed();
    m_previewDirty = false;
}

bool MaterialEditorPanel::setRoughness(f32 roughness, CommandStack& cmds) {
    if (!m_binding.isBound()) {
        return false;
    }

    markEditDirty_();
    return m_binding.setRoughness(roughness, cmds);
}

bool MaterialEditorPanel::setMetallic(f32 metallic, CommandStack& cmds) {
    if (!m_binding.isBound()) {
        return false;
    }

    markEditDirty_();
    return m_binding.setMetallic(metallic, cmds);
}

bool MaterialEditorPanel::setBaseColor(f32 r, f32 g, f32 b, CommandStack& cmds) {
    if (!m_binding.isBound()) {
        return false;
    }

    markEditDirty_();
    return m_binding.setBaseColor(r, g, b, cmds);
}

bool MaterialEditorPanel::setShadingModel(u8 shadingModel, CommandStack& cmds) {
    if (!m_binding.isBound()) {
        return false;
    }

    markEditDirty_();
    return m_binding.setShadingModel(shadingModel, cmds);
}

bool MaterialEditorPanel::pushToMaterialSystem(renderer::MaterialSystem& materials,
                                              CommandStack& cmds) {
#ifdef FUSE_VULKAN_BACKEND
    if (m_selectedMatId == kInvalidMaterialId || !materials.isReady()) {
        return false;
    }

    if (m_selectedMatId >= materials.materialCount()) {
        return false;
    }

    renderer::Material material = materials.get(m_selectedMatId);
    material.baseColor.x = m_editState.baseColorR;
    material.baseColor.y = m_editState.baseColorG;
    material.baseColor.z = m_editState.baseColorB;
    material.roughness = m_editState.roughness;
    material.metallic = m_editState.metallic;
    material.shadingModel = static_cast<renderer::ShadingModel>(m_editState.shadingModel);
    materials.updateMaterial(m_selectedMatId, material);

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = materialHandle(m_selectedMatId);
    command.propertyName = "material";
    command.propertyValue = std::to_string(m_selectedMatId);
    cmds.execute(std::move(command));
    refreshPanel();
    m_editDirty = false;
    return true;
#else
    (void)materials;
    (void)cmds;
    return false;
#endif
}

} // namespace fuse::editor
