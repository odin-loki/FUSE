#pragma once

#include <fuse/editor/material_property_binding.hpp>
#include <fuse/types.hpp>

namespace fuse::editor {

/// UI/inspector metadata for one material property field (B6.7 deepen).
struct MaterialPropertyDescriptor {
    MaterialPropertyId id = MaterialPropertyId::Roughness;
    const char* label = "";
    f32 minValue = 0.f;
    f32 maxValue = 1.f;
};

/// Number of bindable material inspector properties.
[[nodiscard]] u32 materialPropertyCount();

/// Metadata for slider/field widgets (B6.7 deepen).
[[nodiscard]] MaterialPropertyDescriptor materialPropertyDescriptor(MaterialPropertyId id);

/// Clamp helpers — authoring edits stay in valid PBR ranges (B6.7 deepen).
[[nodiscard]] f32 clampRoughness(f32 value);
[[nodiscard]] f32 clampMetallic(f32 value);
[[nodiscard]] f32 clampBaseColorComponent(f32 value);
[[nodiscard]] u8 clampShadingModel(u8 value);

} // namespace fuse::editor
