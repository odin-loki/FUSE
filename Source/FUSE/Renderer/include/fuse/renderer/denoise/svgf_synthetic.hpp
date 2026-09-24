#pragma once

// WP-6.4 synthetic noisy-input generator + ensemble statistics for the denoiser gates (and for manual
// tuning). Host only, Vulkan-free. It stands in for the WP-6.2 ray tracer (concurrent), with an analytic
// converged reference instead of a many-frame render.
//
// Scene (screen space, render pixels; +x right, +y down), camera panning right by `panPx` per frame, so the
// background moves by -panPx (UV motion -panPx / width); every surface point is a world coordinate
// (u = x + pan t on the background, u = x - objectX(t) on the object):
//   sky    y < 0.12 h                      depth 0 (no surface: passes through, never a neighbour)
//   wall   y < 0.45 h                      depth 30, normal +z; a soft column shadow at u = 24
//   floor  below                           depth 30 * 0.45 h / y (13.5 .. 30), normal +y; the moving object's
//                                          soft shadow (3 px penumbra)
//   ball   radius 0.14 h, static in world  sphere normals, depth 14 - 3 n.z; self-shadow terminator
//   object 0.2 w x 0.42 h rectangle         depth 6, normal +z, moving right by objectSpeedPx per frame (its
//                                          trail is disoccluded every frame)
// Converged signal: Shadow = visibility V; Reflection / GI = demodulated (albedo-free, as SVGF expects: texture
// detail is remodulated after denoising) smooth irradiance-like RGB per surface, scaled by (0.3 + 0.7 V) and by
// lightStepScale from lightStepFrame on (an abrupt lighting change for A-SVGF). The illumination features are
// the shadow penumbrae / umbrae and the geometric edges (depth and normal discontinuities).
// One-sample estimators with the converged value as their mean, per pixel random number r (hash of seed,
// frame, pixel):
//   Shadow      Bernoulli: r < V ? 1 : 0                       (variance V (1 - V))
//   Reflection  truth x 2r                                      (variance truth^2 / 3)
//   GI          truth x -ln(1 - r)  (exponential, mean 1)       (variance truth^2, heavy tail)
// A-SVGF gradient samples ([A-SVGF] 4): per 3 x 3 stratum one pixel p (hash of seed, frame, stratum); the
// previous frame's pixel q it came from; valid when q was the same surface; the sample = the luminance of q's
// surface point shaded at t - 1 and re-shaded at t with q's previous random number (so only a change of the
// lighting, never the noise, makes it differ).

#include <fuse/renderer/denoise/denoise_types.hpp>
#include <fuse/renderer/denoise/svgf_kernel.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::denoise {

struct SyntheticDesc {
    u32 width = 64;
    u32 height = 48;
    DenoiseSignal signal = DenoiseSignal::Gi;
    f32 panPx = 0.37f;         ///< camera pan per frame (background moves by -panPx)
    f32 objectSpeedPx = 1.5f;  ///< object screen motion per frame (+x)
    f32 objectStartPx = 4.f;   ///< object left edge at frame 0 (wraps around the width)
    u32 lightStepFrame = ~0u;  ///< from this frame on the lighting is scaled by lightStepScale
    f32 lightStepScale = 0.25f;
};

enum SyntheticSurface : u8 { kSurfSky = 0, kSurfWall = 1, kSurfFloor = 2, kSurfObject = 3, kSurfBall = 4 };

struct SyntheticFrame {
    u32 width = 0;
    u32 height = 0;
    u32 stride = 4;                         ///< floats per pixel of signal / truth
    std::vector<f32> signal;                ///< noisy one-sample estimate
    std::vector<f32> truth;                 ///< converged value
    std::vector<svgf_kernel::F2> motion;    ///< UV motion current - previous
    std::vector<f32> depth;                 ///< linear depth (0 = sky)
    std::vector<svgf_kernel::F4> normals;   ///< unit world normal (xyz)
    std::vector<svgf_kernel::F4> normalOct; ///< RT0 texel: GBufferEncoding::encodeNormalRgba16f in xy (half values)
    std::vector<u8> surface;                ///< SyntheticSurface per pixel
    std::vector<svgf_kernel::F4> gradient;  ///< per stratum (cur, prev, valid, 0); all invalid on frame 0
};

/// Generates frame `frame` of the sequence for random stream `seed` (resizes `out` only when needed).
void synthetic_frame(const SyntheticDesc& desc, u32 frame, u32 seed, SyntheticFrame& out);

/// Luminance of one pixel of a stride-1 (scalar) or stride-4 (RGB) record.
f32 synthetic_luminance(const f32* v, u32 stride);

/// Pixels whose four bilinear history taps (through the motion vectors) all missed their surface in the
/// previous frame (outside the image or another surface): the temporal pass must report history length 1
/// there. `stable`: all four taps on the same surface (history must continue).
void synthetic_disocclusion(const SyntheticFrame& prev, const SyntheticFrame& cur, std::vector<u8>& disoccluded,
                            std::vector<u8>& stable);

/// Per-pixel running mean / variance (Welford, f64) of a luminance over an ensemble of realisations.
class LuminanceEnsemble {
public:
    void init(usize pixels);
    void add(const f32* values, u32 stride);                ///< one realisation of the noisy signal
    void add(const std::vector<svgf_kernel::F4>& values, bool scalar); ///< one realisation of a denoiser output
    usize pixels() const { return m_mean.size(); }
    u32 samples() const { return m_count; }
    f64 mean(usize i) const { return m_mean[i]; }
    f64 variance(usize i) const { return m_count > 1u ? m_m2[i] / static_cast<f64>(m_count - 1u) : 0.0; }

private:
    void push(usize i, f64 v);
    std::vector<f64> m_mean;
    std::vector<f64> m_m2;
    u32 m_count = 0;
};

/// Variance reduction and bias of a denoiser output ensemble against the converged reference.
struct QualityReport {
    u32 pixels = 0;
    f64 inputVariance = 0.0;  ///< mean over pixels of the noisy input's variance
    f64 outputVariance = 0.0; ///< mean over pixels of the output's variance
    f64 vrf = 0.0;            ///< inputVariance / outputVariance
    f64 meanTruth = 0.0;      ///< mean converged luminance
    f64 bias = 0.0;           ///< mean |mean(output) - truth| (includes the ensemble's noise floor ~ sigma / sqrt(R))
    f64 relBias = 0.0;        ///< bias / meanTruth
    /// RMS bias with the noise floor removed: sqrt(max(0, mean[(mean(output) - truth)^2 - var(output) / R])), an
    /// unbiased estimate of the mean squared bias (the gated bias metric); relRmsBias = rmsBias / meanTruth.
    f64 rmsBias = 0.0;
    f64 relRmsBias = 0.0;
    f64 inputRelRmsBias = 0.0; ///< the same for the noisy input (sanity: ~0, the estimators are unbiased)
    f64 inputRmse = 0.0;      ///< sqrt(mean E[(input - truth)^2])
    f64 outputRmse = 0.0;     ///< sqrt(mean E[(output - truth)^2])
};

/// Over the pixels with mask[i] != 0 (every pixel when the mask is empty). `truth`: stride floats per pixel.
QualityReport evaluate_quality(const LuminanceEnsemble& input, const LuminanceEnsemble& output, const std::vector<f32>& truth,
                               u32 stride, const std::vector<u8>& mask);

} // namespace fuse::renderer::denoise
