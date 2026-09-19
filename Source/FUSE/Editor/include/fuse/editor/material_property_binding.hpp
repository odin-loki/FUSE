#pragma once

#include <fuse/editor/command_stack.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::editor {

struct MaterialEditState;
struct MaterialInspectorRefreshInfo;

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
    /// Catalog-validated bind — unbinds and returns false when the slot is invalid (B6.7 deepen).
    bool tryBind(u32 materialId, u32 catalogCount, MaterialEditState& editState);
    void unbind();

    [[nodiscard]] bool isBound() const { return m_materialId != kInvalidMaterialId; }
    [[nodiscard]] u32 boundMaterialId() const { return m_materialId; }

    /// Binding guard — bound with a live edit-state pointer (B6.7 deepen).
    [[nodiscard]] bool canPostProperty() const { return isBound() && m_editState != nullptr; }

    /// True when any inspector property row is marked dirty (B6.7 deepen follow-up).
    [[nodiscard]] bool hasAnyPropertyDirty() const { return m_dirtyMask != 0u; }

    /// Refresh guard — bound with a live edit-state pointer (B6.7 deepen follow-up).
    [[nodiscard]] bool canRefreshFromEditState() const { return canPostProperty(); }

    /// Scalar try-get guard — bound and property id is a scalar field (B6.7 deepen follow-up).
    [[nodiscard]] bool canTryGetPropertyScalar(MaterialPropertyId id) const;

    /// Scalar try-set guard — bound and property id is a scalar field (B6.7 deepen follow-up).
    [[nodiscard]] bool canTrySetPropertyScalar(MaterialPropertyId id) const;

    /// Vec3 try-get guard — bound and property id is base color (B6.7 deepen follow-up).
    [[nodiscard]] bool canTryGetPropertyVec3(MaterialPropertyId id) const;

    /// Vec3 try-set guard — bound and property id is base color (B6.7 deepen follow-up).
    [[nodiscard]] bool canTrySetPropertyVec3(MaterialPropertyId id) const;

    /// Snapshot of refresh/dirty state for inspector wiring (B6.7 deepen follow-up).
    [[nodiscard]] MaterialInspectorRefreshInfo refreshInfo() const;

    bool getRoughness(f32& out) const;
    bool getMetallic(f32& out) const;
    bool getBaseColor(f32& r, f32& g, f32& b) const;
    bool getShadingModel(u8& out) const;

    bool setRoughness(f32 roughness, CommandStack& cmds);
    bool setMetallic(f32 metallic, CommandStack& cmds);
    bool setBaseColor(f32 r, f32 g, f32 b, CommandStack& cmds);
    bool setShadingModel(u8 shadingModel, CommandStack& cmds);

    /// Generic get/set by property id for inspector round-trip (B6.7 deepen).
    bool getProperty(MaterialPropertyId id, f32& out) const;
    bool setProperty(MaterialPropertyId id, f32 value, CommandStack& cmds);
    bool getPropertyVec3(MaterialPropertyId id, f32& x, f32& y, f32& z) const;
    bool setPropertyVec3(MaterialPropertyId id, f32 x, f32 y, f32 z, CommandStack& cmds);

    /// Guarded get/set — early-out when unbound or property id invalid (B6.7 deepen).
    bool tryGetProperty(MaterialPropertyId id, f32& out) const;
    bool trySetProperty(MaterialPropertyId id, f32 value, CommandStack& cmds);
    bool tryGetPropertyVec3(MaterialPropertyId id, f32& x, f32& y, f32& z) const;
    bool trySetPropertyVec3(MaterialPropertyId id, f32 x, f32 y, f32 z, CommandStack& cmds);

    [[nodiscard]] bool isPropertyDirty(MaterialPropertyId id) const;
    /// Guarded dirty lookup — rejects invalid property ids (B6.7 deepen).
    [[nodiscard]] bool tryIsPropertyDirty(MaterialPropertyId id) const;
    /// Number of distinct dirty property bits currently set (B6.7 deepen).
    [[nodiscard]] u32 dirtyPropertyCount() const;
    [[nodiscard]] bool needsPanelRefresh() const { return m_panelRefreshPending; }
    [[nodiscard]] u32 dirtyPropertyMask() const { return m_dirtyMask; }
    [[nodiscard]] u32 coalescedDirtyCount() const { return m_coalescedDirtyCount; }

    /// Panel-refresh guard — dirty mask or pending refresh flag set (B6.7 deepen).
    [[nodiscard]] bool canMarkPanelRefreshed() const {
        return needsPanelRefresh() || hasAnyPropertyDirty();
    }
    /// Dirty-clear guard — property id valid and currently dirty (B6.7 deepen).
    [[nodiscard]] bool canClearPropertyDirty(MaterialPropertyId id) const;

    void clearPropertyDirty(MaterialPropertyId id);
    /// Guarded dirty clear — rejects invalid or clean property ids (B6.7 deepen).
    bool tryClearPropertyDirty(MaterialPropertyId id);
    void clearAllPropertyDirty();
    void markPanelRefreshed();
    /// Guarded panel refresh — no-op when unbound (B6.7 deepen).
    bool tryMarkPanelRefreshed();

    void refreshFromEditState(const MaterialEditState& state);
    /// Guarded edit-state refresh — early-out when unbound (B6.7 deepen).
    bool tryRefreshFromEditState(const MaterialEditState& state);

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
