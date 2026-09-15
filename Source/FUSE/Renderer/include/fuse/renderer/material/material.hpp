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

    bool isProcedural = false;
    u32 proceduralFnId = 0;

    /// GPU-packed representation — uploaded to material SSBO.
    struct GPUMaterial {
        MaterialVec4 baseColor{};
        MaterialVec4 roughnessEmissive{};
        u32 baseColorTexIdx = UINT32_MAX;
        u32 roughnessTexIdx = UINT32_MAX;
        u32 normalTexIdx = UINT32_MAX;
        u32 aoTexIdx = UINT32_MAX;
        u32 emissiveTexIdx = UINT32_MAX;
        f32 emissiveIntensity = 0.f;
        u32 shadingModel = 0;
        u32 flags = 0;
    };

    GPUMaterial pack() const;
};

} // namespace fuse::renderer
