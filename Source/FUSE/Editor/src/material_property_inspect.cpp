#include <fuse/editor/material_property_inspect.hpp>

#include <algorithm>

namespace fuse::editor {

namespace {

constexpr u8 kMaxShadingModel = 5u; // Cloth — matches renderer::ShadingModel::Cloth

f32 clampUnit_(f32 value) {
    return std::clamp(value, 0.f, 1.f);
}

} // namespace

u32 materialPropertyCount() {
    return 4u;
}

MaterialPropertyDescriptor materialPropertyDescriptor(MaterialPropertyId id) {
    switch (id) {
    case MaterialPropertyId::Roughness:
        return {id, "Roughness", 0.f, 1.f};
    case MaterialPropertyId::Metallic:
        return {id, "Metallic", 0.f, 1.f};
    case MaterialPropertyId::BaseColor:
        return {id, "Base Color", 0.f, 1.f};
    case MaterialPropertyId::ShadingModel:
        return {id, "Shading Model", 0.f, static_cast<f32>(kMaxShadingModel)};
    }
    return {id, "Unknown", 0.f, 1.f};
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

} // namespace fuse::editor
