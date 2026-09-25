#pragma once

// Asset plan W0.7 (docs/plans/FUSE_ASSET_PLAN.md §1.5, §5.1): layered materials. The records shared by the C++
// host code, the CPU reference (ml_kernel.hpp) and the compute kernels (shaders/material_layers/ml_common.{glsl,
// slang} declare the same structs with the same fields in the same order; fuse_rp_material_layers_layout checks
// names, order and offsets). Vulkan-free: builds in the stub backend.
//
// Every field is a 4-byte scalar (or an array of them) or a u64 address, so std430 (GLSL buffer_reference) and
// Slang's pointer layout agree with the C++ layout without padding rules.
//
// Textures live in one texel pool (RGBA8 packed in a u32, x fastest) reached through a buffer device address and
// are filtered with the same manual wrap-around bilinear filter on the CPU and the GPU; unorm / sRGB decode goes
// through a 512-entry LUT (the same f32 values on both sides), so the CPU reference and the kernels only differ by
// the rounding of the arithmetic (and the transcendentals). The layered bin of the WP-1.5 material resolve samples
// the same textures as mip-mapped bindless images with textureGrad instead (MlResolveTable; CPU twin: the
// trilinear filter of ml_mips.hpp over the same mip chain).
//
// Texture-set convention (one set = two textures):
//   albedo set texture  RGB = albedo (sRGB or linear, MlTexture::flags), A = height (linear, 0.5 = reference plane)
//   normal set texture  RG = tangent-space normal xy (unorm, OpenGL +Y convention), B = roughness, A = ambient occl.
// A missing texture (kMlNoTexture) reads as albedo (1, 1, 1), height 0.5, normal (0, 0, 1), roughness 1, AO 1.

#include <fuse/types.hpp>

namespace fuse::renderer::material_layers {

inline constexpr u32 kMlNoTexture = 0xFFFFFFFFu;
inline constexpr u32 kMlMaxLayers = 3u;
/// Entries of the decode LUT: [0, 256) sRGB -> linear, [256, 512) unorm (v / 255).
inline constexpr u32 kMlLutEntries = 512u;
/// Workgroup sizes: eval 64 x 1, balls 8 x 8.
inline constexpr u32 kMlEvalGroup = 64u;
inline constexpr u32 kMlTile = 8u;
/// Most balls the golden scene kernel intersects.
inline constexpr u32 kMlMaxBalls = 32u;

/// MlMaterial::flags
enum MlFlag : u32 {
    kMlFlagTriplanar = 1u << 0,  ///< world-space triplanar projection (P * uvScale) instead of UV0
    kMlFlagStochastic = 1u << 1, ///< histogram-preserving stochastic tiling of every texture set
    kMlFlagDetail = 1u << 2,     ///< detail albedo / normal at detailScale x, faded with the view distance
    kMlFlagMacro = 1u << 3,      ///< world-space macro variation of the base albedo
};

/// MlLayer::mask: where a layer's blend mask comes from (then x coverage).
enum MlMask : u32 {
    kMlMaskConstant = 0u,    ///< 1
    kMlMaskVertexR = 1u,     ///< vertex colour channels (§1.5: R layer blend, G AO/cavity, B wetness/snow, A seed)
    kMlMaskVertexG = 2u,
    kMlMaskVertexB = 3u,
    kMlMaskVertexA = 4u,
    kMlMaskSlopeUp = 5u,     ///< saturate((N.y - maskBias) * maskScale), geometric normal, +Y up
    kMlMaskWorldHeight = 6u, ///< saturate((P.y - maskBias) * maskScale)
    kMlMaskCount = 7u,
};

/// MlLayer::mode
enum MlLayerMode : u32 {
    kMlLayerHeight = 0u, ///< height-blended material layer (moss, dust, sand, snow): its own texture set
    kMlLayerWet = 1u,    ///< wetness: darkens (albedo = factor), lowers roughness, flattens the normal
    kMlLayerModeCount = 2u,
};

/// MlTexture::flags
enum MlTexFlag : u32 {
    kMlTexSrgb = 1u << 0, ///< RGB through the sRGB half of the LUT (alpha always linear)
};

/// One texture of the pool: 32 bytes.
struct MlTexture {
    u32 offset = 0; ///< first texel (u32 RGBA8) in the pool
    u32 width = 0;
    u32 height = 0;
    u32 flags = 0;     ///< MlTexFlag
    f32 mean[4] = {};  ///< mean of the decoded texels (histogram-preserving blend)
};
static_assert(sizeof(MlTexture) == 32u, "MlTexture layout (ml_common.glsl / .slang)");

/// One layer of the stack: 64 bytes.
struct MlLayer {
    f32 albedo[3] = {1.f, 1.f, 1.f}; ///< factor (height mode) / wet darkening factor per channel (wet mode)
    f32 roughness = 1.f;             ///< factor (height mode) / wet roughness target (wet mode)
    f32 metallic = 0.f;
    f32 uvScale = 1.f;   ///< multiplies the material's projection coordinate (UV0 or P)
    f32 contrast = 8.f;  ///< height-blend sharpness (>= 1)
    f32 coverage = 1.f;  ///< mask multiplier
    u32 albedoTex = kMlNoTexture;
    u32 normalTex = kMlNoTexture;
    u32 mask = kMlMaskConstant; ///< MlMask
    u32 mode = kMlLayerHeight;  ///< MlLayerMode
    f32 maskBias = 0.f;
    f32 maskScale = 1.f;
    f32 normalStrength = 1.f;
    u32 reserved = 0;
};
static_assert(sizeof(MlLayer) == 64u, "MlLayer layout (ml_common.glsl / .slang)");

/// A resolved material (a cooked .fusemat with texture indices): 288 bytes.
struct MlMaterial {
    f32 albedo[3] = {1.f, 1.f, 1.f}; ///< linear factor
    f32 roughness = 1.f;
    f32 metallic = 0.f;
    f32 uvScale = 1.f;
    f32 normalStrength = 1.f;
    u32 flags = 0; ///< MlFlag
    u32 albedoTex = kMlNoTexture;
    u32 normalTex = kMlNoTexture;
    u32 layerCount = 0;
    u32 shadingModel = 0; ///< FuseMatShading (carried through, the G-buffer shading model)
    u32 detailAlbedoTex = kMlNoTexture;
    u32 detailNormalTex = kMlNoTexture;
    f32 detailScale = 8.f;
    f32 detailStrength = 1.f;
    f32 detailFadeStart = 5.f;
    f32 detailFadeEnd = 30.f;
    f32 triplanarSharpness = 4.f;
    f32 stochasticLattice = 2.f; ///< triangle-grid vertices per texture repeat
    f32 macroScale = 0.25f;      ///< cycles per metre
    f32 macroStrength = 0.f;
    u32 procedural = 0;          ///< procedural function id (procedural_materials.hpp), 0 = none
    u32 category = 0;            ///< FuseMatCategory
    MlLayer layers[kMlMaxLayers] = {};
};
static_assert(sizeof(MlMaterial) == 288u, "MlMaterial layout (ml_common.glsl / .slang)");

/// Input of one evaluation (a surface point): 80 bytes.
struct MlSurface {
    f32 position[3] = {};   ///< world, metres
    f32 viewDistance = 0.f; ///< detail fade
    f32 normal[3] = {0.f, 1.f, 0.f}; ///< geometric / interpolated (renormalised by the kernel)
    f32 tangentSign = 1.f;
    f32 tangent[3] = {1.f, 0.f, 0.f};
    u32 material = 0;
    f32 uv[2] = {};
    f32 reserved[2] = {};
    f32 color[4] = {1.f, 1.f, 1.f, 1.f}; ///< vertex colour masks
};
static_assert(sizeof(MlSurface) == 80u, "MlSurface layout");

/// Output of one evaluation: 48 bytes.
struct MlResult {
    f32 albedo[3] = {};
    f32 roughness = 0.f;
    f32 normal[3] = {};
    f32 metallic = 0.f;
    f32 ao = 0.f;
    f32 height = 0.f;
    f32 reserved[2] = {};
};
static_assert(sizeof(MlResult) == 48u, "MlResult layout");

/// One ball of the golden material-ball scene: 32 bytes.
struct MlBall {
    f32 center[3] = {};
    f32 radius = 1.f;
    u32 material = 0;
    u32 reserved[3] = {};
};
static_assert(sizeof(MlBall) == 32u, "MlBall layout");

/// Per-frame record (host-visible ring, read through BDA): 224 bytes.
struct MlParams {
    u64 materials = 0; ///< MlMaterial[materialCount]
    u64 textures = 0;  ///< MlTexture[textureCount]
    u64 texels = 0;    ///< u32 RGBA8 pool
    u64 lut = 0;       ///< f32[kMlLutEntries]
    u64 surfaces = 0;  ///< eval input (set per addEval through the push constants; kept for inspection)
    u64 results = 0;
    u64 image = 0;     ///< f32x4 per pixel (balls)
    u64 balls = 0;     ///< MlBall[ballCount]
    u32 count = 0;
    u32 width = 0;
    u32 height = 0;
    u32 flags = 0;
    u32 ballCount = 0;
    u32 materialCount = 0;
    u32 textureCount = 0;
    u32 reserved0 = 0;
    f32 camPos[3] = {};
    f32 tanHalfY = 0.5f;
    f32 camForward[3] = {0.f, 0.f, -1.f};
    f32 aspect = 1.f;
    f32 camRight[3] = {1.f, 0.f, 0.f};
    f32 exposure = 1.f;
    f32 camUp[3] = {0.f, 1.f, 0.f};
    f32 reserved1 = 0.f;
    f32 sunDir[3] = {0.f, 1.f, 0.f}; ///< towards the sun, normalised
    f32 sunIntensity = 3.f;
    f32 skyColor[3] = {0.5f, 0.6f, 0.8f};
    f32 ambient = 1.f;
    f32 groundColor[3] = {0.2f, 0.18f, 0.15f};
    f32 reserved2 = 0.f;
    f32 background[4] = {0.f, 0.f, 0.f, 0.f};
};
static_assert(sizeof(MlParams) == 224u, "MlParams layout (ml_common.glsl / .slang)");

/// The layered-material table the WP-1.5 material resolve reads (its layered bin, see material_resolve.hpp): the
/// header of the "materials.resolve_table" buffer MaterialLayers::setLibrary builds when it is given an UploadQueue,
/// reached through its bindless storage-buffer handle (MaterialLayers::resolveTableHandle()). 32 bytes.
/// `textures` rows are MlTexture records whose `offset` is the bindless sampled-image handle of that texture's
/// mip-mapped image (R8G8B8A8_SRGB when kMlTexSrgb, else _UNORM; the mip chain of ml_mips.hpp), sampled with
/// textureGrad instead of the texel pool's manual bilinear filter; width / height / flags / mean are unchanged.
struct MlResolveTable {
    u64 materials = 0; ///< BDA of MlMaterial[materialCount] (the library's resolved materials, same order)
    u64 textures = 0;  ///< BDA of MlTexture[textureCount] (offset = bindless sampled-image handle)
    u32 materialCount = 0;
    u32 textureCount = 0;
    u32 reserved[2] = {};
};
static_assert(sizeof(MlResolveTable) == 32u, "MlResolveTable layout (ml_common.glsl / .slang)");

/// Push constants of every kernel: 32 bytes.
struct MlPush {
    u64 params = 0;   ///< BDA of this frame's MlParams
    u64 src = 0;      ///< eval: MlSurface[count]
    u64 dst = 0;      ///< eval: MlResult[count]
    u32 count = 0;    ///< eval
    u32 reserved = 0;
};
static_assert(sizeof(MlPush) == 32u, "MlPush layout");

} // namespace fuse::renderer::material_layers
