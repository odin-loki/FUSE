#include <fuse/renderer/material/material.hpp>

#include <fuse/renderer/material/brdf.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <type_traits>

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
    gpu.proceduralSeed = proceduralSeed;
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
    // Clamp the id into its field so it can never corrupt the texture-presence bits (16+).
    flags |= (proceduralFnId & MaterialFlagBits::kProceduralIdMask) << MaterialFlagBits::kProceduralIdShift;
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
    // std430 offsets of FuseGpuMaterial (shaders/common/material.glsl).
    static_assert(offsetof(Material::GPUMaterial, roughnessEmissive) == 16u, "roughnessEmissive offset");
    static_assert(offsetof(Material::GPUMaterial, baseColorTexIdx) == 32u, "texture index block offset");
    static_assert(offsetof(Material::GPUMaterial, emissiveIntensity) == 56u, "emissiveIntensity offset");
    static_assert(offsetof(Material::GPUMaterial, flags) == 68u, "flags offset");
    static_assert(offsetof(Material::GPUMaterial, subsurfaceBlock) % 16u == 0u, "vec4 block 16-byte aligned");
    static_assert(offsetof(Material::GPUMaterial, subsurfaceBlock) == 80u, "subsurfaceBlock offset");
    static_assert(offsetof(Material::GPUMaterial, clearCoatBlock) == 96u, "clearCoatBlock offset");
    static_assert(offsetof(Material::GPUMaterial, clothBlock) == 112u, "clothBlock offset");
    static_assert(sizeof(Material::GPUMaterial) == MaterialLayout::kGpuMaterialStride, "128-byte row stride");
    static_assert(std::is_trivially_copyable_v<Material::GPUMaterial>, "GPUMaterial is memcpy-able");
    static_assert(MaterialFlagBits::kProceduralIdMask << MaterialFlagBits::kProceduralIdShift <
                      MaterialFlagBits::kHasNormalMap,
                  "procedural id field below texture flag bits");

    const Material::GPUMaterial probe{};
    if (probe.baseColorTexIdx != UINT32_MAX || probe.metallicTexIdx != UINT32_MAX) {
        return false;
    }
    return MaterialLayout::gpuMaterialStride() >= 64u;
}

bool MaterialLayout::fetchRow(const void* ssboBytes, usize byteSize, u32 index, Material::GPUMaterial& out) {
    if (ssboBytes == nullptr) {
        return false;
    }
    const usize offset = static_cast<usize>(index) * kGpuMaterialStride;
    if (offset + kGpuMaterialStride > byteSize) {
        return false;
    }
    std::memcpy(&out, static_cast<const u8*>(ssboBytes) + offset, sizeof(out));
    return true;
}

fuse::math::Vec3 MaterialEval::emissiveRadiance(const Material& material) {
    const f32 k = std::max(material.emissiveIntensity, 0.f);
    return {std::max(material.emissiveColor.x, 0.f) * k,
            std::max(material.emissiveColor.y, 0.f) * k,
            std::max(material.emissiveColor.z, 0.f) * k};
}

fuse::math::Vec3 MaterialEval::emissiveRadiance(const Material::GPUMaterial& gpu) {
    const f32 k = std::max(gpu.emissiveIntensity, 0.f);
    return {std::max(gpu.roughnessEmissive.y, 0.f) * k,
            std::max(gpu.roughnessEmissive.z, 0.f) * k,
            std::max(gpu.roughnessEmissive.w, 0.f) * k};
}

MaterialSample MaterialEval::sample(const Material::GPUMaterial& gpu, const fuse::math::Vec3& worldPos) {
    MaterialSample s{};
    if ((gpu.flags & MaterialFlagBits::kProcedural) != 0u) {
        const u32 id = MaterialFlagBits::proceduralId(gpu.flags);
        s = ProceduralMaterials::evaluate(static_cast<ProceduralMaterialId>(id), worldPos, gpu.proceduralSeed);
    } else {
        s.albedo = {gpu.baseColor.x, gpu.baseColor.y, gpu.baseColor.z};
        s.roughness = gpu.roughnessEmissive.x;
        s.metallic = gpu.baseColor.w;
    }
    s.emissive = emissiveRadiance(gpu);
    return s;
}

fuse::math::Vec3 MaterialEval::probeRayRadiance(const MaterialSample& sample, const fuse::math::Vec3& irradiance) {
    const f32 kd = std::clamp(1.f - sample.metallic, 0.f, 1.f) / Brdf::kPi;
    return {sample.emissive.x + sample.albedo.x * kd * irradiance.x,
            sample.emissive.y + sample.albedo.y * kd * irradiance.y,
            sample.emissive.z + sample.albedo.z * kd * irradiance.z};
}

} // namespace fuse::renderer
