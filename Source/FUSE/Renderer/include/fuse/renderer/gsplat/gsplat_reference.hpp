#pragma once

// WP-9.2 CPU reference renderer: the oracle of every GPU pass, written in the kernels' operation order (the
// GLSL twins mark their intermediates `precise`, Slang compiles with -fp-mode precise).
//
//   preprocess   per splat: world -> camera, cull (z <= nearZ, degenerate 2D covariance, empty tile rect),
//                3D covariance R S S^T R^T, EWA projection J W Sigma W^T J^T (+ low-pass), conic, 3-sigma radius,
//                tile rectangle, SH radiance along the camera -> splat direction
//   scan         exclusive prefix sum of the tiles touched -> each splat's first sort entry
//   emit         one (key, value) per touched tile: key = (tile << 32) | bits(view z), value = splat index;
//                entries past the capacity are dropped; the rest of the capacity is padding (tileCount << 32 |
//                0xFFFFFFFF, value 0xFFFFFFFF) so the fixed-size sort puts it last
//   sort         stable ascending sort of the whole capacity (== the LSD radix sort)
//   ranges       [begin, end) of each tile in the sorted entries
//   raster       per pixel, front to back: alpha = min(0.99, opacity exp(-d^T conic d / 2)), skipped below
//                alphaMin; stops at the first splat behind the composite depth (sorted by view z, so every
//                later one is behind too) or when the transmittance would drop below transmittanceMin
//                (that splat not blended); output (sum c alpha T, T)

#include <fuse/renderer/gsplat/gsplat_types.hpp>

#include <vector>

namespace fuse::renderer::gsplat {

GsProjected gs_preprocess_splat(const GsSplat& splat, const GsFrameConstants& frame);

/// SH radiance + 0.5, clamped at 0, of `degree` bands along the unit direction (x, y, z).
void gs_eval_sh(const GsSplat& splat, u32 degree, f32 x, f32 y, f32 z, f32 (&rgb)[3]);

/// One pixel of the raster (entries [begin, end) of the sorted values); `sceneDepth` ignored without
/// kGsFlagDepthTest.
void gs_raster_pixel(const GsProjected* projected, const u32* values, u32 begin, u32 end, u32 x, u32 y,
                     f32 sceneDepth, const GsFrameConstants& frame, f32 (&out)[4]);

inline u64 gs_pad_key(u32 tileCount) { return (static_cast<u64>(tileCount) << 32) | 0xFFFFFFFFull; }

struct GsReferenceFrame {
    std::vector<GsProjected> projected;
    std::vector<u32> offsets;
    std::vector<u64> keys;   ///< sorted, capacity entries
    std::vector<u32> values; ///< sorted, capacity entries
    std::vector<u32> ranges; ///< 2 per tile
    std::vector<f32> image;  ///< 4 per pixel
    u32 entries = 0;         ///< emitted (before the capacity clamp)
    u32 dropped = 0;         ///< entries over the capacity
};

/// The whole pipeline. `sceneDepth`: width x height composite depth (null = no depth test).
void gs_render_reference(const GsSplat* splats, const GsFrameConstants& frame, const f32* sceneDepth,
                         GsReferenceFrame& out);

/// Peak signal-to-noise ratio (dB) of two RGB(A) images over the rgb channels, peak 1 (identical -> 200).
f64 gs_psnr(const f32* a, const f32* b, usize pixels, u32 stride = 4u);

} // namespace fuse::renderer::gsplat
