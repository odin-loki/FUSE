#pragma once

// WP-1.5 material resolve: records shared by the C++ side and the shaders
// (shaders/material_resolve/*.{glsl,comp,vert,frag,slang}). Keep them in sync; the static_asserts pin
// the layouts. Device-safe (only <fuse/types.hpp>).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::material_resolve {

/// Screen tiles of the classification (one 8 x 8 compute workgroup per tile).
inline constexpr u32 kTileSize = 8u;

/// Resolve bins (mr_common.glsl FUSE_MR_BIN_*). A pixel's bin is the feature class of its material;
/// the classes are nested (Flat ⊂ Textured ⊂ NormalMapped ⊂ Layered), so a tile goes into the bin of the
/// most demanding pixel in it and that bin's pipeline shades every pixel of the tile exactly as the uber
/// pipeline would. Empty tiles (no geometry) get their own bin, so every pixel is written once by
/// either path and no clear pass is needed.
///
/// Layered (asset W0.7): material rows with gpu_scene::kGpuMaterialLayered are evaluated as layered
/// materials (material_layers::ml_evaluate over the MlResolveTable of ResolveFrameDesc::layered: triplanar /
/// stochastic tiling / detail maps / height-blended layers, world position + normal from the visibility
/// buffer, bindless textures with textureGrad). Its id is above kBinUber so the ids 0..4 and the bin-buffer
/// layout of the four feature bins stay what they were; tiles still take the maximum pixel bin (a pixel is
/// never kBinUber), and its args + tile list are appended after the four lists (ResolveBinLayout).
enum ResolveBin : u32 {
    kBinEmpty = 0u,        ///< no geometry (or a bad id): clear values
    kBinFlat = 1u,         ///< material without textures (or no material: the default surface)
    kBinTextured = 2u,     ///< base colour / roughness / metallic / AO / emissive textures, no normal map
    kBinNormalMapped = 3u, ///< normal map (tangent frame + extra fetch)
    kBinCount = 4u,        ///< feature bins in the ResolveBinLayout header (kBinLayered is appended)
    kBinUber = 4u,         ///< specialisation of the single-pass fallback: every feature, per-pixel branches
    kBinLayered = 5u,      ///< layered material rows (+ every feature of kBinNormalMapped for the other pixels)
    kBinDrawCount = 5u,    ///< indirect draws of the binned path (kBinCount feature bins + kBinLayered)
};

/// Resolve kernels' feature bits (derived from the bin specialisation constant).
enum ResolveFeature : u32 {
    kFeatureTextures = 1u << 0,
    kFeatureNormalMap = 1u << 1,
    kFeatureAll = kFeatureTextures | kFeatureNormalMap,
    kFeatureLayered = 1u << 2, ///< layered-material evaluation (resolve_features)
};

/// Feature bits of the non-layered material evaluation a pipeline specialised for `bin` runs.
FUSE_HOST_DEVICE constexpr u32 bin_features(u32 bin) {
    return bin >= kBinNormalMapped ? kFeatureAll : (bin == kBinTextured ? kFeatureTextures : 0u);
}

/// Every feature bit a resolve pipeline specialised for `bin` evaluates: bin_features, plus the layered
/// evaluation for the layered bin and the uber path (mr_common fuse_mr_resolve_features).
FUSE_HOST_DEVICE constexpr u32 resolve_features(u32 bin) {
    return bin >= kBinUber ? (kFeatureAll | kFeatureLayered) : bin_features(bin);
}

/// Per-frame constants, read through BDA from a host-visible ring (MaterialResolve::beginFrame):
/// 176 bytes, std430 (FuseMrFrame in mr_common.glsl, MrFrame in mr_common.slang).
struct ResolveFrameConstants {
    f32 viewProj[16] = {};     ///< column-major, Vulkan clip space, forward depth (visbuffer conventions)
    f32 prevViewProj[16] = {}; ///< last frame's (velocity)
    u32 width = 0;
    u32 height = 0;
    u32 tilesX = 0;
    u32 tilesY = 0;
    u32 scene = 0;        ///< GpuScene::headerHandle()
    u32 vis = 0;          ///< bindless storage-image handle of the R32G32_UINT visibility image
    u32 sampler = 0;      ///< bindless sampler handle for every material texture
    u32 tileCapacity = 0; ///< tile-list entries per bin (= tilesX * tilesY)
    u64 bins = 0;         ///< BDA of the bin buffer (ResolveBinLayout; tile lists read by the vertex stage)
    u32 binsHandle = 0;   ///< bindless storage-buffer handle of the bin buffer (classify atomics)
    u32 layered = 0;      ///< bindless storage-buffer handle of the layered-material table (MlResolveTable), 0 = none
};
static_assert(sizeof(ResolveFrameConstants) == 176u && offsetof(ResolveFrameConstants, width) == 128u &&
                  offsetof(ResolveFrameConstants, scene) == 144u && offsetof(ResolveFrameConstants, bins) == 160u,
              "ResolveFrameConstants layout (mr_common.glsl / .slang)");

/// Push constants of every WP-1.5 pipeline (compute, vertex, fragment): 16 bytes.
struct ResolvePush {
    u64 frame = 0; ///< BDA of this frame's ResolveFrameConstants
    u64 out = 0;   ///< attribute dump: BDA of ResolveAttributeTexel[width * height]; otherwise 0
};
static_assert(sizeof(ResolvePush) == 16u, "ResolvePush layout");

/// Byte layout of the bin buffer: kBinCount VkDrawIndirectCommand {6, tiles, 0, 0} (the classify
/// kernel counts tiles into instanceCount), then the draw-count block {1, 0, 0, 0} (the count buffer of
/// every bin's vkCmdDrawIndirectCount, maxDrawCount 1), then kBinCount tile lists of `tileCapacity`
/// u32 entries (x | y << 16), bin b at listOffset + b * tileCapacity * 4. "resolve.reset" writes the
/// first kResetBytes every frame.
///
/// Why a draw count at all: a plain vkCmdDrawIndirect is not self-contained on Lavapipe (Mesa
/// lvp_execute.c handle_draw_indirect sets offset / stride / draw_count / buffer of the command
/// buffer's pipe_draw_indirect_info but never clears indirect_draw_count, which an earlier
/// vkCmdDraw(Indexed)IndirectCount of the same command buffer set, e.g. the visibility buffer's
/// culled draws). The draw then runs min(1, that stale count), and once the culler's last count is 0
/// (a steady frame whose phase 2 draws nothing) no tile is drawn and the G-buffer keeps its old
/// contents. vkCmdDrawIndirectCount sets every field (drawIndirectCount is already a visibility-buffer
/// requirement), and on every other driver it is the same single draw.
struct ResolveBinLayout {
    static constexpr u32 kArgsStride = 16u;
    static constexpr u32 kArgsBytes = kArgsStride * kBinCount;
    static constexpr u32 kDrawCountOffset = kArgsBytes; ///< u32 1 (16-byte block)
    static constexpr u32 kDrawCountBytes = 16u;
    static constexpr u32 kResetBytes = kArgsBytes + kDrawCountBytes;
    static constexpr u32 kListOffset = kResetBytes;
    static constexpr u32 kVerticesPerTile = 6u; ///< two triangles per tile quad
    static constexpr u64 bytes(u32 tileCapacity) { return kListOffset + static_cast<u64>(tileCapacity) * 4u * kBinCount; }
    /// The layered bin, appended after the kBinCount lists (the layout above is unchanged): its
    /// VkDrawIndirectCommand {6, tiles, 0, 0} ("resolve.reset" writes it too), then its tile list.
    static constexpr u64 layeredArgsOffset(u32 tileCapacity) { return bytes(tileCapacity); }
    static constexpr u64 layeredListOffset(u32 tileCapacity) { return bytes(tileCapacity) + kArgsStride; }
    /// Size of the bin buffer MaterialResolve allocates.
    static constexpr u64 totalBytes(u32 tileCapacity) {
        return layeredListOffset(tileCapacity) + static_cast<u64>(tileCapacity) * 4u;
    }
    /// Byte offsets of bin `bin`'s args and tile list (any bin, kBinLayered included).
    static constexpr u64 argsOffset(u32 bin, u32 tileCapacity) {
        return bin == kBinLayered ? layeredArgsOffset(tileCapacity) : static_cast<u64>(bin) * kArgsStride;
    }
    static constexpr u64 listOffset(u32 bin, u32 tileCapacity) {
        return bin == kBinLayered ? layeredListOffset(tileCapacity) : kListOffset + static_cast<u64>(bin) * tileCapacity * 4u;
    }
};

FUSE_HOST_DEVICE constexpr u32 pack_tile(u32 x, u32 y) { return x | (y << 16); }
FUSE_HOST_DEVICE constexpr u32 tile_x(u32 packed) { return packed & 0xFFFFu; }
FUSE_HOST_DEVICE constexpr u32 tile_y(u32 packed) { return packed >> 16; }

/// ResolveAttributeTexel::flags (the visbuffer decode flags, vis_types.hpp VisDecodeFlag).
enum ResolveAttributeFlag : u32 {
    kAttrEmpty = 0u,
    kAttrOk = 1u,
    kAttrBadId = 2u,
    kAttrDegenerate = 3u,
};

/// Per-pixel output of the attribute dump ("resolve.attributes") and of the CPU reference kernel
/// "material_resolve_attributes": what the resolve reconstructs before it evaluates the material.
/// 112 bytes, std430.
struct ResolveAttributeTexel {
    f32 b1 = 0.f;       ///< perspective-correct barycentrics of vertices 1 and 2 (b0 = 1 - b1 - b2 up to rounding)
    f32 b2 = 0.f;
    f32 depth = 1.f;    ///< z / w at the pixel centre
    u32 flags = kAttrEmpty;
    f32 db1dx = 0.f;    ///< screen-space derivatives (per pixel) of b1 / b2, analytic
    f32 db1dy = 0.f;
    f32 db2dx = 0.f;
    f32 db2dy = 0.f;
    f32 uv[2] = {0.f, 0.f};    ///< interpolated UV0
    f32 duvdx[2] = {0.f, 0.f}; ///< d(uv)/dx per pixel (texture LOD / anisotropy)
    f32 duvdy[2] = {0.f, 0.f};
    u32 material = 0xFFFFFFFFu; ///< material row (GpuInstance::material + submesh material index), ~0 = none
    u32 bin = kBinEmpty;        ///< ResolveBin of the pixel
    f32 normal[3] = {0.f, 0.f, 0.f}; ///< interpolated world normal (not renormalised)
    f32 tangentSign = 1.f;           ///< bitangent sign (vertex 0's, times the transform's determinant sign)
    f32 tangent[3] = {0.f, 0.f, 0.f}; ///< interpolated world tangent (not renormalised)
    f32 pad0 = 0.f;
    f32 velocity[2] = {0.f, 0.f}; ///< pixels, current - previous position (0 when the previous w <= 0)
    f32 pad1[2] = {0.f, 0.f};
};
static_assert(sizeof(ResolveAttributeTexel) == 112u && offsetof(ResolveAttributeTexel, uv) == 32u &&
                  offsetof(ResolveAttributeTexel, material) == 56u && offsetof(ResolveAttributeTexel, normal) == 64u &&
                  offsetof(ResolveAttributeTexel, velocity) == 96u,
              "ResolveAttributeTexel layout (mr_attributes.comp / .slang)");

/// Material id attachment (R32_UINT) value of an empty pixel.
inline constexpr u32 kNoMaterial = 0xFFFFFFFFu;
/// VkFormat of the material id attachment (VK_FORMAT_R32_UINT).
inline constexpr u32 kMaterialIdFormat = 98u;
/// Colour attachments of the resolve: G-buffer RT0..RT5 (GBufferAttachment order) + material id.
inline constexpr u32 kResolveColorAttachments = 7u;
/// Forward reference: the same 7 + the (instance, triangle) ids (R32G32_UINT), + a D32 depth.
inline constexpr u32 kForwardColorAttachments = 8u;

} // namespace fuse::renderer::material_resolve
