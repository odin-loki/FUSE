#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/material/procedural_materials.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// PBR shading model identifiers (B5.3 — P5 §5.3).
enum class ShadingModel : u8 {
    Opaque = 0,
    Translucent = 1,
    Emissive = 2,
    SubsurfaceSSS = 3,
    ClearCoat = 4,
    Cloth = 5,
};

/// GPU material flag bits — bits 1..15 carry the procedural function id (see Material::pack).
namespace MaterialFlagBits {
static constexpr u32 kProcedural = 1u << 0;
static constexpr u32 kProceduralIdShift = 1u;
static constexpr u32 kProceduralIdMask = 0x7FFFu; // ids 0..32767; never reaches bit 16
static constexpr u32 proceduralId(u32 flags) { return (flags >> kProceduralIdShift) & kProceduralIdMask; }
static constexpr u32 kHasNormalMap = 1u << 16;
static constexpr u32 kHasAoMap = 1u << 17;
static constexpr u32 kHasMetallicMap = 1u << 18;
} // namespace MaterialFlagBits

/// Authoring-side shading-model extension parameters (packed to SSBO extension rows).
struct SubsurfaceParams {
    fuse::math::Vec3 scatterColor{1.f, 0.2f, 0.1f};
    f32 scatterRadius = 1.f;
};

struct ClearCoatParams {
    f32 clearCoat = 0.f;
    f32 clearCoatRoughness = 0.03f;
};

struct ClothParams {
    fuse::math::Vec3 sheenColor{1.f, 1.f, 1.f};
    f32 sheenRoughness = 0.5f;
};

struct MaterialParameterBlock {
    SubsurfaceParams subsurface{};
    ClearCoatParams clearCoat{};
    ClothParams cloth{};
};

struct MaterialVec4 {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    f32 w = 0.f;
};

/// Authoring-side PBR material — packed to SSBO rows for bindless lookup.
struct Material {
    fuse::math::Vec3 baseColor{1.f, 1.f, 1.f};
    TextureHandle baseColorTex{};

    f32 roughness = 0.5f;
    TextureHandle roughnessTex{};
    f32 metallic = 0.f;
    TextureHandle metallicTex{};

    TextureHandle normalTex{};
    f32 normalStrength = 1.f;

    fuse::math::Vec3 emissiveColor{};
    f32 emissiveIntensity = 0.f;
    TextureHandle emissiveTex{};

    TextureHandle aoTex{};

    ShadingModel shadingModel = ShadingModel::Opaque;

    MaterialParameterBlock parameters{};

    bool isProcedural = false;
    u32 proceduralFnId = 0; // ProceduralMaterialId; 15-bit field in GPU flags
    u32 proceduralSeed = 0; // decorrelates procedural materials sharing a function id

    /// GPU-packed representation — uploaded to material SSBO.
    struct GPUMaterial {
        MaterialVec4 baseColor{};
        MaterialVec4 roughnessEmissive{};
        u32 baseColorTexIdx = UINT32_MAX;
        u32 roughnessTexIdx = UINT32_MAX;
        u32 metallicTexIdx = UINT32_MAX;
        u32 normalTexIdx = UINT32_MAX;
        u32 aoTexIdx = UINT32_MAX;
        u32 emissiveTexIdx = UINT32_MAX;
        f32 emissiveIntensity = 0.f;
        f32 normalStrength = 1.f;
        u32 shadingModel = 0;
        u32 flags = 0;
        u32 proceduralSeed = 0;
        u32 padding = 0; // std430: the vec4 extension blocks below must start on a 16-byte boundary
        MaterialVec4 subsurfaceBlock{};  // rgb = scatter colour, w = scatter radius
        MaterialVec4 clearCoatBlock{};   // x = clear coat, y = clear coat roughness, z = sheen roughness
        MaterialVec4 clothBlock{};       // rgb = sheen colour
    };

    GPUMaterial pack() const;
};

/// SSBO layout validation helpers for CPU tests. The row layout is std430-compatible and mirrors
/// `FuseGpuMaterial` in shaders/common/material.glsl (128-byte stride).
struct MaterialLayout {
    static constexpr usize kGpuMaterialStride = 128u;

    static usize gpuMaterialStride() { return sizeof(Material::GPUMaterial); }
    static bool validateGpuStruct();

    /// Bindless-style row fetch from raw SSBO bytes (`materials[index]` in the shader).
    /// Returns false when the index lies outside the buffer.
    static bool fetchRow(const void* ssboBytes, usize byteSize, u32 index, Material::GPUMaterial& out);
};

/// Shader-side material evaluation, CPU reference (mirrors `fuse_material_sample` in material.glsl).
struct MaterialEval {
    /// Emitted radiance of a surface: emissive colour x intensity (linear, W / sr / m^2).
    static fuse::math::Vec3 emissiveRadiance(const Material& material);
    static fuse::math::Vec3 emissiveRadiance(const Material::GPUMaterial& gpu);

    /// Evaluates an SSBO row at a world-space point — procedural rows run their function, flat rows
    /// return their constants. Emissive radiance is filled from the row in both cases.
    static MaterialSample sample(const Material::GPUMaterial& gpu, const fuse::math::Vec3& worldPos);

    /// Radiance leaving a surface along a GI probe ray: emission plus the Lambertian response of the
    /// diffuse albedo (albedo * (1 - metallic)) to the irradiance already cached at the hit point.
    static fuse::math::Vec3 probeRayRadiance(const MaterialSample& sample, const fuse::math::Vec3& irradiance);
};

} // namespace fuse::renderer
