#include <fuse/editor/material_property_binding.hpp>

#include <fuse/editor/material_editor_panel.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>

namespace fuse::editor {

namespace {

Handle<Object> materialHandle(u32 materialId) {
    return Handle<Object>(materialId, 1u);
}

} // namespace

u32 MaterialPropertyBinding::propertyBit_(MaterialPropertyId id) {
    return 1u << static_cast<u32>(id);
}

const char* MaterialPropertyBinding::commandPropertyName_(MaterialPropertyId id) {
    switch (id) {
    case MaterialPropertyId::Roughness:
        return "material.roughness";
    case MaterialPropertyId::Metallic:
        return "material.metallic";
    case MaterialPropertyId::BaseColor:
        return "material.baseColor";
    case MaterialPropertyId::ShadingModel:
        return "material.shadingModel";
    }
    return "material.unknown";
}

void MaterialPropertyBinding::bind(u32 materialId, MaterialEditState& editState) {
    m_materialId = materialId;
    m_editState = &editState;
    m_dirtyMask = 0u;
    m_coalescedDirtyCount = 0u;
    m_panelRefreshPending = false;
}

void MaterialPropertyBinding::unbind() {
    m_materialId = kInvalidMaterialId;
    m_editState = nullptr;
    m_dirtyMask = 0u;
    m_coalescedDirtyCount = 0u;
    m_panelRefreshPending = false;
}

bool MaterialPropertyBinding::getRoughness(f32& out) const {
    if (!isBound() || m_editState == nullptr) {
        return false;
    }

    out = m_editState->roughness;
    return true;
}

bool MaterialPropertyBinding::getMetallic(f32& out) const {
    if (!isBound() || m_editState == nullptr) {
        return false;
    }

    out = m_editState->metallic;
    return true;
}

bool MaterialPropertyBinding::getBaseColor(f32& r, f32& g, f32& b) const {
    if (!isBound() || m_editState == nullptr) {
        return false;
    }

    r = m_editState->baseColorR;
    g = m_editState->baseColorG;
    b = m_editState->baseColorB;
    return true;
}

bool MaterialPropertyBinding::getShadingModel(u8& out) const {
    if (!isBound() || m_editState == nullptr) {
        return false;
    }

    out = m_editState->shadingModel;
    return true;
}

void MaterialPropertyBinding::markPropertyDirty_(MaterialPropertyId id) {
    const u32 bit = propertyBit_(id);
    if ((m_dirtyMask & bit) != 0u) {
        ++m_coalescedDirtyCount;
    }
    m_dirtyMask |= bit;
    m_panelRefreshPending = true;
}

bool MaterialPropertyBinding::postProperty_(MaterialPropertyId id,
                                            const std::string& propertyValue,
                                            CommandStack& cmds) {
    if (!isBound()) {
        return false;
    }

    markPropertyDirty_(id);

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = materialHandle(m_materialId);
    command.propertyName = commandPropertyName_(id);
    command.propertyValue = propertyValue;
    cmds.execute(std::move(command));
    return true;
}

bool MaterialPropertyBinding::setRoughness(f32 roughness, CommandStack& cmds) {
    if (!isBound() || m_editState == nullptr) {
        return false;
    }

    m_editState->roughness = roughness;
    return postProperty_(MaterialPropertyId::Roughness, std::to_string(roughness), cmds);
}

bool MaterialPropertyBinding::setMetallic(f32 metallic, CommandStack& cmds) {
    if (!isBound() || m_editState == nullptr) {
        return false;
    }

    m_editState->metallic = metallic;
    return postProperty_(MaterialPropertyId::Metallic, std::to_string(metallic), cmds);
}

bool MaterialPropertyBinding::setBaseColor(f32 r, f32 g, f32 b, CommandStack& cmds) {
    if (!isBound() || m_editState == nullptr) {
        return false;
    }

    m_editState->baseColorR = r;
    m_editState->baseColorG = g;
    m_editState->baseColorB = b;

    const std::string value = std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b);
    return postProperty_(MaterialPropertyId::BaseColor, value, cmds);
}

bool MaterialPropertyBinding::setShadingModel(u8 shadingModel, CommandStack& cmds) {
    if (!isBound() || m_editState == nullptr) {
        return false;
    }

    m_editState->shadingModel = shadingModel;
    return postProperty_(MaterialPropertyId::ShadingModel, std::to_string(shadingModel), cmds);
}

bool MaterialPropertyBinding::isPropertyDirty(MaterialPropertyId id) const {
    return (m_dirtyMask & propertyBit_(id)) != 0u;
}

void MaterialPropertyBinding::clearPropertyDirty(MaterialPropertyId id) {
    m_dirtyMask &= ~propertyBit_(id);
    if (m_dirtyMask == 0u) {
        m_panelRefreshPending = false;
    }
}

void MaterialPropertyBinding::clearAllPropertyDirty() {
    m_dirtyMask = 0u;
    m_panelRefreshPending = false;
}

void MaterialPropertyBinding::markPanelRefreshed() {
    clearAllPropertyDirty();
    m_coalescedDirtyCount = 0u;
}

void MaterialPropertyBinding::refreshFromEditState(const MaterialEditState& state) {
    if (!isBound() || m_editState == nullptr) {
        return;
    }

    *m_editState = state;
    markPanelRefreshed();
}

} // namespace fuse::editor
