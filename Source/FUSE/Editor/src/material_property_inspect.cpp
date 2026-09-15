#include <fuse/editor/material_property_inspect.hpp>

#include <fuse/editor/material_editor_panel.hpp>

#include <algorithm>

namespace fuse::editor {

namespace {

constexpr u8 kMaxShadingModel = 5u; // Cloth — matches renderer::ShadingModel::Cloth

constexpr MaterialPropertyId kMaterialPropertyOrder[] = {
    MaterialPropertyId::Roughness,
    MaterialPropertyId::Metallic,
    MaterialPropertyId::BaseColor,
    MaterialPropertyId::ShadingModel,
};

f32 clampUnit_(f32 value) {
    return std::clamp(value, 0.f, 1.f);
}

} // namespace

u32 materialPropertyCount() {
    return static_cast<u32>(sizeof(kMaterialPropertyOrder) / sizeof(kMaterialPropertyOrder[0]));
}

MaterialPropertyId materialPropertyIdAt(u32 index) {
    if (index >= materialPropertyCount()) {
        return MaterialPropertyId::Roughness;
    }
    return kMaterialPropertyOrder[index];
}

bool materialPropertyIsVec3(MaterialPropertyId id) {
    return id == MaterialPropertyId::BaseColor;
}

MaterialPropertyDescriptor materialPropertyDescriptor(MaterialPropertyId id) {
    switch (id) {
    case MaterialPropertyId::Roughness:
        return {id, "Roughness", 0.f, 1.f, false};
    case MaterialPropertyId::Metallic:
        return {id, "Metallic", 0.f, 1.f, false};
    case MaterialPropertyId::BaseColor:
        return {id, "Base Color", 0.f, 1.f, true};
    case MaterialPropertyId::ShadingModel:
        return {id, "Shading Model", 0.f, static_cast<f32>(kMaxShadingModel), false};
    }
    return {id, "Unknown", 0.f, 1.f, false};
}

f32 clampRoughness(f32 value) {
    return clampUnit_(value);
}

f32 clampMetallic(f32 value) {
    return clampUnit_(value);
}

f32 clampBaseColorComponent(f32 value) {
    return clampUnit_(value);
}

u8 clampShadingModel(u8 value) {
    return static_cast<u8>(std::min(value, kMaxShadingModel));
}

void clampMaterialEditState(MaterialEditState& state) {
    state.roughness = clampRoughness(state.roughness);
    state.metallic = clampMetallic(state.metallic);
    state.baseColorR = clampBaseColorComponent(state.baseColorR);
    state.baseColorG = clampBaseColorComponent(state.baseColorG);
    state.baseColorB = clampBaseColorComponent(state.baseColorB);
    state.shadingModel = clampShadingModel(state.shadingModel);
}

} // namespace fuse::editor
