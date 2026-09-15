#pragma once

#include <fuse/math/vec.hpp>
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

/// GPU material flag bits — low bits reserved for procedural function id (see Material::pack).
namespace MaterialFlagBits {
static constexpr u32 kProcedural = 1u << 0;
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
    u32 proceduralFnId = 0;

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
        MaterialVec4 subsurfaceBlock{};
        MaterialVec4 clearCoatBlock{};
        MaterialVec4 clothBlock{};
        u32 padding[2]{}; // SSBO row alignment pad (128-byte stride)
    };

    GPUMaterial pack() const;
};

/// SSBO layout validation helpers for CPU tests.
struct MaterialLayout {
    static usize gpuMaterialStride() { return sizeof(Material::GPUMaterial); }
    static bool validateGpuStruct();
};

} // namespace fuse::renderer
