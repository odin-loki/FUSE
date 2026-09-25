// FUSE Relight RL-5.7: local tone mapper Look node (see local_tonemap.hpp).
#include <fuse/relight/render/post/local_tonemap.hpp>

#include <fuse/renderer/look/look_params.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::relight::render::post {

namespace look = fuse::renderer::look;

bool validateLocalToneMapPlacement(const look::LookEffectGraph& graph) {
    if (!graph.validate().ok()) {
        return false;
    }
    const u32 tm = graph.indexOf(look::LookEffect::ToneMap);
    if (tm >= graph.size()) {
        return false;
    }
    for (u32 i = 0; i < tm; ++i) {
        const look::LookEffectInfo& info = look::look_effect_info(graph.at(i));
        if (info.output != look::LookDomain::SceneHdr && info.output != look::LookDomain::Any) {
            return false;
        }
    }
    return true;
}

bool LocalToneMapper::init(u32 width, u32 height, const LocalToneMapConfig& config) {
    if (width == 0u || height == 0u) {
        return false;
    }
    m_config = config;
    m_width = width;
    m_height = height;
    m_levels = 1u;
    u32 w = width, h = height;
    const u32 wanted = std::min(config.mip, kLocalToneMapMaxLevels - 1u);
    while (m_levels <= wanted && (w > 1u || h > 1u)) {
        w = std::max(1u, (w + 1u) / 2u);
        h = std::max(1u, (h + 1u) / 2u);
        ++m_levels;
    }
    w = width;
    h = height;
    for (u32 l = 0; l < kLocalToneMapMaxLevels; ++l) {
        Level& L = m_level[l];
        if (l >= m_levels) {
            L = Level{};
            continue;
        }
        L.w = w;
        L.h = h;
        const std::size_t n = std::size_t(w) * h;
        for (u32 k = 0; k < 3u; ++k) {
            L.d[k].assign(n, 0.f);
            L.w3[k].assign(n, 0.f);
        }
        L.fused.assign(n, 0.f);
        w = std::max(1u, (w + 1u) / 2u);
        h = std::max(1u, (h + 1u) / 2u);
    }
    m_lum.assign(std::size_t(width) * height, 0.f);
    return true;
}

void LocalToneMapper::down(const std::vector<float>& src, u32 sw, u32 sh, std::vector<float>& dst, u32 dw,
                           u32 dh) const {
    for (u32 y = 0; y < dh; ++y) {
        const u32 y0 = std::min(2u * y, sh - 1u), y1 = std::min(2u * y + 1u, sh - 1u);
        for (u32 x = 0; x < dw; ++x) {
            const u32 x0 = std::min(2u * x, sw - 1u), x1 = std::min(2u * x + 1u, sw - 1u);
            dst[std::size_t(y) * dw + x] = 0.25f * (src[std::size_t(y0) * sw + x0] + src[std::size_t(y0) * sw + x1] +
                                                    src[std::size_t(y1) * sw + x0] + src[std::size_t(y1) * sw + x1]);
        }
    }
}

float LocalToneMapper::up(const std::vector<float>& src, u32 sw, u32 sh, u32 x, u32 y) const {
    // Fine pixel centre (x + 0.5) maps to coarse coordinate (x + 0.5) / 2 - 0.5.
    const float fx = std::clamp((float(x) + 0.5f) * 0.5f - 0.5f, 0.f, float(sw - 1u));
    const float fy = std::clamp((float(y) + 0.5f) * 0.5f - 0.5f, 0.f, float(sh - 1u));
    const u32 x0 = static_cast<u32>(fx), y0 = static_cast<u32>(fy);
    const u32 x1 = std::min(x0 + 1u, sw - 1u), y1 = std::min(y0 + 1u, sh - 1u);
    const float tx = fx - float(x0), ty = fy - float(y0);
    const float a = src[std::size_t(y0) * sw + x0] * (1.f - tx) + src[std::size_t(y0) * sw + x1] * tx;
    const float b = src[std::size_t(y1) * sw + x0] * (1.f - tx) + src[std::size_t(y1) * sw + x1] * tx;
    return a * (1.f - ty) + b * ty;
}

void LocalToneMapper::process(const math::Vec3* in, math::Vec3* out) {
    if (!ready()) {
        return;
    }
    const float factor[3] = {1.f / m_config.highlights, 1.f, m_config.shadows};
    const float s2 = 0.5f * m_config.exposurePreferenceSigma * m_config.exposurePreferenceSigma;
    const float centre = 0.5f + m_config.exposurePreferenceOffset;
    Level& L0 = m_level[0];
    const std::size_t n = std::size_t(m_width) * m_height;
    for (std::size_t i = 0; i < n; ++i) {
        const math::Vec3 c = in[i];
        const float lum = std::max(0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z, 0.f);
        m_lum[i] = std::isfinite(lum) ? lum : 0.f;
        float wsum = 0.f;
        for (u32 k = 0; k < 3u; ++k) {
            const float e = m_lum[i] * factor[k];
            const float d = std::sqrt(e / (1.f + e));
            const float t = d - centre;
            const float w = std::exp(-s2 * t * t) + 1e-6f;
            L0.d[k][i] = d;
            L0.w3[k][i] = w;
            wsum += w;
        }
        for (u32 k = 0; k < 3u; ++k) {
            L0.w3[k][i] /= wsum;
        }
    }
    for (u32 l = 1; l < m_levels; ++l) {
        const Level& a = m_level[l - 1u];
        Level& b = m_level[l];
        for (u32 k = 0; k < 3u; ++k) {
            down(a.d[k], a.w, a.h, b.d[k], b.w, b.h);
            down(a.w3[k], a.w, a.h, b.w3[k], b.w, b.h);
        }
    }
    // Coarsest: weighted blend of the candidates' Gaussian level.
    {
        Level& t = m_level[m_levels - 1u];
        for (std::size_t i = 0; i < std::size_t(t.w) * t.h; ++i) {
            float num = 0.f, den = 0.f;
            for (u32 k = 0; k < 3u; ++k) {
                num += t.w3[k][i] * t.d[k][i];
                den += t.w3[k][i];
            }
            t.fused[i] = num / std::max(den, 1e-12f);
        }
    }
    // Finer levels: fused Laplacian + upsampled coarser fused.
    for (u32 l = m_levels - 1u; l-- > 0u;) {
        Level& f = m_level[l];
        const Level& c = m_level[l + 1u];
        for (u32 y = 0; y < f.h; ++y) {
            for (u32 x = 0; x < f.w; ++x) {
                const std::size_t i = std::size_t(y) * f.w + x;
                float num = 0.f, den = 0.f;
                for (u32 k = 0; k < 3u; ++k) {
                    const float lap = f.d[k][i] - up(c.d[k], c.w, c.h, x, y);
                    num += f.w3[k][i] * lap;
                    den += f.w3[k][i];
                }
                f.fused[i] = num / std::max(den, 1e-12f) + up(c.fused, c.w, c.h, x, y);
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        const float F = std::clamp(L0.fused[i], 0.f, 0.999f);
        const float Y = F * F;
        const float target = Y / (1.f - Y);
        const float lum = m_lum[i];
        const float gain = lum > 1e-8f ? target / lum : 1.f;
        const math::Vec3 c = in[i];
        out[i] = math::Vec3(c.x * gain, c.y * gain, c.z * gain);
    }
}

} // namespace fuse::relight::render::post
