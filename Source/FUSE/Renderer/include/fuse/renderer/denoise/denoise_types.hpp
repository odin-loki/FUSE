#pragma once

// WP-6.4 in-tree denoiser (SVGF / A-SVGF): the records shared by the C++ host code, the single-source CPU
// reference (svgf_kernel.hpp) and the compute kernels (shaders/denoise/dn_common.{glsl,slang} declare the same
// fields in the same order; the layout gate fuse_rp_denoise_layout checks the offsets). Vulkan-free and
// device-safe (only <fuse/types.hpp>): builds in the stub backend.
//
// Algorithm (self-written from the papers; no NRD, the pinned FidelityFX subset has no denoiser):
//   [SVGF]   C. Schied, A. Kaplanyan, C. Wyman, A. Patney, C. R. A. Chaitanya, J. Burgess, S. Liu,
//            C. Dachsbacher, A. Lefohn, M. Salvi. "Spatiotemporal Variance-Guided Filtering: Real-Time
//            Reconstruction for Path-Traced Global Illumination". High Performance Graphics 2017.
//   [A-SVGF] C. Schied, C. Peters, C. Dachsbacher. "Gradient Estimation for Real-Time Adaptive Temporal
//            Filtering". Proc. ACM Comput. Graph. Interact. Tech. 1(2) (HPG 2018).
//
// Storage: every per-pixel record is f32 (x 2 or x 4) in device buffers reached through buffer device
// addresses, so every pass computes in the CPU reference's f32 and the parity gates compare like with like.
//
//   inputs (the caller's buffers / images, this frame):
//     signal     f32 x signalStride per pixel: the noisy, demodulated signal (stride 1: shadow visibility;
//                stride 4: RGB + ignored w for reflections / GI)
//     motion     f32 x 2 per pixel: UV motion current - previous (WP-4.1 TemporalMotion::motionBuffer)
//     depth      f32 per pixel: linear view depth, 0 = sky / no surface (WP-4.1 TemporalMotion::depthBuffer)
//     normal     either f32 x 4 per pixel (xyz = unit world normal) or the WP-1.5 G-buffer RT0 image
//                (RGBA16F, signed octahedral normal in xy: GBufferEncoding::encodeNormalRgba16f), read through
//                a bindless sampled-image handle
//     gradient   optional (A-SVGF), f32 x 4 per 3 x 3 stratum: (luminance of the previous frame's sample
//                re-shaded in this frame with its previous random numbers, luminance of that sample in the
//                previous frame, valid (> 0), unused). Produced by the ray tracer ([A-SVGF] section 4).
//   state (persistent, ping-pong by frame parity): guide f32x4 (n.xyz, z), colour history f32x4 (rgb, 0),
//     moments f32x4 (m1, m2, history length, 0)
//   work (per frame): depth gradient f32x2, temporal result f32x4 (rgb, 0), filtered f32x4 (rgb, variance)
//     ping-pong (or one per iteration when keepIntermediates), gradient f32x4 per stratum
//   output f32x4 per pixel: (rgb, variance) (scalar signals: (v, 0, 0, variance)) + an RGBA16F storage image
//     ((rgb, variance); scalar signals: (v, v, v, variance)).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::denoise {

/// Workgroup edge of every kernel (8 x 8 threads, one pixel / stratum each).
inline constexpr u32 kDenoiseTile = 8u;
/// A-SVGF gradient stratum edge in pixels ([A-SVGF]: one gradient sample per 3 x 3 stratum).
inline constexpr u32 kDenoiseStratum = 3u;
/// Upper bounds of the configurable iteration counts.
inline constexpr u32 kDenoiseMaxAtrous = 5u;
inline constexpr u32 kDenoiseMaxGradientIterations = 5u;

/// Signal variants (presets: svgf_preset).
enum class DenoiseSignal : u8 {
    Shadow = 0,     ///< scalar visibility (stride 1)
    Reflection = 1, ///< RGB (stride 4)
    Gi = 2,         ///< RGB (stride 4)
};

/// DenoiseFrameConstants::flags
enum DenoiseFlag : u32 {
    kDenoiseFlagHistory = 1u << 0,         ///< the previous frame's state is valid (else every pixel starts over)
    kDenoiseFlagGradients = 1u << 1,       ///< A-SVGF: gradient input present, temporal alpha adapts to lambda
    kDenoiseFlagScalar = 1u << 2,          ///< scalar signal: luminance = x, y = z = 0
    kDenoiseFlagNormalOct = 1u << 3,       ///< normal input holds the signed octahedral normal in xy (RT0)
    kDenoiseFlagNormalImage = 1u << 4,     ///< GPU: the normal input is the bindless image normalImage
    kDenoiseFlagSpatialVariance = 1u << 5, ///< estimate the variance spatially while the history is short
};

/// DenoisePush::flags (per pass).
enum DenoisePassFlag : u32 {
    kDenoisePassWriteImage = 1u << 0, ///< a-trous: also store the result into outputImage
};

/// Sub-pass selectors (DenoisePush::mode) of dn_gradient.
enum DenoiseGradientMode : u32 {
    kDenoiseGradientPrepare = 0u, ///< producer samples -> (delta, max, valid, 0)
    kDenoiseGradientAtrous = 1u,  ///< one edge-aware a-trous iteration on the stratum grid
};

/// Per-frame constants, one host-visible ring slot per frame in flight, read through BDA (240 bytes, std430).
struct DenoiseFrameConstants {
    // --- addresses (0 = absent; the CPU reference ignores them) -----------------------------------
    u64 signal = 0;     ///< f32 x signalStride per pixel
    u64 motion = 0;     ///< f32x2 per pixel
    u64 depth = 0;      ///< f32 per pixel
    u64 normal = 0;     ///< f32x4 per pixel (0 when normalImage is used)
    u64 gradientIn = 0; ///< f32x4 per stratum (A-SVGF producer samples)
    u64 guideCur = 0;   ///< f32x4 per pixel (n.xyz, z)
    u64 guidePrev = 0;
    u64 gradZ = 0;      ///< f32x2 per pixel (dz/dx, dz/dy)
    u64 histPrev = 0;   ///< f32x4 per pixel (rgb, 0)
    u64 histCur = 0;
    u64 momPrev = 0;    ///< f32x4 per pixel (m1, m2, history length, 0)
    u64 momCur = 0;
    u64 accum = 0;      ///< f32x4 per pixel (rgb, 0): temporal result
    u64 lambda = 0;     ///< f32x4 per stratum: the filtered gradient the temporal pass reads
    u64 output = 0;     ///< f32x4 per pixel (rgb, variance)
    u64 reserved0 = 0;
    // --- extent, handles, flags ---------------------------------------------------------------------
    u32 width = 0;
    u32 height = 0;
    u32 strataW = 0; ///< ceil(width / 3)
    u32 strataH = 0;
    u32 normalImage = 0; ///< bindless sampled-image handle (kDenoiseFlagNormalImage)
    u32 outputImage = 0; ///< bindless storage-image handle (RGBA16F), 0 = none
    u32 flags = 0;       ///< DenoiseFlag
    u32 signalStride = 4u; ///< floats per pixel of the signal input (1 or 4)
    // --- temporal accumulation ([SVGF] 4.1) ----------------------------------------------------------
    f32 alphaColor = 0.2f;   ///< minimum blend weight of the new sample (colour)
    f32 alphaMoments = 0.2f; ///< minimum blend weight of the new sample (moments)
    f32 maxHistory = 32.f;   ///< history length cap (frames)
    f32 reprojDepth = 0.05f; ///< relative depth tolerance of a history tap (x z) ...
    f32 reprojNormal = 0.9f; ///< ... and minimum dot(n_prev, n_cur)
    f32 minReprojWeight = 1e-3f; ///< minimum summed bilinear weight of the valid taps (else disoccluded)
    // --- variance estimation ([SVGF] 4.2) ------------------------------------------------------------
    f32 varianceHistory = 4.f;   ///< history length below which the variance is estimated spatially (7 x 7)
    f32 varianceBoost = 4.f;     ///< spatial estimate x (boost / length)
    // --- edge-stopping functions ([SVGF] 4.3, 4.4) ---------------------------------------------------
    f32 sigmaDepth = 1.f;        ///< sigma_z
    f32 sigmaLuminance = 4.f;    ///< sigma_l (a-trous: x sqrt(3 x 3 Gaussian of the variance))
    f32 sigmaVarianceLum = 10.f; ///< luminance phi of the spatial variance estimate (no variance yet)
    f32 depthEpsilon = 1e-2f;    ///< epsilon of the depth weight's denominator (metres)
    u32 sigmaNormal = 128u;      ///< sigma_n: integer exponent of max(0, dot(n_p, n_q))
    u32 atrousIterations = 5u;   ///< 1..kDenoiseMaxAtrous (steps 1, 2, 4, 8, 16)
    u32 historyTap = 0u;         ///< the a-trous iteration whose result becomes the colour history
    u32 gradientIterations = 2u; ///< A-SVGF gradient a-trous iterations (0..kDenoiseMaxGradientIterations)
    // --- A-SVGF ([A-SVGF] 5) -------------------------------------------------------------------------
    f32 gradientScale = 2.f;     ///< lambda = min(1, scale |delta| / max) ([A-SVGF]: 1; 2 compensates the dilution by the filter)
    f32 gradientEpsilon = 1e-4f; ///< lambda = 0 where the filtered max luminance is below this
    u32 reserved1 = 0;
    u32 reserved2 = 0;
};
static_assert(sizeof(DenoiseFrameConstants) == 240, "DenoiseFrameConstants layout (dn_common.glsl / .slang)");
static_assert(offsetof(DenoiseFrameConstants, width) == 128, "DenoiseFrameConstants::width");
static_assert(offsetof(DenoiseFrameConstants, alphaColor) == 160, "DenoiseFrameConstants::alphaColor");
static_assert(offsetof(DenoiseFrameConstants, sigmaDepth) == 192, "DenoiseFrameConstants::sigmaDepth");
static_assert(offsetof(DenoiseFrameConstants, sigmaNormal) == 208, "DenoiseFrameConstants::sigmaNormal");
static_assert(offsetof(DenoiseFrameConstants, gradientScale) == 224, "DenoiseFrameConstants::gradientScale");

/// Push constants (64 bytes, compute stage): src / dst / aux are f32x4 addresses (0 = unused).
struct DenoisePush {
    u64 frame = 0; ///< BDA of DenoiseFrameConstants
    u64 src = 0;
    u64 dst = 0;
    u64 aux = 0;   ///< a-trous: colour-history destination (0 = none)
    u32 mode = 0;  ///< DenoiseGradientMode (dn_gradient)
    u32 step = 1;  ///< a-trous step (pixels / strata)
    u32 flags = 0; ///< DenoisePassFlag
    u32 reserved[5] = {0u, 0u, 0u, 0u, 0u};
};
static_assert(sizeof(DenoisePush) == 64, "DenoisePush layout");

/// Tunables of one denoiser instance (resolved into DenoiseFrameConstants by resolve_constants).
struct SvgfSettings {
    DenoiseSignal signal = DenoiseSignal::Gi;
    f32 alphaColor = 0.2f;
    f32 alphaMoments = 0.2f;
    f32 maxHistory = 32.f;
    f32 reprojDepth = 0.05f;
    f32 reprojNormal = 0.9f;
    f32 minReprojWeight = 1e-3f;
    bool spatialVariance = true;
    f32 varianceHistory = 4.f;
    f32 varianceBoost = 4.f;
    f32 sigmaDepth = 1.f;
    f32 sigmaLuminance = 4.f;
    f32 sigmaVarianceLum = 10.f;
    f32 depthEpsilon = 1e-2f;
    u32 sigmaNormal = 128u;
    u32 atrousIterations = 5u;
    u32 historyTap = 0u;
    bool gradients = false; ///< A-SVGF (needs the producer's gradient samples every frame)
    u32 gradientIterations = 2u;
    f32 gradientScale = 2.f;
    f32 gradientEpsilon = 1e-4f;
};

/// Floats per pixel of a signal variant's input (Shadow: 1, Reflection / GI: 4).
FUSE_HOST_DEVICE constexpr u32 signal_stride(DenoiseSignal s) { return s == DenoiseSignal::Shadow ? 1u : 4u; }

/// ceil(n / kDenoiseStratum)
FUSE_HOST_DEVICE constexpr u32 strata(u32 n) { return (n + kDenoiseStratum - 1u) / kDenoiseStratum; }

} // namespace fuse::renderer::denoise
