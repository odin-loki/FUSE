#pragma once

// Asset plan W0.7 x WP-1.5: mip chains of the layered-material textures and the CPU twin of sampling them.
//
// The WP-1.5 material resolve evaluates layered materials (its layered bin) with the textures as mip-mapped bindless
// sampled images and textureGrad (analytic screen-space derivatives), not with the texel pool's manual bilinear filter
// of materials.eval / materials.balls. MaterialLayers uploads exactly the chain ml_build_mips produces (RGBA8, sRGB
// textures as R8G8B8A8_SRGB), and the CPU reference samples the same chain with ml_trilinear, the Vulkan "texel
// filtering" of a REPEAT / LINEAR / mipmapMode LINEAR sampler without anisotropy:
//
//   rho    = max(|(du/dx, dv/dx)| * (W, H), |(du/dy, dv/dy)| * (W, H))   (isotropic footprint, level-0 texels)
//   lambda = log2(rho) + lodBias
//   lambda <= 0: magnification, bilinear of level 0 (the pool filter's exact arithmetic: ml_bilinear)
//   else:        lambda clamped to [0, levels - 1], d = floor(lambda), bilinear(d) and bilinear(d + 1) blended by
//                frac(lambda)
//
// Bilinear taps decode through the library's LUT (sRGB decode before filtering, like an _SRGB image view), so at
// magnification the CPU filter equals the texel-pool filter bit for bit. A GPU differs from this by its LOD
// approximation (Lavapipe: rho / log2 approximations) and its filter-weight precision; the resolve gate
// (fuse_rp_material_resolve_layered_vk_*) bounds both, see the "Asset W0.7" row of RENDERER-EXECUTION.md.
//
// Mip generation: 2 x 2 box filter (the odd last row / column of a level is clamped), level sizes max(1, n / 2),
// down to 1 x 1; RGB of sRGB textures is averaged in linear light (LUT decode, IEC 61966-2-1 encode in f64, round to
// nearest), linear channels and alpha are averaged as integers with round-half-up. Deterministic on every platform.

#include <fuse/renderer/material_layers/ml_kernel.hpp>
#include <fuse/renderer/material_layers/ml_types.hpp>

#include <vector>

namespace fuse::renderer::material_layers {

class MlLibrary;

/// One level of one texture's chain.
struct MlMipLevel {
    u32 offset = 0; ///< first texel (u32 RGBA8, x fastest) in MlMipChains::texels
    u32 width = 0;
    u32 height = 0;
};

/// Every texture of a library with its full mip chain; the levels of a texture are contiguous and tightly packed
/// (mip-major), which is the layout UploadQueue::stageImage takes.
struct MlMipChains {
    std::vector<u32> texels;
    std::vector<MlMipLevel> levels;
    std::vector<u32> firstLevel; ///< per texture: index of its level 0 in `levels`
    std::vector<u32> levelCount; ///< per texture
    const f32* lut = nullptr;    ///< the library's decode LUT (kMlLutEntries)

    u32 textureCount() const { return static_cast<u32>(firstLevel.size()); }
    /// Texels of all levels of texture `t` (from its level 0).
    u64 textureTexels(u32 t) const;
};

/// Number of levels of a full chain for a w x h image (floor(log2(max(w, h))) + 1).
u32 ml_mip_level_count(u32 width, u32 height);

/// Builds the chains of every library texture (level 0 = the library's texels).
void ml_build_mips(const MlLibrary& library, MlMipChains& out);

/// Trilinear sample of texture `texture` (see the header comment). `flags` = MlTexture::flags (sRGB decode).
MlF4 ml_trilinear(const MlMipChains& chains, u32 texture, u32 flags, MlF2 uv, MlF2 dx, MlF2 dy, f32 lodBias = 0.f);

/// The LOD ml_trilinear selects before clamping (log2 of the isotropic footprint + bias); -128 for a zero footprint.
f32 ml_trilinear_lod(const MlMipChains& chains, u32 texture, MlF2 dx, MlF2 dy, f32 lodBias = 0.f);

/// MlView::filter adaptor: MlView::filterUser points at one of these.
struct MlTrilinearFilter {
    const MlMipChains* chains = nullptr;
    const MlTexture* textures = nullptr; ///< the library's (flags)
    u32 textureCount = 0;
    f32 lodBias = 0.f;
};
MlF4 ml_trilinear_filter(const void* user, u32 texture, MlF2 uv, MlF2 dx, MlF2 dy);

/// The library's view with the trilinear filter installed (`filter` must outlive the view).
MlView ml_trilinear_view(const MlLibrary& library, const MlTrilinearFilter& filter);

} // namespace fuse::renderer::material_layers
