#pragma once

// WP-3.2 virtual shadow maps, page rendering / filtering / local lights: records shared by the C++
// side (VsmShadows, the CPU references in vsm_raster_kernel.hpp) and the shaders
// (shaders/shadow_vsm/vsm_shadow.{glsl,slang}). Keep all three in sync; every record uses 4-byte
// scalars, scalar arrays and 8-byte addresses only (no implicit padding, pinned by the
// static_asserts), so std430 / Slang pointers and C++ agree byte for byte.
//
// Device-safe (only <fuse/types.hpp>).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::vsm {

/// Local (spot / point) lights with a shadow per frame, and their page atlas.
inline constexpr u32 kMaxLocalLights = 16u;
inline constexpr u32 kCubeFaces = 6u;
inline constexpr u32 kMaxLocalPagesPerAxis = 16u;
inline constexpr u32 kMaxLocalPages = kMaxLocalPagesPerAxis * kMaxLocalPagesPerAxis;
/// Local pages have the directional pages' size (128 x 128 texels).
inline constexpr u32 kLocalPageTexels = 128u;
/// PCF radius limit (a (2r + 1)^2 box of depth comparisons).
inline constexpr u32 kMaxFilterRadius = 4u;
/// PCSS blocker search: a kPcssBlockerGrid^2 grid spread over the search radius.
inline constexpr u32 kPcssBlockerGrid = 5u;
/// Directional depth: a level's depth range is kDirDepthPages page widths centred on its depth
/// centre (VsmLevelConstants::depthCenter, the WP-3.1 depth key): d = 0.5 + (depthCenter - z) /
/// (kDirDepthPages * pageWorld), 0 towards the light. Receivers of a level lie within 63 pages
/// (containment) of the camera, the camera within one depth step (32 pages) of the centre, so they
/// are always inside the +-128-page range; casters nearer the light clamp to 0 (still occlude).
inline constexpr f32 kDirDepthPages = 256.f;
/// One texel of directional depth in d units: 1 / (kDirDepthPages * kPageTexels).
inline constexpr f32 kDirDepthPerTexel = 1.f / 32768.f;
inline constexpr u32 kDepthClearBits = 0x3F800000u; ///< float bits of 1.0 (far; nothing rendered)
inline constexpr u32 kNoSlot = 0xFFFFFFFFu;

enum VsmShadowFilter : u32 {
    kFilterHard = 0, ///< one depth comparison (nearest texel)
    kFilterPcf = 1,  ///< box of (2 pcfRadius + 1)^2 comparisons
    kFilterPcss = 2, ///< blocker search, penumbra estimate, then PCF with the estimated radius
};

enum VsmLocalType : u32 {
    kLocalNone = 0,
    kLocalSpot = 1,  ///< one page: a square perspective frustum around the cone
    kLocalPoint = 2, ///< six pages: the cube faces (90 degree frusta, vsm_raster_kernel.hpp cube_basis)
};

/// One shadowed local light (128 bytes). The spot frustum looks along `forward` with `right` / `up`
/// as the image axes; a point light's faces use the fixed cube bases. Depth stored in its pages is
/// the linear view depth along the face axis times invRange (0 at the light, 1 = range / cleared).
struct VsmLocalLight {
    f32 position[3] = {0.f, 0.f, 0.f};
    u32 slot = kNoSlot;          ///< GpuScene light slot this shadow belongs to
    f32 forward[3] = {0.f, 0.f, -1.f};
    f32 range = 0.f;
    f32 right[3] = {1.f, 0.f, 0.f};
    f32 nearPlane = 0.05f;
    f32 up[3] = {0.f, 1.f, 0.f};
    f32 invTanHalf = 1.f;        ///< 1 / tan(half field of view); point faces: 1
    u32 type = kLocalNone;       ///< VsmLocalType
    u32 faces = 0;               ///< 1 (spot) or 6 (point)
    f32 invRange = 0.f;
    f32 lightSize = 0.f;         ///< PCSS: emitter radius (world units)
    u32 page[kCubeFaces] = {0u, 0u, 0u, 0u, 0u, 0u}; ///< atlas page per face
    u32 reserved[6] = {};
};
static_assert(sizeof(VsmLocalLight) == 128u && offsetof(VsmLocalLight, type) == 64u && offsetof(VsmLocalLight, page) == 80u,
              "VsmLocalLight layout (vsm_shadow.glsl / .slang)");

/// Per-frame constants of the shadow passes and of every shading pass that samples them (host ring,
/// BDA; LightingFrameConstants::shadowsLo / shadowsHi carry the address to light.shade and the
/// forward pass). 128 + 16 x 128 bytes.
struct VsmShadowConstants {
    u64 vsm = 0;                 ///< BDA of this frame's VsmFrameConstants (0: no directional VSM)
    u64 work = 0;                ///< BDA of the raster work buffer (VsmRasterWork)
    u32 directionalSlot = kNoSlot; ///< GpuScene light slot the VSM shadows (kNoSlot: none)
    u32 filterMode = kFilterPcf;     ///< VsmShadowFilter
    u32 pcfRadius = 1u;          ///< kFilterPcf box radius (texels, <= kMaxFilterRadius)
    u32 localCount = 0u;
    f32 normalOffset = 1.5f;     ///< receiver offset along its normal, in texels of the sampled map
    f32 depthBias = 1.f;         ///< constant depth bias, in texels of the sampled map (world texel size)
    f32 sunTanAngle = 0.f;       ///< PCSS, directional: tan(angular radius of the light)
    u32 pcssMaxRadius = kMaxFilterRadius; ///< PCSS: blocker search radius and filter radius cap (texels)
    u32 localPool = 0u;          ///< bindless storage-image handle of the local atlas (R32_UINT)
    u32 localPagesX = 0u;        ///< atlas pages per row
    u32 scene = 0u;              ///< GpuScene::headerHandle()
    u32 instanceCount = 0u;      ///< GpuScene::instanceHighWater()
    u32 forceMask = 0u;          ///< local lights re-rendered this frame whatever moved (bit per light)
    u32 workHandle = 0u;         ///< bindless storage-buffer handle of the raster work buffer
    u32 localClear = kDepthClearBits;
    u32 frame = 0u;
    u32 reserved[12] = {};
    VsmLocalLight local[kMaxLocalLights] = {};
};
static_assert(offsetof(VsmShadowConstants, directionalSlot) == 16u && offsetof(VsmShadowConstants, normalOffset) == 32u &&
                  offsetof(VsmShadowConstants, localPool) == 48u && offsetof(VsmShadowConstants, localClear) == 72u &&
                  offsetof(VsmShadowConstants, local) == 128u,
              "VsmShadowConstants offsets (vsm_shadow.glsl / .slang)");
static_assert(sizeof(VsmShadowConstants) == 128u + 128u * kMaxLocalLights, "VsmShadowConstants layout");

/// Raster work buffer (u32 words).
enum VsmRasterWord : u32 {
    kWordLocalDispatchX = 0,  ///< VkDispatchIndirectCommand of the local render list (one workgroup per page)
    kWordLocalDispatchY = 1,
    kWordLocalDispatchZ = 2,
    kWordDirPages = 3,        ///< directional pages rendered this frame (vsm.raster workgroups)
    kWordLocalPages = 4,      ///< local pages rendered this frame (vsm.local_raster workgroups)
    kWordDirtyMask = 5,       ///< local lights whose range a moved / removed / added caster touched
    kWordDirTriangles = 6,    ///< triangles vsm.raster set up (after culling)
    kWordDirBigTriangles = 7, ///< of those, with a texel box > kBigTriangleTexels (one thread each)
    kWordLocalTriangles = 8,
    kWordHeaderCount = 16,
    kWordLocalList = 16,      ///< 2 words per entry: light | face << 8, atlas page
};
inline constexpr u32 kRasterWorkWords = kWordLocalList + 2u * kMaxLocalPages;

/// Push constants of every WP-3.2 kernel (32 bytes).
struct VsmRasterPush {
    u64 shadow = 0; ///< BDA of this frame's VsmShadowConstants
    u64 in = 0;     ///< vsm.probe: VsmProbeInput[count]
    u64 out = 0;    ///< vsm.probe: VsmProbeOutput[count]
    u32 count = 0;
    u32 arg = 0;
};
static_assert(sizeof(VsmRasterPush) == 32u, "VsmRasterPush layout");

/// vsm.probe (test / debug): shadow visibility of explicit world points.
struct VsmProbeInput {
    f32 position[3] = {0.f, 0.f, 0.f};
    u32 slot = kNoSlot;      ///< light slot (directional or local)
    f32 normal[3] = {0.f, 1.f, 0.f};
    s32 level = -1;          ///< directional: force this clipmap level (-1 = the marking's level + fallback)
};
static_assert(sizeof(VsmProbeInput) == 32u, "VsmProbeInput layout");

struct VsmProbeOutput {
    f32 visibility = 1.f;    ///< < 0: the forced level's page is not mapped
    s32 level = -1;          ///< directional level sampled (-1: outside the clipmap / not directional)
    f32 receiverDepth = 0.f; ///< biased receiver depth compared (d units)
    f32 margin = 0.f;        ///< min |receiverDepth - stored| over the comparisons made (ambiguity)
};
static_assert(sizeof(VsmProbeOutput) == 16u, "VsmProbeOutput layout");

/// Workgroup sizes (shaders/shadow_vsm/*).
inline constexpr u32 kRasterThreads = 64u;  ///< vsm.raster / vsm.local_raster: one page per workgroup
inline constexpr u32 kBigTriangleTexels = 256u; ///< "big" triangle statistic (kWordDirBigTriangles)
inline constexpr u32 kProbeGroup = 64u;

} // namespace fuse::renderer::vsm
