#pragma once

// WP-6.5 radiance cascades (research): the record shared by the C++ host code and the compute kernels
// (shaders/rc_common.{glsl,slang} declare struct RcPush with the same fields in the same order;
// fuse_rp_rc_layout checks names, order and offsets). Vulkan-free: builds in the stub backend.
//
// Storage (all through buffer device addresses, f32x4 records):
//   scene     one f32x4 per texel: rgb emission, a = opacity (> 0.5: opaque; a ray stops on it and returns its
//             emission; flatland surfaces are opaque emitters / blockers)
//   dirs      f32x4 per direction (cos, sin, 0, 0) of every cascade, cascade i at RcLayout::dirOffset[i]
//             (host-built from double cos / sin, so the CPU and the kernels use the same bits)
//   cascades  merged radiance per (probe, direction): f32x4 (rgb, 0), index (py * probesX + px) * dirs + k;
//             two ping-pong sections (cascade i reads i + 1)
//   output    mean radiance per pixel (f32x4: rgb, 1)

#include <fuse/types.hpp>

namespace fuse::renderer::research::rc {

/// Threads per workgroup of both kernels (1D dispatch).
inline constexpr u32 kRcGroupSize = 64u;
/// Upper bound on the cascade count (branching 2: t_12 = r0 * 4095; branching 4: t_12 = r0 * 5592405).
inline constexpr u32 kRcMaxCascades = 12u;

/// RcPush::flags
enum RcFlag : u32 {
    kRcFlagBilinearFix = 1u << 0, ///< one ray per bilinear neighbour, ending at that neighbour's interval start
    kRcFlagHasUpper = 1u << 1,    ///< merge with cascade i + 1 (else with the sky: the top cascade)
};

/// Push constants of rc.cascade and rc.gather (112 bytes <= 128 guaranteed).
struct RcPush {
    u64 scene = 0; ///< f32x4 per texel (rc.cascade)
    u64 dirs = 0;  ///< this cascade's direction table (f32x4 per direction)
    u64 upper = 0; ///< cascade i + 1 merged radiance (rc.cascade) / cascade 0 (rc.gather)
    u64 dst = 0;   ///< cascade i merged radiance (rc.cascade) / per-pixel output (rc.gather)
    u32 width = 0; ///< scene / output extent
    u32 height = 0;
    u32 probesX = 0; ///< this cascade's probe grid (rc.gather: cascade 0's)
    u32 probesY = 0;
    u32 dirCount = 0; ///< this cascade's directions (rc.gather: cascade 0's)
    u32 upperProbesX = 0;
    u32 upperProbesY = 0;
    u32 flags = 0; ///< RcFlag
    f32 spacing = 1.0f;      ///< probe spacing in texels (probe (x, y) sits at ((x + 0.5) s, (y + 0.5) s))
    f32 upperSpacing = 2.0f; ///< cascade i + 1's
    f32 tStart = 0.0f;       ///< ray interval [tStart, tEnd) in texels
    f32 tEnd = 1.0f;
    f32 step = 0.5f;    ///< march step in texels
    f32 invStep = 2.0f; ///< 1 / step
    f32 skyR = 0.0f; ///< sky radiance (beyond the top cascade); scalars, not an array: Slang lays push
    f32 skyG = 0.0f; ///< constants out std140 (array stride 16)
    f32 skyB = 0.0f;
    u32 count = 0; ///< records of this dispatch (probes * dirs, or pixels)
    u32 branch = 4u;        ///< directions of cascade i + 1 per direction of cascade i (RcSettings::branch)
    f32 invBranch = 0.25f;  ///< 1 / branch
};
static_assert(sizeof(RcPush) == 112u, "RcPush is 112 bytes");

} // namespace fuse::renderer::research::rc
