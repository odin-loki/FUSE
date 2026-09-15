#pragma once

#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class MaterialSystem;
}

namespace fuse::editor {

/// Authoring-side material fields mirrored for Qt-free panel logic (B6.7).
struct MaterialEditState {
    f32 baseColorR = 1.f;
    f32 baseColorG = 1.f;
    f32 baseColorB = 1.f;
    f32 roughness = 0.5f;
    f32 metallic = 0.f;
    u8 shadingModel = 0;
};

/// Headless material editor panel stub (B6.7).
class MaterialEditorPanel {
public:
    static constexpr u32 kInvalidMaterialId = UINT32_MAX;

    void sync(const EditorState& state, u32 materialCount);
    void syncFromMaterialSystem(const EditorState& state, renderer::MaterialSystem& materials);

    [[nodiscard]] u32 selectedMaterialId() const { return m_selectedMatId; }
    [[nodiscard]] u32 catalogCount() const { return m_catalogCount; }
    [[nodiscard]] const MaterialEditState& editState() const { return m_editState; }
    [[nodiscard]] bool previewDirty() const { return m_previewDirty; }
    [[nodiscard]] bool editDirty() const { return m_editDirty; }

    bool selectMaterial(u32 materialId);
    bool setRoughness(f32 roughness, CommandStack& cmds);
    bool setMetallic(f32 metallic, CommandStack& cmds);
    bool setBaseColor(f32 r, f32 g, f32 b, CommandStack& cmds);
    bool setShadingModel(u8 shadingModel, CommandStack& cmds);
    bool pushToMaterialSystem(renderer::MaterialSystem& materials, CommandStack& cmds);

    void clearPreviewDirty() { m_previewDirty = false; }
    void clearEditDirty() { m_editDirty = false; }

private:
    bool postMaterialProperty_(const char* propertyName, const std::string& propertyValue,
                               CommandStack& cmds);

    u32 m_selectedMatId = kInvalidMaterialId;
    u32 m_catalogCount = 0;
    MaterialEditState m_editState{};
    bool m_previewDirty = true;
    bool m_editDirty = false;
};

} // namespace fuse::editor
