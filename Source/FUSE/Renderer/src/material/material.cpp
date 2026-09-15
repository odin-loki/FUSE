#include <fuse/renderer/material/material.hpp>

namespace fuse::renderer {
namespace {

u32 textureBindlessIndex(const TextureHandle& handle) {
    return handle.isValid() ? handle.index() : UINT32_MAX;
}

} // namespace

Material::GPUMaterial Material::pack() const {
    GPUMaterial gpu{};
    gpu.baseColor = {baseColor.x, baseColor.y, baseColor.z, metallic};
    gpu.roughnessEmissive = {roughness, emissiveColor.x, emissiveColor.y, emissiveColor.z};
    gpu.baseColorTexIdx = textureBindlessIndex(baseColorTex);
    gpu.roughnessTexIdx = textureBindlessIndex(roughnessTex);
    gpu.normalTexIdx = textureBindlessIndex(normalTex);
    gpu.aoTexIdx = textureBindlessIndex(aoTex);
    gpu.emissiveTexIdx = textureBindlessIndex(emissiveTex);
    gpu.emissiveIntensity = emissiveIntensity;
    gpu.shadingModel = static_cast<u32>(shadingModel);

    u32 flags = 0u;
    if (isProcedural) {
        flags |= 1u << 0;
    }
    if (proceduralFnId != 0u) {
        flags |= proceduralFnId << 1;
    }
    gpu.flags = flags;
    return gpu;
}

} // namespace fuse::renderer
