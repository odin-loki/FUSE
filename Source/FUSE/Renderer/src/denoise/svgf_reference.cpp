// WP-6.4 SVGF / A-SVGF CPU reference pipeline: see include/fuse/renderer/denoise/svgf_reference.hpp.
#include <fuse/renderer/denoise/svgf_reference.hpp>

#include <algorithm>

namespace fuse::renderer::denoise {

using svgf_kernel::F2;
using svgf_kernel::F4;

SvgfSettings svgf_preset(DenoiseSignal signal) {
    SvgfSettings s{};
    s.signal = signal;
    switch (signal) {
    case DenoiseSignal::Shadow:
        s.alphaColor = 0.2f;
        s.alphaMoments = 0.2f;
        s.maxHistory = 32.f;
        s.sigmaLuminance = 4.f;
        break;
    case DenoiseSignal::Reflection:
        s.alphaColor = 0.2f;
        s.alphaMoments = 0.2f;
        s.maxHistory = 16.f;
        s.sigmaLuminance = 4.f;
        break;
    case DenoiseSignal::Gi:
        s.alphaColor = 0.2f;
        s.alphaMoments = 0.2f;
        s.maxHistory = 32.f;
        s.sigmaLuminance = 2.f;
        break;
    }
    return s;
}

DenoiseFrameConstants resolve_constants(const SvgfSettings& s, u32 width, u32 height, bool history, bool normalOct) {
    DenoiseFrameConstants c{};
    c.width = width;
    c.height = height;
    c.strataW = strata(width);
    c.strataH = strata(height);
    c.signalStride = signal_stride(s.signal);
    c.flags = 0u;
    if (history) {
        c.flags |= kDenoiseFlagHistory;
    }
    if (s.gradients) {
        c.flags |= kDenoiseFlagGradients;
    }
    if (s.signal == DenoiseSignal::Shadow) {
        c.flags |= kDenoiseFlagScalar;
    }
    if (normalOct) {
        c.flags |= kDenoiseFlagNormalOct;
    }
    if (s.spatialVariance) {
        c.flags |= kDenoiseFlagSpatialVariance;
    }
    c.alphaColor = std::clamp(s.alphaColor, 0.f, 1.f);
    c.alphaMoments = std::clamp(s.alphaMoments, 0.f, 1.f);
    c.maxHistory = std::max(s.maxHistory, 1.f);
    c.reprojDepth = std::max(s.reprojDepth, 0.f);
    c.reprojNormal = s.reprojNormal;
    c.minReprojWeight = std::max(s.minReprojWeight, 0.f);
    c.varianceHistory = s.varianceHistory;
    c.varianceBoost = std::max(s.varianceBoost, 0.f);
    c.sigmaDepth = std::max(s.sigmaDepth, 0.f);
    c.sigmaLuminance = std::max(s.sigmaLuminance, 0.f);
    c.sigmaVarianceLum = std::max(s.sigmaVarianceLum, 1e-6f);
    c.depthEpsilon = std::max(s.depthEpsilon, 1e-8f);
    c.sigmaNormal = s.sigmaNormal;
    c.atrousIterations = std::clamp(s.atrousIterations, 1u, kDenoiseMaxAtrous);
    c.historyTap = std::min(s.historyTap, c.atrousIterations - 1u);
    c.gradientIterations = std::min(s.gradientIterations, kDenoiseMaxGradientIterations);
    c.gradientScale = std::max(s.gradientScale, 0.f);
    c.gradientEpsilon = std::max(s.gradientEpsilon, 0.f);
    return c;
}

void SvgfReference::init(u32 width, u32 height, const SvgfSettings& settings) {
    m_width = width;
    m_height = height;
    m_settings = settings;
    m_history = false;
    m_parity = 1u;
    const usize n = static_cast<usize>(width) * height;
    const usize ns = static_cast<usize>(strata(width)) * strata(height);
    for (u32 k = 0; k < 2u; ++k) {
        m_guide[k].assign(n, F4{});
        m_hist[k].assign(n, F4{});
        m_mom[k].assign(n, F4{});
    }
    m_gradZ.assign(n, F2{});
    m_accum.assign(n, F4{});
    m_variance.assign(n, F4{});
    for (auto& a : m_atrous) {
        a.assign(n, F4{});
    }
    for (auto& g : m_gradient) {
        g.assign(ns, F4{});
    }
    m_constants = resolve_constants(settings, width, height, false, false);
}

namespace {
template <typename T>
kernel::Span<T> span(std::vector<T>& v) {
    return kernel::Span<T>{v.data(), static_cast<u32>(v.size())};
}
template <typename T>
kernel::Span<const T> cspan(const std::vector<T>& v) {
    return kernel::Span<const T>{v.data(), static_cast<u32>(v.size())};
}
template <typename T>
kernel::Span<const T> cspan(const T* p, usize n) {
    return kernel::Span<const T>{p, p != nullptr ? static_cast<u32>(n) : 0u};
}
} // namespace

bool SvgfReference::runFrame(const SvgfReferenceInputs& in, kernel::Backend backend) {
    if (m_width == 0u || m_height == 0u || in.signal == nullptr || in.motion == nullptr || in.depth == nullptr ||
        in.normals == nullptr || (m_settings.gradients && in.gradient == nullptr)) {
        return false;
    }
    using namespace svgf_kernel;
    const u32 cur = m_parity ^ 1u;
    const u32 prev = m_parity;
    const DenoiseFrameConstants c = resolve_constants(m_settings, m_width, m_height, m_history, in.normalOct);
    m_constants = c;
    const usize n = static_cast<usize>(m_width) * m_height;
    const usize ns = static_cast<usize>(c.strataW) * c.strataH;
    bool ok = true;

    GuideParams gp{};
    gp.c = c;
    gp.depth = cspan(in.depth, n);
    gp.normals = cspan(in.normals, n);
    gp.guide = span(m_guide[cur]);
    gp.gradZ = span(m_gradZ);
    ok &= kernel::launch(backend, make_launch(kNameGuide, m_width, m_height), GuideKernel{}, gp).ok;

    if (m_settings.gradients) {
        GradientParams rp{};
        rp.c = c;
        rp.guide = cspan(m_guide[cur]);
        rp.src = cspan(in.gradient, ns);
        rp.dst = span(m_gradient[0]);
        rp.mode = kDenoiseGradientPrepare;
        ok &= kernel::launch(backend, make_launch(kNameGradientPrepare, c.strataW, c.strataH), GradientKernel{}, rp).ok;
        for (u32 k = 0; k < c.gradientIterations; ++k) {
            rp.src = cspan(m_gradient[k]);
            rp.dst = span(m_gradient[k + 1u]);
            rp.mode = kDenoiseGradientAtrous;
            rp.step = 1u << k;
            ok &= kernel::launch(backend, make_launch(kNameGradientAtrous, c.strataW, c.strataH), GradientKernel{}, rp).ok;
        }
    }

    TemporalParams tp{};
    tp.c = c;
    tp.signal = cspan(in.signal, n * c.signalStride);
    tp.motion = cspan(in.motion, n);
    tp.guideCur = cspan(m_guide[cur]);
    tp.guidePrev = cspan(m_guide[prev]);
    tp.gradZ = cspan(m_gradZ);
    tp.histPrev = cspan(m_hist[prev]);
    tp.momPrev = cspan(m_mom[prev]);
    tp.lambda = cspan(m_gradient[c.gradientIterations]);
    tp.accum = span(m_accum);
    tp.momCur = span(m_mom[cur]);
    ok &= kernel::launch(backend, make_launch(kNameTemporal, m_width, m_height), TemporalKernel{}, tp).ok;

    VarianceParams vp{};
    vp.c = c;
    vp.guide = cspan(m_guide[cur]);
    vp.gradZ = cspan(m_gradZ);
    vp.accum = cspan(m_accum);
    vp.moments = cspan(m_mom[cur]);
    vp.dst = span(m_variance);
    ok &= kernel::launch(backend, make_launch(kNameVariance, m_width, m_height), VarianceKernel{}, vp).ok;

    AtrousParams ap{};
    ap.c = c;
    ap.guide = cspan(m_guide[cur]);
    ap.gradZ = cspan(m_gradZ);
    for (u32 k = 0; k < c.atrousIterations; ++k) {
        ap.src = cspan(k == 0u ? m_variance : m_atrous[k - 1u]);
        ap.dst = span(m_atrous[k]);
        ap.history = k == c.historyTap ? span(m_hist[cur]) : kernel::Span<F4>{};
        ap.step = 1u << k;
        ok &= kernel::launch(backend, make_launch(kNameAtrous, m_width, m_height), AtrousKernel{}, ap).ok;
    }
    m_parity = cur;
    m_history = true;
    return ok;
}

} // namespace fuse::renderer::denoise
