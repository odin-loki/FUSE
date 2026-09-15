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
    m_previewDirty = true;
    return true;
}

bool MaterialEditorPanel::postMaterialProperty_(const char* propertyName,
                                                const std::string& propertyValue,
                                                CommandStack& cmds) {
    if (m_selectedMatId == kInvalidMaterialId) {
        return false;
    }

    m_previewDirty = true;
    m_editDirty = true;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = materialHandle(m_selectedMatId);
    command.propertyName = propertyName;
    command.propertyValue = propertyValue;
    cmds.execute(std::move(command));
    return true;
}

bool MaterialEditorPanel::setRoughness(f32 roughness, CommandStack& cmds) {
    if (m_selectedMatId == kInvalidMaterialId) {
        return false;
    }

    m_editState.roughness = roughness;
    return postMaterialProperty_("material.roughness", std::to_string(roughness), cmds);
}

bool MaterialEditorPanel::setMetallic(f32 metallic, CommandStack& cmds) {
    if (m_selectedMatId == kInvalidMaterialId) {
        return false;
    }

    m_editState.metallic = metallic;
    return postMaterialProperty_("material.metallic", std::to_string(metallic), cmds);
}

bool MaterialEditorPanel::setBaseColor(f32 r, f32 g, f32 b, CommandStack& cmds) {
    if (m_selectedMatId == kInvalidMaterialId) {
        return false;
    }

    m_editState.baseColorR = r;
    m_editState.baseColorG = g;
    m_editState.baseColorB = b;

    const std::string value = std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b);
    return postMaterialProperty_("material.baseColor", value, cmds);
}

bool MaterialEditorPanel::setShadingModel(u8 shadingModel, CommandStack& cmds) {
    if (m_selectedMatId == kInvalidMaterialId) {
        return false;
    }

    m_editState.shadingModel = shadingModel;
    return postMaterialProperty_("material.shadingModel", std::to_string(shadingModel), cmds);
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
    m_previewDirty = false;
    m_editDirty = false;
    return true;
#else
    (void)materials;
    (void)cmds;
    return false;
#endif
}

} // namespace fuse::editor
