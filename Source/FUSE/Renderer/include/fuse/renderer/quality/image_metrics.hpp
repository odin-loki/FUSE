#pragma once

// Image-quality and temporal-stability metrics for upscaler / anti-aliasing evaluation (host API).
//
//   PSNR     10 log10(1 / MSE) over display-encoded RGB in [0, 1] (+inf for identical images).
//   SSIM     Wang et al. 2004 on Rec. 601 luma, Gaussian 11x11 (sigma 1.5) or 8x8 box window, mean over
//            windows fully inside the image; matches scikit-image `structural_similarity(gaussian_weights=True,
//            sigma=1.5, use_sample_covariance=False, data_range=1)`.
//   FLIP     LDR-FLIP (Andersson et al., HPG 2020; BSD-3 reference github.com/NVlabs/flip), mean over pixels,
//            default 67.02 pixels per degree (0.7 m from a 0.7 m wide 3840-pixel display).
//   Temporal tPSNR: PSNR of the frame-to-frame change of the test sequence against the change of the reference
//            (flicker / shimmer and temporal lag both raise it); temporal FLIP excess: mean over pixels of
//            max(0, FLIP(test_t, test_t-1) - FLIP(ref_t, ref_t-1)) — the perceived frame-to-frame change the
//            test has on top of the reference's own motion.
//   Masked   mean |luma error|, RMS and PSNR restricted to a mask: the ghosting residual (mask = the trail a
//            moving object left over the last frames) and the disocclusion error (mask = pixels not visible in
//            the previous frame).
//
// Inputs are either display-referred (already tone mapped / encoded, [0, 1]) or linear scene colour, which is
// exposed, clamped to [0, 1] and sRGB-encoded first. All per-pixel work runs as single-source kernels
// (quality_kernels.hpp) on `QualityOptions::backend`; reductions are serial in double precision, so every CPU
// backend returns bit-identical scores.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::quality {

/// Non-owning RGB image (row-major, width * height).
struct QualityImage {
    const math::Vec3* pixels = nullptr;
    u32 width = 0;
    u32 height = 0;

    bool valid() const { return pixels != nullptr && width != 0u && height != 0u; }
};

enum class QualityEncoding : u8 {
    Display = 0, ///< Values are display-referred [0, 1] (clamped only).
    Linear = 1,  ///< Linear scene colour: * exposure, clamp, sRGB OETF.
};

struct QualityOptions {
    QualityEncoding encoding = QualityEncoding::Display;
    f32 exposure = 1.f;
    kernel::Backend backend = kernel::Backend::CpuParallel;
};

enum class SsimWindow : u8 {
    Gaussian11 = 0, ///< 11x11 circular-symmetric Gaussian, sigma 1.5 (the paper's reported configuration).
    Box8 = 1,       ///< 8x8 square window (the paper's first experiments).
};

/// FLIP pixels-per-degree for a viewing setup: distance * (resolution_x / width) * pi / 180.
f32 flipPixelsPerDegree(f32 distanceMeters, f32 resolutionX, f32 widthMeters);
/// 67.0206 — the FLIP default (0.7 m, 3840 px, 0.7 m).
f32 flipDefaultPixelsPerDegree();

struct PsnrResult {
    f64 mse = 0.0;
    f64 psnr = 0.0; ///< +inf when mse == 0.
};

struct TemporalResult {
    f64 tmse = 0.0;
    f64 tpsnr = 0.0;              ///< +inf when the test changes exactly like the reference.
    f64 flip_excess = 0.0;        ///< mean max(0, FLIP(test_t, test_t-1) - FLIP(ref_t, ref_t-1)); 0 unless requested.
    f64 test_change_flip = 0.0;   ///< mean FLIP(test_t, test_t-1)
    f64 ref_change_flip = 0.0;    ///< mean FLIP(ref_t, ref_t-1)
};

struct MaskedErrorResult {
    u64 pixels = 0;             ///< Mask pixels (mask != 0).
    f64 mean_abs_luma = 0.0;    ///< mean |luma(test) - luma(ref)| over the mask
    f64 rms = 0.0;              ///< sqrt(mean RGB squared error) over the mask ("residual energy")
    f64 psnr = 0.0;             ///< PSNR over the mask (+inf when exact)
    f64 fraction_above = 0.0;   ///< share of mask pixels with |luma error| > threshold
};

/// PSNR from an MSE on a [0, peak] signal (+inf for mse == 0).
f64 psnrFromMse(f64 mse, f64 peak = 1.0);

/// Reusable evaluator (owns scratch; not thread-safe). Every metric launches its kernels on the configured
/// backend and records them in the kernel stats under the quality_* / flip_* names.
class ImageQualityEvaluator {
public:
    explicit ImageQualityEvaluator(const QualityOptions& options = {});

    QualityOptions& options() { return m_options; }
    const QualityOptions& options() const { return m_options; }

    /// False (and a default result) when the images are invalid or differ in size.
    bool psnr(const QualityImage& test, const QualityImage& reference, PsnrResult& out);
    /// Mean SSIM (1 for identical images); NaN when invalid or smaller than the window.
    f64 ssim(const QualityImage& test, const QualityImage& reference, SsimWindow window = SsimWindow::Gaussian11);
    /// Mean LDR-FLIP (0 for identical images); NaN when invalid. Per-pixel map in `lastFlipMap()`.
    f64 flip(const QualityImage& test, const QualityImage& reference, f32 pixelsPerDegree = flipDefaultPixelsPerDegree());
    /// Temporal metrics of one frame pair. `withFlip` adds the temporal-FLIP terms (two FLIP evaluations).
    bool temporal(const QualityImage& testPrev, const QualityImage& testCur, const QualityImage& refPrev,
                  const QualityImage& refCur, bool withFlip, TemporalResult& out,
                  f32 pixelsPerDegree = flipDefaultPixelsPerDegree());
    /// Error restricted to `mask` (u8 per pixel, != 0 = inside).
    bool maskedError(const QualityImage& test, const QualityImage& reference, const u8* mask, MaskedErrorResult& out,
                     f32 threshold = 0.1f);

    const std::vector<f32>& lastFlipMap() const { return m_flipMap; }
    const std::vector<f32>& lastSsimMap() const { return m_ssimMap; }
    /// |luma error| map of the last psnr / maskedError call.
    const std::vector<f32>& lastAbsLumaMap() const { return m_absLuma; }

private:
    bool prepare(const QualityImage& test, const QualityImage& reference);

    QualityOptions m_options{};
    std::vector<f32> m_lumaA;
    std::vector<f32> m_lumaB;
    std::vector<f32> m_sqError;
    std::vector<f32> m_absLuma;
    std::vector<f32> m_ssimMap;
    std::vector<f32> m_flipMap;
    std::vector<f32> m_flipMapB;
    std::vector<math::Vec3> m_refYcc;
    std::vector<math::Vec3> m_testYcc;
    std::vector<math::Vec4> m_refColor;
    std::vector<math::Vec4> m_testColor;
    std::vector<math::Vec3> m_refFeature;
    std::vector<math::Vec3> m_testFeature;
};

/// Trail mask of a moving object: pixels covered by `objectId` in any of `historyCount` previous id buffers but
/// not in the current one (ids are per display pixel). Writes 0/1 into `out` (width * height).
void buildTrailMask(const u32* const* previousIds, u32 historyCount, const u32* currentIds, u32 objectId, u32 width,
                    u32 height, u8* out);

/// Dilates a 0/1 mask by `radius` pixels (square structuring element) — keeps edge pixels of a trail inside.
void dilateMask(const u8* in, u32 width, u32 height, u32 radius, u8* out);

/// True when any value in the image is NaN or infinite.
bool imageHasNonFinite(const math::Vec3* pixels, usize count);

} // namespace fuse::renderer::quality
