#pragma once

// WP-1.3 Hi-Z pyramid, CPU reference as a single-source kernel (docs/compute-kernels.md), one
// output texel per item, one launch per level. The GPU builds the same pyramid in ONE dispatch
// (shaders/culling/hiz_build.{slang,comp}, SPD-style: every 16x16 workgroup reduces a 64x64 mip-0
// tile down to one mip-6 texel, the last workgroup to finish, found with a global atomic counter,
// reduces mip 6 to the top). Every value is a max() of depths, which is exact, so the GPU pyramid
// equals this one bit for bit whatever the reduction order.
//
// Layout (cull_types.hpp): P = nextPow2(max(depthW, depthH)), mip 0 = max(1, P/2) squared,
// mip0[x, y] = max(depth[2x + i, 2y + j]) over i, j in {0, 1} with texels outside the depth image
// read as 1 (far: they never occlude), mip k+1 [x, y] = max of the 2x2 quad of mip k.
// A mip-0 texel therefore covers depth pixels [2x, 2x + 2) x [2y, 2y + 2), a mip-k texel
// [2^(k+1) x, 2^(k+1) (x + 1)).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/culling/cull_types.hpp>

namespace fuse::renderer::culling::hiz_kernel {

inline constexpr const char* kName = "culling_hiz_level";
inline constexpr u32 kWorkgroup = 64u;

struct HizDims {
    u32 dim0 = 0;       ///< mip-0 width == height (0 = invalid depth extent)
    u32 mipCount = 0;
    f32 scaleX = 0.f;   ///< uv -> mip-0 texel (depthWidth / 2)
    f32 scaleY = 0.f;
};

FUSE_HOST_DEVICE inline u32 next_pow2(u32 v) {
    u32 p = 1u;
    while (p < v) {
        p <<= 1u;
    }
    return p;
}

/// Pyramid shape for a depth extent (0 dims for an empty or oversized extent).
FUSE_HOST_DEVICE inline HizDims hiz_dims(u32 depthWidth, u32 depthHeight) {
    HizDims d{};
    if (depthWidth == 0u || depthHeight == 0u || depthWidth > kMaxDepthExtent || depthHeight > kMaxDepthExtent) {
        return d;
    }
    const u32 p = next_pow2(depthWidth > depthHeight ? depthWidth : depthHeight);
    d.dim0 = p >= 2u ? p / 2u : 1u;
    d.mipCount = 1u;
    while ((d.dim0 >> d.mipCount) != 0u) {
        ++d.mipCount;
    }
    d.scaleX = static_cast<f32>(depthWidth) * 0.5f;
    d.scaleY = static_cast<f32>(depthHeight) * 0.5f;
    return d;
}

/// Width (== height) of mip `level`.
FUSE_HOST_DEVICE inline u32 mip_dim(u32 dim0, u32 level) {
    const u32 d = dim0 >> level;
    return d != 0u ? d : 1u;
}

FUSE_HOST_DEVICE inline f32 max4(f32 a, f32 b, f32 c, f32 d) {
    const f32 ab = a > b ? a : b;
    const f32 cd = c > d ? c : d;
    return ab > cd ? ab : cd;
}

struct Params {
    /// Level 0: the depth image (depthWidth x depthHeight, row-major). Level k > 0: mip k-1.
    kernel::Span<const f32> src;
    u32 srcWidth = 0;
    u32 srcHeight = 0;
    bool fromDepth = true;
    kernel::Span<f32> dst; ///< dstDim x dstDim, row-major
    u32 dstDim = 0;
};

FUSE_HOST_DEVICE inline f32 fetch_src(const Params& p, u32 x, u32 y) {
    if (x >= p.srcWidth || y >= p.srcHeight) {
        return 1.f; // outside the depth image: far (never an occluder); mips are always in range
    }
    return p.src[y * p.srcWidth + x];
}

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 x = idx.linear % p.dstDim;
        const u32 y = idx.linear / p.dstDim;
        p.dst[idx.linear] = max4(fetch_src(p, 2u * x, 2u * y), fetch_src(p, 2u * x + 1u, 2u * y),
                                 fetch_src(p, 2u * x, 2u * y + 1u), fetch_src(p, 2u * x + 1u, 2u * y + 1u));
    }
};

inline kernel::KernelLaunch make_launch(u32 dstDim) {
    return kernel::KernelLaunch{kName, kernel::extent1(dstDim * dstDim), {kWorkgroup, 1u, 1u}};
}

} // namespace fuse::renderer::culling::hiz_kernel
