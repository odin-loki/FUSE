#include <fuse/renderer/quality/image_metrics.hpp>
#include <fuse/renderer/quality/quality_kernels.hpp>

#include <fuse/compute_kernel/launch.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::renderer::quality {
namespace {

template <typename T>
kernel::Span<T> spanOf(std::vector<T>& v) {
    return kernel::make_span(v.data(), static_cast<u32>(v.size()));
}

template <typename T>
kernel::Span<const T> constSpan(const T* data, usize count) {
    return kernel::make_span(data, static_cast<u32>(count));
}

bool sameSize(const QualityImage& a, const QualityImage& b) {
    return a.valid() && b.valid() && a.width == b.width && a.height == b.height;
}

f64 serialMean(const std::vector<f32>& values) {
    if (values.empty()) {
        return 0.0;
    }
    f64 sum = 0.0;
    for (f32 v : values) {
        sum += static_cast<f64>(v);
    }
    return sum / static_cast<f64>(values.size());
}

kernel::KernelLaunch imageLaunch(const char* name, u32 width, u32 height) {
    return kernel::KernelLaunch{name, kernel::extent2(width, height), kWorkgroup};
}

} // namespace

f32 flipPixelsPerDegree(f32 distanceMeters, f32 resolutionX, f32 widthMeters) {
    return distanceMeters * (resolutionX / widthMeters) * (3.14159265358979f / 180.0f);
}

f32 flipDefaultPixelsPerDegree() {
    return flipPixelsPerDegree(0.7f, 3840.0f, 0.7f);
}

f64 psnrFromMse(f64 mse, f64 peak) {
    if (!(mse > 0.0)) {
        return mse == 0.0 ? std::numeric_limits<f64>::infinity() : std::numeric_limits<f64>::quiet_NaN();
    }
    return 10.0 * std::log10(peak * peak / mse);
}

ImageQualityEvaluator::ImageQualityEvaluator(const QualityOptions& options) : m_options(options) {}

bool ImageQualityEvaluator::prepare(const QualityImage& test, const QualityImage& reference) {
    if (!sameSize(test, reference)) {
        return false;
    }
    const usize n = static_cast<usize>(test.width) * test.height;
    m_lumaA.resize(n);
    m_lumaB.resize(n);
    m_sqError.resize(n);
    m_absLuma.resize(n);
    PrepareParams p{};
    p.a = constSpan(test.pixels, n);
    p.b = constSpan(reference.pixels, n);
    p.luma_a = spanOf(m_lumaA);
    p.luma_b = spanOf(m_lumaB);
    p.sq_error = spanOf(m_sqError);
    p.abs_luma = spanOf(m_absLuma);
    p.width = test.width;
    p.height = test.height;
    p.exposure = m_options.exposure;
    p.linear_input = m_options.encoding == QualityEncoding::Linear ? 1u : 0u;
    return kernel::launch(m_options.backend, imageLaunch(kPrepareName, test.width, test.height), PrepareKernel{}, p).ok;
}

bool ImageQualityEvaluator::psnr(const QualityImage& test, const QualityImage& reference, PsnrResult& out) {
    out = {};
    if (!prepare(test, reference)) {
        return false;
    }
    out.mse = serialMean(m_sqError);
    out.psnr = psnrFromMse(out.mse);
    return true;
}

f64 ImageQualityEvaluator::ssim(const QualityImage& test, const QualityImage& reference, SsimWindow window) {
    const u32 win = window == SsimWindow::Box8 ? 8u : 11u;
    if (!sameSize(test, reference) || test.width < win || test.height < win || !prepare(test, reference)) {
        return std::numeric_limits<f64>::quiet_NaN();
    }
    SsimParams p{};
    p.window = win;
    if (window == SsimWindow::Box8) {
        for (u32 i = 0; i < win; ++i) {
            p.weights[i] = 1.f / static_cast<f32>(win);
        }
    } else {
        constexpr f32 kSigma = 1.5f;
        f64 sum = 0.0;
        f64 w[kMaxSsimWindow] = {};
        for (u32 i = 0; i < win; ++i) {
            const f64 d = static_cast<f64>(i) - 5.0;
            w[i] = std::exp(-(d * d) / (2.0 * kSigma * kSigma));
            sum += w[i];
        }
        for (u32 i = 0; i < win; ++i) {
            p.weights[i] = static_cast<f32>(w[i] / sum);
        }
    }
    const f32 k1 = 0.01f;
    const f32 k2 = 0.03f;
    p.c1 = k1 * k1;
    p.c2 = k2 * k2;
    const u32 ow = test.width - win + 1u;
    const u32 oh = test.height - win + 1u;
    m_ssimMap.resize(static_cast<usize>(ow) * oh);
    p.x = constSpan(m_lumaA.data(), m_lumaA.size());
    p.y = constSpan(m_lumaB.data(), m_lumaB.size());
    p.map = spanOf(m_ssimMap);
    p.width = test.width;
    p.height = test.height;
    if (!kernel::launch(m_options.backend, imageLaunch(kSsimName, ow, oh), SsimKernel{}, p).ok) {
        return std::numeric_limits<f64>::quiet_NaN();
    }
    return serialMean(m_ssimMap);
}

f64 ImageQualityEvaluator::flip(const QualityImage& test, const QualityImage& reference, f32 pixelsPerDegree) {
    if (!sameSize(test, reference) || !(pixelsPerDegree > 0.f)) {
        return std::numeric_limits<f64>::quiet_NaN();
    }
    const u32 w = test.width;
    const u32 h = test.height;
    const usize n = static_cast<usize>(w) * h;
    m_refYcc.resize(n);
    m_testYcc.resize(n);
    m_refColor.resize(n);
    m_testColor.resize(n);
    m_refFeature.resize(n);
    m_testFeature.resize(n);
    m_flipMap.resize(n);

    const FlipFilters filters = make_flip_filters(std::min(pixelsPerDegree, 230.f));

    FlipConvertParams cp{};
    cp.ref = constSpan(reference.pixels, n);
    cp.test = constSpan(test.pixels, n);
    cp.ref_ycxcz = spanOf(m_refYcc);
    cp.test_ycxcz = spanOf(m_testYcc);
    cp.width = w;
    cp.height = h;
    cp.exposure = m_options.exposure;
    cp.linear_input = m_options.encoding == QualityEncoding::Linear ? 1u : 0u;
    if (!kernel::launch(m_options.backend, imageLaunch(kFlipConvertName, w, h), FlipConvertKernel{}, cp).ok) {
        return std::numeric_limits<f64>::quiet_NaN();
    }

    FlipFilterXParams xp{};
    xp.ref_ycxcz = constSpan(m_refYcc.data(), n);
    xp.test_ycxcz = constSpan(m_testYcc.data(), n);
    xp.ref_color = spanOf(m_refColor);
    xp.test_color = spanOf(m_testColor);
    xp.ref_feature = spanOf(m_refFeature);
    xp.test_feature = spanOf(m_testFeature);
    xp.width = w;
    xp.height = h;
    xp.filters = filters;
    if (!kernel::launch(m_options.backend, imageLaunch(kFlipFilterXName, w, h), FlipFilterXKernel{}, xp).ok) {
        return std::numeric_limits<f64>::quiet_NaN();
    }

    FlipFilterYParams yp{};
    yp.ref_color = constSpan(m_refColor.data(), n);
    yp.test_color = constSpan(m_testColor.data(), n);
    yp.ref_feature = constSpan(m_refFeature.data(), n);
    yp.test_feature = constSpan(m_testFeature.data(), n);
    yp.error = spanOf(m_flipMap);
    yp.width = w;
    yp.height = h;
    yp.filters = filters;
    if (!kernel::launch(m_options.backend, imageLaunch(kFlipFilterYName, w, h), FlipFilterYKernel{}, yp).ok) {
        return std::numeric_limits<f64>::quiet_NaN();
    }
    return serialMean(m_flipMap);
}

bool ImageQualityEvaluator::temporal(const QualityImage& testPrev, const QualityImage& testCur,
                                     const QualityImage& refPrev, const QualityImage& refCur, bool withFlip,
                                     TemporalResult& out, f32 pixelsPerDegree) {
    out = {};
    if (!sameSize(testPrev, testCur) || !sameSize(testCur, refPrev) || !sameSize(refPrev, refCur)) {
        return false;
    }
    const u32 w = testCur.width;
    const u32 h = testCur.height;
    const usize n = static_cast<usize>(w) * h;
    m_sqError.resize(n);
    TemporalParams p{};
    p.test_prev = constSpan(testPrev.pixels, n);
    p.test_cur = constSpan(testCur.pixels, n);
    p.ref_prev = constSpan(refPrev.pixels, n);
    p.ref_cur = constSpan(refCur.pixels, n);
    p.sq_error = spanOf(m_sqError);
    p.width = w;
    p.height = h;
    p.exposure = m_options.exposure;
    p.linear_input = m_options.encoding == QualityEncoding::Linear ? 1u : 0u;
    if (!kernel::launch(m_options.backend, imageLaunch(kTemporalName, w, h), TemporalKernel{}, p).ok) {
        return false;
    }
    out.tmse = serialMean(m_sqError);
    out.tpsnr = psnrFromMse(out.tmse);
    if (withFlip) {
        out.ref_change_flip = flip(refCur, refPrev, pixelsPerDegree);
        m_flipMapB = m_flipMap;
        out.test_change_flip = flip(testCur, testPrev, pixelsPerDegree);
        f64 excess = 0.0;
        for (usize i = 0; i < n; ++i) {
            excess += std::max(0.0, static_cast<f64>(m_flipMap[i]) - static_cast<f64>(m_flipMapB[i]));
        }
        out.flip_excess = excess / static_cast<f64>(n);
    }
    return true;
}

bool ImageQualityEvaluator::maskedError(const QualityImage& test, const QualityImage& reference, const u8* mask,
                                        MaskedErrorResult& out, f32 threshold) {
    out = {};
    if (mask == nullptr || !prepare(test, reference)) {
        return false;
    }
    const usize n = m_absLuma.size();
    f64 absSum = 0.0;
    f64 sqSum = 0.0;
    u64 above = 0;
    for (usize i = 0; i < n; ++i) {
        if (mask[i] == 0u) {
            continue;
        }
        ++out.pixels;
        absSum += static_cast<f64>(m_absLuma[i]);
        sqSum += static_cast<f64>(m_sqError[i]);
        above += m_absLuma[i] > threshold ? 1u : 0u;
    }
    if (out.pixels == 0u) {
        out.psnr = std::numeric_limits<f64>::infinity();
        return true;
    }
    const f64 count = static_cast<f64>(out.pixels);
    out.mean_abs_luma = absSum / count;
    out.rms = std::sqrt(sqSum / count);
    out.psnr = psnrFromMse(sqSum / count);
    out.fraction_above = static_cast<f64>(above) / count;
    return true;
}

void buildTrailMask(const u32* const* previousIds, u32 historyCount, const u32* currentIds, u32 objectId, u32 width,
                    u32 height, u8* out) {
    const usize n = static_cast<usize>(width) * height;
    for (usize i = 0; i < n; ++i) {
        bool before = false;
        for (u32 k = 0; k < historyCount && !before; ++k) {
            before = previousIds[k] != nullptr && previousIds[k][i] == objectId;
        }
        out[i] = (before && currentIds[i] != objectId) ? 1u : 0u;
    }
}

void dilateMask(const u8* in, u32 width, u32 height, u32 radius, u8* out) {
    const i32 r = static_cast<i32>(radius);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            u8 v = 0u;
            for (i32 dy = -r; dy <= r && v == 0u; ++dy) {
                for (i32 dx = -r; dx <= r && v == 0u; ++dx) {
                    const i32 nx = static_cast<i32>(x) + dx;
                    const i32 ny = static_cast<i32>(y) + dy;
                    if (nx >= 0 && ny >= 0 && nx < static_cast<i32>(width) && ny < static_cast<i32>(height)) {
                        v = in[static_cast<u32>(ny) * width + static_cast<u32>(nx)] != 0u ? 1u : 0u;
                    }
                }
            }
            out[y * width + x] = v;
        }
    }
}

bool imageHasNonFinite(const math::Vec3* pixels, usize count) {
    for (usize i = 0; i < count; ++i) {
        if (!std::isfinite(pixels[i].x) || !std::isfinite(pixels[i].y) || !std::isfinite(pixels[i].z)) {
            return true;
        }
    }
    return false;
}

} // namespace fuse::renderer::quality
