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

bool isMaterialCatalogEmpty(u32 catalogCount) {
    return catalogCount == 0u;
}

bool shouldSkipMaterialInspectorBind(u32 catalogCount) {
    return isMaterialCatalogEmpty(catalogCount);
}

bool isMaterialSlotValid(u32 materialId, u32 catalogCount) {
    if (isMaterialCatalogEmpty(catalogCount)) {
        return false;
    }
    return materialId < catalogCount;
}

bool isInvalidMaterialSlot(u32 materialId, u32 catalogCount) {
    return !isMaterialSlotValid(materialId, catalogCount);
}

bool isSentinelMaterialSlot(u32 materialId) {
    return materialId == kInvalidMaterialSlot;
}

bool isMaterialPropertyIdValid(MaterialPropertyId id) {
    switch (id) {
    case MaterialPropertyId::Roughness:
    case MaterialPropertyId::Metallic:
    case MaterialPropertyId::BaseColor:
    case MaterialPropertyId::ShadingModel:
        return true;
    }
    return false;
}

bool isInvalidMaterialPropertyId(MaterialPropertyId id) {
    return !isMaterialPropertyIdValid(id);
}

bool canBindMaterialSlot(u32 materialId, u32 catalogCount) {
    return isMaterialSlotValid(materialId, catalogCount);
}

bool canRefreshMaterialSlot(u32 materialId, u32 catalogCount) {
    return canBindMaterialSlot(materialId, catalogCount);
}

bool isMaterialPropertyIndexValid(u32 index) {
    return index < materialPropertyCount();
}

MaterialPropertyId materialPropertyIdAt(u32 index) {
    if (!isMaterialPropertyIndexValid(index)) {
        return MaterialPropertyId::Roughness;
    }
    return kMaterialPropertyOrder[index];
}

u32 materialPropertyIndexOf(MaterialPropertyId id) {
    for (u32 i = 0u; i < materialPropertyCount(); ++i) {
        if (kMaterialPropertyOrder[i] == id) {
            return i;
        }
    }
    return kInvalidMaterialPropertyIndex;
}

bool materialPropertyIsVec3(MaterialPropertyId id) {
    return id == MaterialPropertyId::BaseColor;
}

MaterialPropertyDescriptor materialPropertyDescriptorAt(u32 index) {
    if (!isMaterialPropertyIndexValid(index)) {
        return {};
    }
    return materialPropertyDescriptor(materialPropertyIdAt(index));
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

f32 clampMaterialPropertyScalar(MaterialPropertyId id, f32 value) {
    switch (id) {
    case MaterialPropertyId::Roughness:
        return clampRoughness(value);
    case MaterialPropertyId::Metallic:
        return clampMetallic(value);
    case MaterialPropertyId::BaseColor:
        return clampBaseColorComponent(value);
    case MaterialPropertyId::ShadingModel:
        return static_cast<f32>(clampShadingModel(static_cast<u8>(value)));
    }
    return clampUnit_(value);
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
