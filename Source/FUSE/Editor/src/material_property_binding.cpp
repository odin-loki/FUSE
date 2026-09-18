#include <fuse/editor/material_property_binding.hpp>

#include <fuse/editor/material_editor_panel.hpp>
#include <fuse/editor/material_property_inspect.hpp>
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

bool MaterialPropertyBinding::tryBind(u32 materialId, u32 catalogCount, MaterialEditState& editState) {
    if (!canBindMaterialSlot(materialId, catalogCount)) {
        unbind();
        return false;
    }
    bind(materialId, editState);
    return true;
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
    return setProperty(MaterialPropertyId::Roughness, roughness, cmds);
}

bool MaterialPropertyBinding::setMetallic(f32 metallic, CommandStack& cmds) {
    return setProperty(MaterialPropertyId::Metallic, metallic, cmds);
}

bool MaterialPropertyBinding::setBaseColor(f32 r, f32 g, f32 b, CommandStack& cmds) {
    return setPropertyVec3(MaterialPropertyId::BaseColor, r, g, b, cmds);
}

bool MaterialPropertyBinding::setShadingModel(u8 shadingModel, CommandStack& cmds) {
    return setProperty(MaterialPropertyId::ShadingModel, static_cast<f32>(shadingModel), cmds);
}

bool MaterialPropertyBinding::getProperty(MaterialPropertyId id, f32& out) const {
    if (!isBound() || m_editState == nullptr) {
        return false;
    }

    switch (id) {
    case MaterialPropertyId::Roughness:
        out = m_editState->roughness;
        return true;
    case MaterialPropertyId::Metallic:
        out = m_editState->metallic;
        return true;
    case MaterialPropertyId::ShadingModel:
        out = static_cast<f32>(m_editState->shadingModel);
        return true;
    case MaterialPropertyId::BaseColor:
        return false;
    }
    return false;
}

bool MaterialPropertyBinding::setProperty(MaterialPropertyId id, f32 value, CommandStack& cmds) {
    if (!isBound() || m_editState == nullptr) {
        return false;
    }

    switch (id) {
    case MaterialPropertyId::Roughness: {
        const f32 clamped = clampRoughness(value);
        m_editState->roughness = clamped;
        return postProperty_(id, std::to_string(clamped), cmds);
    }
    case MaterialPropertyId::Metallic: {
        const f32 clamped = clampMetallic(value);
        m_editState->metallic = clamped;
        return postProperty_(id, std::to_string(clamped), cmds);
    }
    case MaterialPropertyId::ShadingModel: {
        const u8 clamped = clampShadingModel(static_cast<u8>(value));
        m_editState->shadingModel = clamped;
        return postProperty_(id, std::to_string(clamped), cmds);
    }
    case MaterialPropertyId::BaseColor:
        return false;
    }
    return false;
}

bool MaterialPropertyBinding::getPropertyVec3(MaterialPropertyId id, f32& x, f32& y, f32& z) const {
    if (!isBound() || m_editState == nullptr || id != MaterialPropertyId::BaseColor) {
        return false;
    }

    x = m_editState->baseColorR;
    y = m_editState->baseColorG;
    z = m_editState->baseColorB;
    return true;
}

bool MaterialPropertyBinding::setPropertyVec3(MaterialPropertyId id, f32 x, f32 y, f32 z,
                                              CommandStack& cmds) {
    if (!isBound() || m_editState == nullptr || id != MaterialPropertyId::BaseColor) {
        return false;
    }

    const f32 r = clampBaseColorComponent(x);
    const f32 g = clampBaseColorComponent(y);
    const f32 b = clampBaseColorComponent(z);
    m_editState->baseColorR = r;
    m_editState->baseColorG = g;
    m_editState->baseColorB = b;

    const std::string value = std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b);
    return postProperty_(id, value, cmds);
}

bool MaterialPropertyBinding::tryGetProperty(MaterialPropertyId id, f32& out) const {
    if (!canTryGetPropertyScalar(id)) {
        return false;
    }
    return getProperty(id, out);
}

bool MaterialPropertyBinding::trySetProperty(MaterialPropertyId id, f32 value, CommandStack& cmds) {
    if (!canTrySetPropertyScalar(id)) {
        return false;
    }
    return setProperty(id, value, cmds);
}

bool MaterialPropertyBinding::tryGetPropertyVec3(MaterialPropertyId id, f32& x, f32& y, f32& z) const {
    if (!canTryGetPropertyVec3(id)) {
        return false;
    }
    return getPropertyVec3(id, x, y, z);
}

bool MaterialPropertyBinding::trySetPropertyVec3(MaterialPropertyId id, f32 x, f32 y, f32 z,
                                               CommandStack& cmds) {
    if (!canTrySetPropertyVec3(id)) {
        return false;
    }
    return setPropertyVec3(id, x, y, z, cmds);
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
    if (!canRefreshFromEditState()) {
        return;
    }

    *m_editState = state;
    clampMaterialEditState(*m_editState);
    markPanelRefreshed();
}

bool MaterialPropertyBinding::canTryGetPropertyScalar(MaterialPropertyId id) const {
    return canPostProperty() && canTryMaterialPropertyScalar(id);
}

bool MaterialPropertyBinding::canTrySetPropertyScalar(MaterialPropertyId id) const {
    return canPostProperty() && canTryMaterialPropertyScalar(id);
}

bool MaterialPropertyBinding::canTryGetPropertyVec3(MaterialPropertyId id) const {
    return canPostProperty() && canTryMaterialPropertyVec3(id);
}

bool MaterialPropertyBinding::canTrySetPropertyVec3(MaterialPropertyId id) const {
    return canPostProperty() && canTryMaterialPropertyVec3(id);
}

MaterialInspectorRefreshInfo MaterialPropertyBinding::refreshInfo() const {
    return {m_panelRefreshPending, m_dirtyMask, m_coalescedDirtyCount};
}

} // namespace fuse::editor
