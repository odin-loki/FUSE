#pragma once

// WP-9.2 3D Gaussian splatting (Kerbl, Kopanas, Leimkuehler, Drettakis 2023): the records shared by the host
// code, the CPU reference and the compute kernels. shaders/gsplat/gs_common.{glsl,slang} declare GsFrame with
// the same fields in the same order (fuse_rp_gsplat_layout checks names, order and offsets). Vulkan-free.
//
// Conventions:
//   camera space   +X right, +Y down, +Z forward (the 3DGS / COLMAP convention; pixel axes), world -> camera
//                  is a rigid 3 x 4 row-major [R | t]
//   pixels         px = fx * x / z + cx, py = fy * y / z + cy; pixel (i, j) is sampled at its centre (i + .5, j + .5)
//   composite      splat depth d = depthA + depthB / z is compared with the visibility-buffer depth at the pixel
//                  (WP-1.4 export depth / D32, forward z/w in [0, 1] by default: visible when d < scene)
//   tiles          16 x 16 pixels, row-major tile ids; sort key = (tile << 32) | bits(view z)

#include <fuse/types.hpp>

namespace fuse::renderer::gsplat {

inline constexpr u32 kGsTile = 16u;          ///< tile edge in pixels (one 256-thread workgroup per tile)
inline constexpr u32 kGsTileThreads = 256u;  ///< kGsTile * kGsTile
inline constexpr u32 kGsGroup = 256u;        ///< 1D kernels' workgroup size
inline constexpr u32 kGsMaxShDegree = 3u;
inline constexpr u32 kGsShCoeffs = 16u;      ///< (degree + 1)^2 at degree 3
inline constexpr u32 kGsSplatFloats = 60u;   ///< GsSplat as f32 words
inline constexpr u32 kGsPadValue = 0xFFFFFFFFu; ///< value of a padding sort entry

/// One Gaussian with activated parameters (what the kernels read; 240 bytes, uploaded as is).
struct GsSplat {
    f32 position[3] = {0.f, 0.f, 0.f};
    f32 opacity = 0.f;                  ///< sigmoid-activated, [0, 1]
    f32 scale[3] = {0.f, 0.f, 0.f};     ///< exp-activated standard deviations (world units)
    f32 reserved = 0.f;
    f32 rotation[4] = {1.f, 0.f, 0.f, 0.f}; ///< unit quaternion (w, x, y, z)
    f32 sh[kGsShCoeffs * 3u] = {};      ///< SH coefficient k, channel c at [k * 3 + c]; k = 0 is the DC term
};
static_assert(sizeof(GsSplat) == kGsSplatFloats * 4u, "GsSplat is 60 f32 words");

/// Preprocess output per splat (64 bytes, the GPU record layout). Culled splats are all zero.
struct GsProjected {
    f32 mean[2] = {0.f, 0.f}; ///< pixel coordinates of the projected centre
    f32 viewZ = 0.f;          ///< camera-space depth (> nearZ)
    f32 depth = 0.f;          ///< composite depth, depthA + depthB / viewZ
    f32 conic[3] = {0.f, 0.f, 0.f}; ///< inverse 2D covariance (xx, xy, yy)
    f32 opacity = 0.f;
    f32 color[3] = {0.f, 0.f, 0.f}; ///< SH radiance + 0.5, clamped at 0
    u32 radius = 0;                  ///< pixels; 0 = culled
    u32 rect[4] = {0, 0, 0, 0};      ///< tile rectangle [minX, minY, maxX, maxY) (tiles)
};
static_assert(sizeof(GsProjected) == 64u, "GsProjected is 64 bytes");

inline u32 gs_tiles_touched(const GsProjected& p) { return (p.rect[2] - p.rect[0]) * (p.rect[3] - p.rect[1]); }

/// GsFrameConstants::flags
enum GsFlag : u32 {
    kGsFlagDepthTest = 1u << 0, ///< composite against the depth image (depthHandle)
    kGsFlagReversedZ = 1u << 1, ///< the depth image is reversed Z (visible when d > scene)
};

/// Per-frame constants (one host-visible ring slot per frame in flight, read through BDA). 224 bytes.
struct GsFrameConstants {
    // --- buffers (BDA; filled by GsplatRenderer, 0 in the CPU reference) ------------------------------
    u64 splats = 0;    ///< GsSplat[splatCount]
    u64 projected = 0; ///< GsProjected[splatCount]
    u64 offsets = 0;   ///< u32[splatCount]: exclusive prefix sum of tiles touched
    u64 keys = 0;      ///< u64[capacity]
    u64 values = 0;    ///< u32[capacity]
    u64 ranges = 0;    ///< u32x2[tileCount]: [begin, end) of each tile's sorted entries
    u64 counters = 0;  ///< u32[4]: entries emitted, entries dropped (over capacity), 0, 0
    u64 output = 0;    ///< f32x4[width * height]: premultiplied splat radiance, final transmittance
    // --- sizes ----------------------------------------------------------------------------------------
    u32 splatCount = 0;
    u32 capacity = 0;  ///< sort entries (keys / values) available
    u32 width = 0;
    u32 height = 0;
    u32 tilesX = 0;
    u32 tilesY = 0;
    u32 tileCount = 0;
    u32 depthHandle = 0; ///< bindless sampled image (R32F / D32 composite depth)
    u32 flags = 0;       ///< GsFlag
    u32 shDegree = 0;    ///< active SH degree (<= the asset's)
    u32 reserved0 = 0;
    u32 reserved1 = 0;
    // --- camera ---------------------------------------------------------------------------------------
    f32 view[12] = {};   ///< world -> camera, row-major 3 x 4 [R | t]
    f32 camPos[4] = {};  ///< camera position (world), 0
    f32 fx = 0.f;
    f32 fy = 0.f;
    f32 cx = 0.f;
    f32 cy = 0.f;
    f32 nearZ = 0.f;
    f32 tanFovX = 0.f;   ///< width / (2 fx)
    f32 tanFovY = 0.f;   ///< height / (2 fy)
    f32 lowPass = 0.f;   ///< added to the 2D covariance diagonal (0.3 px^2)
    f32 depthA = 0.f;
    f32 depthB = 0.f;
    f32 alphaMin = 0.f;  ///< 1 / 255
    f32 transmittanceMin = 0.f; ///< early termination threshold (1e-4)
};
static_assert(sizeof(GsFrameConstants) == 224u, "GsFrameConstants is 224 bytes");

/// Push constants of every kernel (16 bytes).
struct GsPush {
    u64 frame = 0; ///< BDA of this frame's GsFrameConstants
    u32 padKeyHigh = 0; ///< sort padding key high word (tileCount), scan kernel
    u32 reserved = 0;
};
static_assert(sizeof(GsPush) == 16u, "GsPush is 16 bytes");

/// Camera of one frame.
struct GsCamera {
    f32 view[12] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f}; ///< world -> camera [R | t]
    f32 fx = 0.f;
    f32 fy = 0.f;
    f32 cx = 0.f;
    f32 cy = 0.f;
    f32 nearZ = 0.2f;   ///< splats with view z <= nearZ are culled (3DGS: 0.2)
    f32 depthA = 0.f;   ///< composite depth = depthA + depthB / z (the visibility buffer's device depth)
    f32 depthB = 0.f;
    bool reversedZ = false;
};

/// A look-at camera with a vertical field of view and the Vulkan forward depth z/w of a [near, far] frustum
/// (depthA = far / (far - near), depthB = -far near / (far - near); cull_types.hpp / WP-1.4 conventions).
GsCamera gs_camera_look_at(const f32 (&eye)[3], const f32 (&target)[3], const f32 (&up)[3], f32 fovY, u32 width,
                           u32 height, f32 nearPlane, f32 farPlane);

struct GsSettings {
    u32 shDegree = kGsMaxShDegree; ///< clamped to the asset's degree
    f32 lowPass = 0.3f;
    f32 alphaMin = 1.f / 255.f;
    f32 transmittanceMin = 1e-4f;
    bool depthTest = true;         ///< composite against the depth image when one is given
};

/// Tile grid, constants and camera of a frame (no buffer addresses). False for an empty extent or an invalid
/// camera (fx, fy, nearZ not positive).
bool gs_resolve_constants(const GsCamera& camera, const GsSettings& settings, u32 width, u32 height, u32 splatCount,
                          u32 assetShDegree, u32 capacity, bool hasDepth, GsFrameConstants& out);

/// Sort key bits for a tile count: 32 depth bits + enough tile bits for the padding tile (tileCount), rounded
/// up so the radix pass count is even (the sorted data ends in the primary buffers, no copy-back).
u32 gs_sort_key_bits(u32 tileCount);

} // namespace fuse::renderer::gsplat
