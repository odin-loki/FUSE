#pragma once

// WP-6.4 SVGF / A-SVGF: settings presets, the constant resolution shared by the GPU denoiser and the CPU
// reference, and the CPU reference pipeline (every pass of svgf_kernel.hpp, run through kernel::launch in the
// GPU's pass order, with the same ping-pong state). Vulkan-free: builds in the stub backend.
//
//   SvgfReference ref;
//   ref.init(w, h, svgf_preset(DenoiseSignal::Gi));
//   ref.runFrame({signal, motion, depth, normals, false, gradient});   // every frame (reset() on a cut)
//   ref.output()[i]   // (rgb, variance)

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/denoise/denoise_types.hpp>
#include <fuse/renderer/denoise/svgf_kernel.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::denoise {

/// Per-signal defaults ([SVGF] alpha 0.2, sigma_z 1, sigma_n 128, 5 iterations; the thresholds of
/// fuse_rp_denoise_quality were measured with these):
///   Shadow      scalar visibility, sigma_l 4, history 32
///   Reflection  RGB, sigma_l 4, history 16 (view-dependent: shorter memory against parallax lag)
///   GI          RGB, sigma_l 2 (1-spp GI noise is heavy-tailed: the per-sample variance drives the luminance
///               stopping, so the paper's 4 over-blurs illumination detail), history 32
/// gradients (A-SVGF) stay off: they need the producer's gradient samples (DenoiseFrameDesc::gradient).
SvgfSettings svgf_preset(DenoiseSignal signal);

/// Settings -> constants (addresses and handles 0; the GPU fills them). Clamps the iteration counts
/// (a-trous 1..5, history tap < a-trous, gradient 0..5) and derives the flags and the signal stride.
/// `history`: the previous frame's state is valid; `normalOct`: the normal input is signed octahedral.
DenoiseFrameConstants resolve_constants(const SvgfSettings& settings, u32 width, u32 height, bool history, bool normalOct);

/// One frame's inputs (host arrays of the GPU buffers' layouts).
struct SvgfReferenceInputs {
    const f32* signal = nullptr;                 ///< signal_stride(settings.signal) floats per pixel
    const svgf_kernel::F2* motion = nullptr;     ///< UV motion per pixel
    const f32* depth = nullptr;                  ///< linear depth per pixel (0 = sky)
    const svgf_kernel::F4* normals = nullptr;    ///< xyz unit normal, or oct xy when normalOct
    bool normalOct = false;
    const svgf_kernel::F4* gradient = nullptr;   ///< per stratum (required when settings.gradients)
};

class SvgfReference {
public:
    void init(u32 width, u32 height, const SvgfSettings& settings);
    void setSettings(const SvgfSettings& settings) { m_settings = settings; }
    /// The next frame starts without history.
    void reset() { m_history = false; }
    /// Runs every pass of one frame (CpuReference: serial ground truth; CpuParallel: same bits).
    bool runFrame(const SvgfReferenceInputs& in, kernel::Backend backend = kernel::Backend::CpuReference);

    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    const SvgfSettings& settings() const { return m_settings; }
    /// The constants of the last runFrame.
    const DenoiseFrameConstants& constants() const { return m_constants; }
    u32 parity() const { return m_parity; } ///< state index written by the last frame

    const std::vector<svgf_kernel::F4>& guide(u32 index) const { return m_guide[index & 1u]; }
    const std::vector<svgf_kernel::F4>& history(u32 index) const { return m_hist[index & 1u]; }
    const std::vector<svgf_kernel::F4>& moments(u32 index) const { return m_mom[index & 1u]; }
    const std::vector<svgf_kernel::F2>& gradZ() const { return m_gradZ; }
    const std::vector<svgf_kernel::F4>& accum() const { return m_accum; }
    const std::vector<svgf_kernel::F4>& variance() const { return m_variance; }
    /// Result of a-trous iteration k (the last one is output()).
    const std::vector<svgf_kernel::F4>& atrous(u32 k) const { return m_atrous[k]; }
    /// Gradient records: 0 = prepare, k = after gradient iteration k.
    const std::vector<svgf_kernel::F4>& gradient(u32 k) const { return m_gradient[k]; }
    const std::vector<svgf_kernel::F4>& output() const { return m_atrous[m_constants.atrousIterations - 1u]; }

private:
    u32 m_width = 0;
    u32 m_height = 0;
    SvgfSettings m_settings{};
    DenoiseFrameConstants m_constants{};
    bool m_history = false;
    u32 m_parity = 1u;
    std::vector<svgf_kernel::F4> m_guide[2];
    std::vector<svgf_kernel::F4> m_hist[2];
    std::vector<svgf_kernel::F4> m_mom[2];
    std::vector<svgf_kernel::F2> m_gradZ;
    std::vector<svgf_kernel::F4> m_accum;
    std::vector<svgf_kernel::F4> m_variance;
    std::vector<svgf_kernel::F4> m_atrous[kDenoiseMaxAtrous];
    std::vector<svgf_kernel::F4> m_gradient[kDenoiseMaxGradientIterations + 1u];
};

} // namespace fuse::renderer::denoise
