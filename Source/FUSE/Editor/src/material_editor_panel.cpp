#include <fuse/editor/material_editor_panel.hpp>

#ifdef FUSE_VULKAN_BACKEND
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/material/material_system.hpp>
#endif

namespace fuse::editor {

void MaterialEditorPanel::sync(const EditorState& /*state*/, u32 materialCount) {
    m_catalogCount = materialCount;
    if (m_selectedMatId >= m_catalogCount) {
        m_selectedMatId = kInvalidMaterialId;
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
        m_previewDirty = false;
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
    m_previewDirty = true;
    return true;
}

bool MaterialEditorPanel::setRoughness(f32 roughness, CommandStack& cmds) {
    if (m_selectedMatId == kInvalidMaterialId) {
        return false;
    }

    m_editState.roughness = roughness;
    m_previewDirty = true;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.propertyName = "material.roughness";
    command.propertyValue = std::to_string(roughness);
    cmds.execute(std::move(command));
    return true;
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
    command.propertyName = "material";
    command.propertyValue = std::to_string(m_selectedMatId);
    cmds.execute(std::move(command));
    m_previewDirty = false;
    return true;
#else
    (void)materials;
    (void)cmds;
    return false;
#endif
}

} // namespace fuse::editor
