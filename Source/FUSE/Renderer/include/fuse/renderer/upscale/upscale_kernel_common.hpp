#pragma once

// Device-safe helpers shared by the single-source spatial upscaler / sharpener kernels
// (fsr1_kernel.hpp, cas_kernel.hpp, nis_kernel.hpp; docs/compute-kernels.md, docs/upscalers.md).
//
// The vendored GPU sources (Engine/lib/fidelityfx, Engine/lib/nvidia-nis) are HLSL/GLSL; the CPU ports
// transcribe their float32 paths operation for operation. Two GPU behaviours are reproduced explicitly
// so the ports match the shaders numerically:
//   * FidelityFX's bit-trick approximations (ffxApproximateReciprocal & co.) are integer arithmetic on the
//     float bit pattern: exact on every backend.
//   * GLSL/HLSL min/max map to IEEE-754 minNum/maxNum on GPUs (a NaN operand yields the other one). RCAS
//     depends on it for black neighbourhoods (0 * inf); gpu_min / gpu_max implement it branch-free.
// Image loads clamp to the edge. That is what the samplers the shaders use do (EASU gathers and NIS loads
// go through a linear-clamp sampler); RCAS / CAS use texelFetch, which is undefined outside the image on
// the GPU, so the ports define it as clamp-to-edge.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <cstring>

namespace fuse::renderer::upscale::kernels {

/// Row-major RGBA f32 image (read side). `data` has `width * height` texels.
struct RgbaSurface {
    const math::Vec4* data = nullptr;
    u32 width = 0;
    u32 height = 0;

    FUSE_HOST_DEVICE bool valid() const { return data != nullptr && width > 0u && height > 0u; }
};

/// Row-major RGBA f32 image (write side).
struct RgbaTarget {
    math::Vec4* data = nullptr;
    u32 width = 0;
    u32 height = 0;

    FUSE_HOST_DEVICE bool valid() const { return data != nullptr && width > 0u && height > 0u; }
};

FUSE_HOST_DEVICE inline u32 as_u32(f32 value) {
#if defined(__CUDA_ARCH__)
    return __float_as_uint(value);
#else
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
#endif
}

FUSE_HOST_DEVICE inline f32 as_f32(u32 bits) {
#if defined(__CUDA_ARCH__)
    return __uint_as_float(bits);
#else
    f32 value = 0.f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
#endif
}

/// IEEE-754 minNum / maxNum (GPU min/max): a NaN operand returns the other operand.
FUSE_HOST_DEVICE inline f32 gpu_min(f32 a, f32 b) { return (b != b || a < b) ? a : b; }
FUSE_HOST_DEVICE inline f32 gpu_max(f32 a, f32 b) { return (b != b || a > b) ? a : b; }
/// ffxMin3 / ffxMax3 in ffx_core_glsl.h: min(x, min(y, z)).
FUSE_HOST_DEVICE inline f32 gpu_min3(f32 x, f32 y, f32 z) { return gpu_min(x, gpu_min(y, z)); }
FUSE_HOST_DEVICE inline f32 gpu_max3(f32 x, f32 y, f32 z) { return gpu_max(x, gpu_max(y, z)); }
/// GLSL clamp(x, 0, 1) == min(max(x, 0), 1).
FUSE_HOST_DEVICE inline f32 saturate(f32 x) { return gpu_min(gpu_max(x, 0.f), 1.f); }
/// GLSL mix / HLSL lerp.
FUSE_HOST_DEVICE inline f32 lerp(f32 a, f32 b, f32 t) { return a * (1.f - t) + b * t; }

// ffx_core_gpu_common.h approximations, transcribed bit for bit.
/// ffxApproximateReciprocal: 1/x to roughly 5 bits.
FUSE_HOST_DEVICE inline f32 ffx_rcp_approx(f32 value) { return as_f32(0x7ef07ebbu - as_u32(value)); }
/// ffxApproximateReciprocalMedium: one Newton step on a tuned seed.
FUSE_HOST_DEVICE inline f32 ffx_rcp_medium(f32 value) {
    const f32 b = as_f32(0x7ef19fffu - as_u32(value));
    return b * (-b * value + 2.f);
}
/// ffxApproximateReciprocalSquareRoot.
FUSE_HOST_DEVICE inline f32 ffx_rsqrt_approx(f32 value) { return as_f32(0x5f347d74u - (as_u32(value) >> 1u)); }
/// ffxApproximateSqrt.
FUSE_HOST_DEVICE inline f32 ffx_sqrt_approx(f32 value) { return as_f32((as_u32(value) >> 1u) + 0x1fbc4639u); }

FUSE_HOST_DEVICE inline s32 clamp_coord(s32 v, u32 extent) {
    return v < 0 ? 0 : (v >= static_cast<s32>(extent) ? static_cast<s32>(extent) - 1 : v);
}

/// Clamp-to-edge texel load.
FUSE_HOST_DEVICE inline math::Vec4 load_clamped(const RgbaSurface& image, s32 x, s32 y) {
    return image.data[static_cast<u32>(clamp_coord(y, image.height)) * image.width +
                      static_cast<u32>(clamp_coord(x, image.width))];
}

/// Bilinear sample at continuous texel-space coordinates (texel centres at integer + 0.5), clamp-to-edge.
/// The same addressing as a GPU linear-clamp sampler at normalized coordinate (tx / width, ty / height).
FUSE_HOST_DEVICE inline math::Vec4 sample_bilinear(const RgbaSurface& image, f32 tx, f32 ty) {
    const f32 sx = tx - 0.5f;
    const f32 sy = ty - 0.5f;
    const f32 fx = std::floor(sx);
    const f32 fy = std::floor(sy);
    const f32 ax = sx - fx;
    const f32 ay = sy - fy;
    const s32 x0 = static_cast<s32>(fx);
    const s32 y0 = static_cast<s32>(fy);
    const math::Vec4 c00 = load_clamped(image, x0, y0);
    const math::Vec4 c10 = load_clamped(image, x0 + 1, y0);
    const math::Vec4 c01 = load_clamped(image, x0, y0 + 1);
    const math::Vec4 c11 = load_clamped(image, x0 + 1, y0 + 1);
    const f32 w00 = (1.f - ax) * (1.f - ay);
    const f32 w10 = ax * (1.f - ay);
    const f32 w01 = (1.f - ax) * ay;
    const f32 w11 = ax * ay;
    return math::Vec4(c00.x * w00 + c10.x * w10 + c01.x * w01 + c11.x * w11,
                      c00.y * w00 + c10.y * w10 + c01.y * w01 + c11.y * w11,
                      c00.z * w00 + c10.z * w10 + c01.z * w01 + c11.z * w11,
                      c00.w * w00 + c10.w * w10 + c01.w * w01 + c11.w * w11);
}

/// 8x8 workgroups over the output image (one item per output pixel).
inline constexpr u32 kPixelTile = 8u;

inline kernel::KernelLaunch pixel_launch(const char* name, u32 width, u32 height) {
    return kernel::KernelLaunch{name, kernel::extent2(width, height), {kPixelTile, kPixelTile, 1u}};
}

// ---- Bilinear upscale (quality baseline) -----------------------------------------------------------

inline constexpr const char* kBilinearKernelName = "upscale_bilinear";

struct BilinearParams {
    RgbaSurface src{};
    RgbaTarget dst{};
};

/// Output pixel centre mapped to the source with the same convention as EASU / NIS
/// (src = (dst + 0.5) * src_size / dst_size - 0.5).
struct BilinearKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BilinearParams& p) const {
        const f32 sx = static_cast<f32>(p.src.width) / static_cast<f32>(p.dst.width);
        const f32 sy = static_cast<f32>(p.src.height) / static_cast<f32>(p.dst.height);
        const f32 tx = (static_cast<f32>(idx.global.x) + 0.5f) * sx;
        const f32 ty = (static_cast<f32>(idx.global.y) + 0.5f) * sy;
        p.dst.data[idx.global.y * p.dst.width + idx.global.x] = sample_bilinear(p.src, tx, ty);
    }
};

} // namespace fuse::renderer::upscale::kernels
