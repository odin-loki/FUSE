#pragma once

#include <fuse/editor/command_stack.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::editor {

struct MaterialEditState;

/// Material inspector property identifiers (B6.7 deepen).
enum class MaterialPropertyId : u8 {
    Roughness,
    Metallic,
    BaseColor,
    ShadingModel,
};

/// Per-property bind/get/set stub with dirty coalescing (B6.7 deepen).
class MaterialPropertyBinding {
public:
    static constexpr u32 kInvalidMaterialId = UINT32_MAX;

    void bind(u32 materialId, MaterialEditState& editState);
    void unbind();

    [[nodiscard]] bool isBound() const { return m_materialId != kInvalidMaterialId; }
    [[nodiscard]] u32 boundMaterialId() const { return m_materialId; }

    bool getRoughness(f32& out) const;
    bool getMetallic(f32& out) const;
    bool getBaseColor(f32& r, f32& g, f32& b) const;
    bool getShadingModel(u8& out) const;

    bool setRoughness(f32 roughness, CommandStack& cmds);
    bool setMetallic(f32 metallic, CommandStack& cmds);
    bool setBaseColor(f32 r, f32 g, f32 b, CommandStack& cmds);
    bool setShadingModel(u8 shadingModel, CommandStack& cmds);

    [[nodiscard]] bool isPropertyDirty(MaterialPropertyId id) const;
    [[nodiscard]] bool needsPanelRefresh() const { return m_panelRefreshPending; }
    [[nodiscard]] u32 coalescedDirtyCount() const { return m_coalescedDirtyCount; }

    void clearPropertyDirty(MaterialPropertyId id);
    void clearAllPropertyDirty();
    void markPanelRefreshed();

    void refreshFromEditState(const MaterialEditState& state);

private:
    static u32 propertyBit_(MaterialPropertyId id);
    static const char* commandPropertyName_(MaterialPropertyId id);

    bool postProperty_(MaterialPropertyId id, const std::string& propertyValue,
                       CommandStack& cmds);
    void markPropertyDirty_(MaterialPropertyId id);

    u32 m_materialId = kInvalidMaterialId;
    MaterialEditState* m_editState = nullptr;
    u32 m_dirtyMask = 0u;
    u32 m_coalescedDirtyCount = 0u;
    bool m_panelRefreshPending = false;
};

} // namespace fuse::editor
