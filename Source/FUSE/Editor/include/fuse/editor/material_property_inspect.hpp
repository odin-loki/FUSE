#pragma once

#include <fuse/editor/material_property_binding.hpp>
#include <fuse/types.hpp>

namespace fuse::editor {

struct MaterialEditState;

/// UI/inspector metadata for one material property field (B6.7 deepen).
struct MaterialPropertyDescriptor {
    MaterialPropertyId id = MaterialPropertyId::Roughness;
    const char* label = "";
    f32 minValue = 0.f;
    f32 maxValue = 1.f;
    bool isVec3 = false;
};

/// Sentinel returned by `materialPropertyIndexOf` when the id is not enumerated.
inline constexpr u32 kInvalidMaterialPropertyIndex = UINT32_MAX;

/// Catalog slot sentinel — matches `MaterialPropertyBinding::kInvalidMaterialId`.
inline constexpr u32 kInvalidMaterialSlot = UINT32_MAX;

/// Number of bindable material inspector properties.
[[nodiscard]] u32 materialPropertyCount();

/// True when the catalog has no materials (B6.7 deepen — empty-material early-out).
[[nodiscard]] bool isMaterialCatalogEmpty(u32 catalogCount);

/// True when `materialId` indexes a slot in `[0, catalogCount)` (B6.7 deepen).
[[nodiscard]] bool isMaterialSlotValid(u32 materialId, u32 catalogCount);

/// Convenience inverse of `isMaterialSlotValid` (B6.7 deepen).
[[nodiscard]] bool isInvalidMaterialSlot(u32 materialId, u32 catalogCount);

/// True when `materialId` is the catalog sentinel (`kInvalidMaterialSlot`).
[[nodiscard]] bool isSentinelMaterialSlot(u32 materialId);

/// True when `id` maps to a bindable inspector property (B6.7 deepen).
[[nodiscard]] bool isMaterialPropertyIdValid(MaterialPropertyId id);

/// Convenience inverse of `isMaterialPropertyIdValid` (B6.7 deepen).
[[nodiscard]] bool isInvalidMaterialPropertyId(MaterialPropertyId id);

/// Binding guard — slot must be valid before inspector bind (B6.7 deepen).
[[nodiscard]] bool canBindMaterialSlot(u32 materialId, u32 catalogCount);

/// True when `index` is safe for `materialPropertyIdAt` / descriptor lookup.
[[nodiscard]] bool isMaterialPropertyIndexValid(u32 index);

/// Stable property order for inspector widget iteration (B6.7 deepen follow-up).
[[nodiscard]] MaterialPropertyId materialPropertyIdAt(u32 index);

/// Reverse lookup — inspector row index for a property id, or `kInvalidMaterialPropertyIndex`.
[[nodiscard]] u32 materialPropertyIndexOf(MaterialPropertyId id);

/// Metadata for slider/field widgets (B6.7 deepen).
[[nodiscard]] MaterialPropertyDescriptor materialPropertyDescriptor(MaterialPropertyId id);

/// Descriptor lookup by stable inspector index — returns default descriptor when out of range.
[[nodiscard]] MaterialPropertyDescriptor materialPropertyDescriptorAt(u32 index);

/// True when the property stores a vec3 (base color) rather than a scalar.
[[nodiscard]] bool materialPropertyIsVec3(MaterialPropertyId id);

/// Clamp helpers — authoring edits stay in valid PBR ranges (B6.7 deepen).
[[nodiscard]] f32 clampRoughness(f32 value);
[[nodiscard]] f32 clampMetallic(f32 value);
[[nodiscard]] f32 clampBaseColorComponent(f32 value);
[[nodiscard]] u8 clampShadingModel(u8 value);

/// Clamp a scalar inspector edit for the given property id (B6.7 deepen follow-up).
[[nodiscard]] f32 clampMaterialPropertyScalar(MaterialPropertyId id, f32 value);

/// Clamp all fields in an authoring-side material edit state (B6.7 deepen follow-up).
void clampMaterialEditState(MaterialEditState& state);

/// Refresh guard — binding must be live before external edit-state pull (B6.7 deepen follow-up).
[[nodiscard]] bool canRefreshMaterialBinding(const MaterialPropertyBinding& binding);

/// True when panel refresh can be skipped for this binding (B6.7 deepen follow-up).
[[nodiscard]] bool shouldSkipMaterialPanelRefresh(const MaterialPropertyBinding& binding);

/// True when a property row still needs inspector repaint (B6.7 deepen follow-up).
[[nodiscard]] bool isMaterialPropertyRefreshPending(const MaterialPropertyBinding& binding,
                                                    MaterialPropertyId id);

} // namespace fuse::editor
