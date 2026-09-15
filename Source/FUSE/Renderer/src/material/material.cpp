#include <fuse/renderer/material/material.hpp>

#include <cstddef>

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
    gpu.metallicTexIdx = textureBindlessIndex(metallicTex);
    gpu.normalTexIdx = textureBindlessIndex(normalTex);
    gpu.aoTexIdx = textureBindlessIndex(aoTex);
    gpu.emissiveTexIdx = textureBindlessIndex(emissiveTex);
    gpu.emissiveIntensity = emissiveIntensity;
    gpu.normalStrength = normalStrength;
    gpu.shadingModel = static_cast<u32>(shadingModel);
    gpu.subsurfaceBlock = {parameters.subsurface.scatterColor.x,
                           parameters.subsurface.scatterColor.y,
                           parameters.subsurface.scatterColor.z,
                           parameters.subsurface.scatterRadius};
    gpu.clearCoatBlock = {parameters.clearCoat.clearCoat,
                          parameters.clearCoat.clearCoatRoughness,
                          parameters.cloth.sheenRoughness,
                          0.f};
    gpu.clothBlock = {parameters.cloth.sheenColor.x,
                      parameters.cloth.sheenColor.y,
                      parameters.cloth.sheenColor.z,
                      0.f};

    u32 flags = 0u;
    if (isProcedural) {
        flags |= MaterialFlagBits::kProcedural;
    }
    if (proceduralFnId != 0u) {
        flags |= proceduralFnId << 1;
    }
    if (normalTex.isValid()) {
        flags |= MaterialFlagBits::kHasNormalMap;
    }
    if (aoTex.isValid()) {
        flags |= MaterialFlagBits::kHasAoMap;
    }
    if (metallicTex.isValid()) {
        flags |= MaterialFlagBits::kHasMetallicMap;
    }
    gpu.flags = flags;
    return gpu;
}

bool MaterialLayout::validateGpuStruct() {
    static_assert(sizeof(Material::GPUMaterial) % 16u == 0u, "GPUMaterial must be 16-byte aligned for SSBO rows");
    static_assert(offsetof(Material::GPUMaterial, baseColor) == 0u, "GPUMaterial baseColor at offset 0");
    static_assert(offsetof(Material::GPUMaterial, flags) < offsetof(Material::GPUMaterial, subsurfaceBlock),
                  "core fields precede extension parameter blocks");

    const Material::GPUMaterial probe{};
    if (probe.baseColorTexIdx != UINT32_MAX || probe.metallicTexIdx != UINT32_MAX) {
        return false;
    }
    return MaterialLayout::gpuMaterialStride() >= 64u;
}

} // namespace fuse::renderer
