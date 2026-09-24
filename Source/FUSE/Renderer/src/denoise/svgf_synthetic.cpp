// WP-6.4 synthetic noisy-input generator: see include/fuse/renderer/denoise/svgf_synthetic.hpp.
#include <fuse/renderer/denoise/svgf_synthetic.hpp>

#include <fuse/math/vec.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::denoise {

using svgf_kernel::F2;
using svgf_kernel::F4;

namespace {

u32 mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// Uniform [0, 1) from (seed, frame, index), 24 bits.
f32 rand01(u32 seed, u32 frame, u32 index) {
    const u32 h = mix(mix(mix(seed * 0x9E3779B9u + 0x68E31DA4u) ^ (frame * 0x85EBCA6Bu)) ^ (index * 0xC2B2AE35u + 0x27D4EB2Fu));
    return static_cast<f32>(h >> 8) / 16777216.f;
}

f32 clamp01(f32 v) { return std::min(std::max(v, 0.f), 1.f); }

struct Surf {
    u8 id = kSurfSky;
    f32 u = 0.f; ///< world coordinate (background: screen x + pan t; object: relative to its left edge)
    f32 v = 0.f;
    f32 depth = 0.f;
    f32 n[3] = {0.f, 0.f, 1.f};
};

struct Geometry {
    f32 w = 0.f;
    f32 h = 0.f;
    f32 objW = 0.f;
    f32 objY0 = 0.f;
    f32 objY1 = 0.f;
    f32 skyY = 0.f;
    f32 wallY = 0.f;
    f32 ballU = 0.f;
    f32 ballV = 0.f;
    f32 ballR = 0.f;
    f32 k = 1.f; ///< feature scale: width / 64 (shadow widths, illumination frequencies)
};

Geometry geometry(const SyntheticDesc& d) {
    Geometry g{};
    g.w = static_cast<f32>(d.width);
    g.h = static_cast<f32>(d.height);
    g.objW = std::floor(0.2f * g.w);
    g.objY0 = std::floor(0.30f * g.h);
    g.objY1 = std::floor(0.72f * g.h);
    g.skyY = std::floor(0.12f * g.h);
    g.wallY = std::floor(0.45f * g.h);
    g.ballU = 0.68f * g.w;
    g.ballV = 0.62f * g.h;
    g.ballR = 0.14f * g.h;
    g.k = g.w / 64.f;
    return g;
}

f32 objectX(const SyntheticDesc& d, const Geometry& g, u32 frame) {
    return std::fmod(d.objectStartPx + d.objectSpeedPx * static_cast<f32>(frame), g.w);
}

/// Surface seen at continuous screen position (sx, sy) at `frame`.
Surf classify(const SyntheticDesc& d, const Geometry& g, f32 sx, f32 sy, u32 frame) {
    Surf s{};
    const f32 t = static_cast<f32>(frame);
    const f32 ox = objectX(d, g, frame);
    if (sx >= ox && sx < ox + g.objW && sy >= g.objY0 && sy < g.objY1) {
        s.id = kSurfObject;
        s.u = sx - ox;
        s.v = sy;
        s.depth = 6.f;
        return s;
    }
    const f32 u = sx + d.panPx * t;
    const f32 dx = u - g.ballU;
    const f32 dy = sy - g.ballV;
    const f32 r2 = dx * dx + dy * dy;
    if (r2 < g.ballR * g.ballR) {
        s.id = kSurfBall;
        s.u = u;
        s.v = sy;
        const f32 nx = dx / g.ballR;
        const f32 ny = -dy / g.ballR;
        const f32 nz = std::sqrt(std::max(0.f, 1.f - nx * nx - ny * ny));
        const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
        s.n[0] = nx / len;
        s.n[1] = ny / len;
        s.n[2] = nz / len;
        s.depth = 14.f - 3.f * s.n[2];
        return s;
    }
    if (sy < g.skyY) {
        s.id = kSurfSky;
        return s;
    }
    s.u = u;
    s.v = sy;
    if (sy < g.wallY) {
        s.id = kSurfWall;
        s.depth = 30.f;
        return s;
    }
    s.id = kSurfFloor;
    s.depth = 30.f * g.wallY / sy;
    s.n[0] = 0.f;
    s.n[1] = 1.f;
    s.n[2] = 0.f;
    return s;
}

/// Converged visibility of a surface point at `frame`.
f32 visibility(const SyntheticDesc& d, const Geometry& g, const Surf& s, u32 frame) {
    switch (s.id) {
    case kSurfWall:
        return clamp01((std::fabs(s.u - 24.f * g.k) - 3.f * g.k) / (5.f * g.k));
    case kSurfFloor: {
        const f32 sx = s.u - d.panPx * static_cast<f32>(frame); // the point's screen x at `frame`
        const f32 ox = objectX(d, g, frame);
        const f32 x0 = ox + 4.f * g.k;
        const f32 x1 = ox + g.objW + 4.f * g.k;
        const f32 y0 = std::floor(0.74f * g.h);
        const f32 y1 = std::floor(0.92f * g.h);
        const f32 ddx = std::max(x0 - sx, sx - x1);
        const f32 ddy = std::max(y0 - s.v, s.v - y1);
        return clamp01(std::max(ddx, ddy) / (3.f * g.k) + 0.5f);
    }
    case kSurfBall: {
        const f32 l[3] = {0.5f, 0.6f, 0.62f};
        const f32 il = 1.f / std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
        const f32 nl = (s.n[0] * l[0] + s.n[1] * l[1] + s.n[2] * l[2]) * il;
        return clamp01(0.5f + 3.f * (nl - 0.2f));
    }
    case kSurfObject:
        return 1.f;
    default:
        return 0.f;
    }
}

/// Converged value (stride floats) of a surface point at `frame`.
void truth(const SyntheticDesc& d, const Geometry& g, const Surf& s, u32 frame, f32* out) {
    const f32 vis = visibility(d, g, s, frame);
    const f32 step = frame >= d.lightStepFrame ? d.lightStepScale : 1.f;
    if (d.signal == DenoiseSignal::Shadow) {
        out[0] = s.id == kSurfSky ? 0.f : vis;
        return;
    }
    f32 c[3] = {0.f, 0.f, 0.f};
    switch (s.id) {
    case kSurfWall:
        c[0] = 0.55f + 0.25f * std::sin(0.21f * s.u / g.k);
        c[1] = 0.45f + 0.2f * std::cos(0.17f * s.v / g.k);
        c[2] = 0.35f + 0.15f * std::sin(0.11f * (s.u + s.v) / g.k);
        break;
    case kSurfFloor:
        c[0] = 0.3f + 0.15f * std::sin(0.13f * s.u / g.k);
        c[1] = 0.5f + 0.1f * std::cos(0.23f * s.u / g.k);
        c[2] = 0.25f + 0.1f * std::cos(0.19f * s.v / g.k);
        break;
    case kSurfObject:
        c[0] = 1.2f;
        c[1] = 0.6f;
        c[2] = 0.25f;
        break;
    case kSurfBall: {
        const f32 l[3] = {0.5f, 0.6f, 0.62f};
        const f32 il = 1.f / std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
        const f32 nl = std::max(0.f, (s.n[0] * l[0] + s.n[1] * l[1] + s.n[2] * l[2]) * il);
        const f32 k = 0.25f + 0.75f * nl;
        c[0] = 0.3f * k;
        c[1] = 0.8f * k;
        c[2] = 0.5f * k;
        break;
    }
    default:
        break;
    }
    const f32 shade = (0.3f + 0.7f * vis) * step;
    out[0] = c[0] * shade;
    out[1] = c[1] * shade;
    out[2] = c[2] * shade;
    out[3] = 0.f;
}

/// One-sample estimate of `t` with random number r.
void estimate(DenoiseSignal signal, const f32* t, f32 r, f32* out) {
    if (signal == DenoiseSignal::Shadow) {
        out[0] = r < t[0] ? 1.f : 0.f;
        return;
    }
    const f32 m = signal == DenoiseSignal::Reflection ? 2.f * r : -std::log(1.f - r);
    out[0] = t[0] * m;
    out[1] = t[1] * m;
    out[2] = t[2] * m;
    out[3] = 0.f;
}

} // namespace

f32 synthetic_luminance(const f32* v, u32 stride) {
    if (stride == 1u) {
        return v[0];
    }
    return 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
}

void synthetic_frame(const SyntheticDesc& d, u32 frame, u32 seed, SyntheticFrame& out) {
    const Geometry g = geometry(d);
    const u32 w = d.width;
    const u32 h = d.height;
    const usize n = static_cast<usize>(w) * h;
    const u32 stride = signal_stride(d.signal);
    out.width = w;
    out.height = h;
    out.stride = stride;
    out.signal.resize(n * stride);
    out.truth.resize(n * stride);
    out.motion.resize(n);
    out.depth.resize(n);
    out.normals.resize(n);
    out.normalOct.resize(n);
    out.surface.resize(n);
    out.gradient.resize(static_cast<usize>(strata(w)) * strata(h));
    const f32 bgMotion = -d.panPx / g.w;
    const f32 objMotion = d.objectSpeedPx / g.w;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const u32 i = y * w + x;
            const Surf s = classify(d, g, static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f, frame);
            out.surface[i] = s.id;
            out.depth[i] = s.depth;
            out.normals[i] = F4{s.n[0], s.n[1], s.n[2], 0.f};
            const math::Vec2 oct = GBufferEncoding::encodeNormalRgba16f(math::Vec3{s.n[0], s.n[1], s.n[2]});
            out.normalOct[i] = F4{oct.x, oct.y, 0.f, 1.f};
            out.motion[i] = s.id == kSurfObject ? F2{objMotion, 0.f} : F2{bgMotion, 0.f};
            f32* t = &out.truth[static_cast<usize>(i) * stride];
            truth(d, g, s, frame, t);
            estimate(d.signal, t, rand01(seed, frame, i), &out.signal[static_cast<usize>(i) * stride]);
        }
    }
    // A-SVGF gradient samples.
    const u32 sw = strata(w);
    const u32 sh = strata(h);
    for (u32 sy = 0; sy < sh; ++sy) {
        for (u32 sx = 0; sx < sw; ++sx) {
            const u32 si = sy * sw + sx;
            F4 rec{};
            const u32 pick = mix(seed * 0x2545F491u ^ (frame * 0x9E3779B9u) ^ (si * 0x632BE5ABu)) % 9u;
            const u32 x = sx * kDenoiseStratum + pick % 3u;
            const u32 y = sy * kDenoiseStratum + pick / 3u;
            if (frame > 0u && x < w && y < h && out.surface[y * w + x] != kSurfSky) {
                const F2 m = out.motion[y * w + x];
                const f32 qxf = std::floor(static_cast<f32>(x) + 0.5f - m.x * g.w);
                const f32 qyf = std::floor(static_cast<f32>(y) + 0.5f - m.y * g.h);
                if (qxf >= 0.f && qyf >= 0.f && qxf < g.w && qyf < g.h) {
                    const u32 qx = static_cast<u32>(qxf);
                    const u32 qy = static_cast<u32>(qyf);
                    const Surf q = classify(d, g, qxf + 0.5f, qyf + 0.5f, frame - 1u);
                    if (q.id == out.surface[y * w + x]) {
                        const f32 r = rand01(seed, frame - 1u, qy * w + qx);
                        f32 tp[4] = {};
                        f32 tc[4] = {};
                        f32 ep[4] = {};
                        f32 ec[4] = {};
                        truth(d, g, q, frame - 1u, tp);
                        truth(d, g, q, frame, tc);
                        estimate(d.signal, tp, r, ep);
                        estimate(d.signal, tc, r, ec);
                        rec = F4{synthetic_luminance(ec, stride), synthetic_luminance(ep, stride), 1.f, 0.f};
                    }
                }
            }
            out.gradient[si] = rec;
        }
    }
}

void synthetic_disocclusion(const SyntheticFrame& prev, const SyntheticFrame& cur, std::vector<u8>& disoccluded,
                            std::vector<u8>& stable) {
    const u32 w = cur.width;
    const u32 h = cur.height;
    const usize n = static_cast<usize>(w) * h;
    disoccluded.assign(n, 0u);
    stable.assign(n, 0u);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const u32 i = y * w + x;
            if (cur.surface[i] == kSurfSky) {
                continue;
            }
            const f32 px = (static_cast<f32>(x) + 0.5f) - cur.motion[i].x * static_cast<f32>(w) - 0.5f;
            const f32 py = (static_cast<f32>(y) + 0.5f) - cur.motion[i].y * static_cast<f32>(h) - 0.5f;
            const s32 ix = static_cast<s32>(std::floor(px));
            const s32 iy = static_cast<s32>(std::floor(py));
            u32 same = 0;
            u32 other = 0;
            for (u32 t = 0; t < 4u; ++t) {
                const s32 tx = ix + static_cast<s32>(t & 1u);
                const s32 ty = iy + static_cast<s32>(t >> 1u);
                if (tx < 0 || ty < 0 || tx >= static_cast<s32>(w) || ty >= static_cast<s32>(h) ||
                    prev.surface[static_cast<u32>(ty) * w + static_cast<u32>(tx)] != cur.surface[i]) {
                    ++other;
                } else {
                    ++same;
                }
            }
            disoccluded[i] = other == 4u ? 1u : 0u;
            stable[i] = same == 4u ? 1u : 0u;
        }
    }
}

void LuminanceEnsemble::init(usize pixels) {
    m_mean.assign(pixels, 0.0);
    m_m2.assign(pixels, 0.0);
    m_count = 0;
}

void LuminanceEnsemble::push(usize i, f64 v) {
    const f64 delta = v - m_mean[i];
    m_mean[i] += delta / static_cast<f64>(m_count);
    m_m2[i] += delta * (v - m_mean[i]);
}

void LuminanceEnsemble::add(const f32* values, u32 stride) {
    ++m_count;
    for (usize i = 0; i < m_mean.size(); ++i) {
        push(i, static_cast<f64>(synthetic_luminance(values + i * stride, stride)));
    }
}

void LuminanceEnsemble::add(const std::vector<F4>& values, bool scalar) {
    ++m_count;
    for (usize i = 0; i < m_mean.size(); ++i) {
        const F4& v = values[i];
        const f64 l = scalar ? static_cast<f64>(v.x) : 0.2126 * v.x + 0.7152 * v.y + 0.0722 * v.z;
        push(i, l);
    }
}

QualityReport evaluate_quality(const LuminanceEnsemble& input, const LuminanceEnsemble& output, const std::vector<f32>& truthValues,
                               u32 stride, const std::vector<u8>& mask) {
    QualityReport r{};
    f64 inVar = 0.0;
    f64 outVar = 0.0;
    f64 sumTruth = 0.0;
    f64 bias = 0.0;
    f64 inMse = 0.0;
    f64 outMse = 0.0;
    f64 outBias2 = 0.0;
    f64 inBias2 = 0.0;
    const f64 rOut = output.samples() > 0u ? static_cast<f64>(output.samples()) : 1.0;
    const f64 rIn = input.samples() > 0u ? static_cast<f64>(input.samples()) : 1.0;
    for (usize i = 0; i < output.pixels(); ++i) {
        if (!mask.empty() && mask[i] == 0u) {
            continue;
        }
        const f64 t = static_cast<f64>(synthetic_luminance(&truthValues[i * stride], stride));
        ++r.pixels;
        inVar += input.variance(i);
        outVar += output.variance(i);
        sumTruth += t;
        const f64 bi = input.mean(i) - t;
        const f64 bo = output.mean(i) - t;
        bias += std::fabs(bo);
        outBias2 += bo * bo - output.variance(i) / rOut;
        inBias2 += bi * bi - input.variance(i) / rIn;
        inMse += input.variance(i) + bi * bi;
        outMse += output.variance(i) + bo * bo;
    }
    if (r.pixels == 0u) {
        return r;
    }
    const f64 k = 1.0 / static_cast<f64>(r.pixels);
    r.inputVariance = inVar * k;
    r.outputVariance = outVar * k;
    r.vrf = r.outputVariance > 0.0 ? r.inputVariance / r.outputVariance : 0.0;
    r.meanTruth = sumTruth * k;
    r.bias = bias * k;
    r.relBias = r.meanTruth > 0.0 ? r.bias / r.meanTruth : 0.0;
    r.rmsBias = std::sqrt(std::max(0.0, outBias2 * k));
    r.relRmsBias = r.meanTruth > 0.0 ? r.rmsBias / r.meanTruth : 0.0;
    r.inputRelRmsBias = r.meanTruth > 0.0 ? std::sqrt(std::max(0.0, inBias2 * k)) / r.meanTruth : 0.0;
    r.inputRmse = std::sqrt(inMse * k);
    r.outputRmse = std::sqrt(outMse * k);
    return r;
}

} // namespace fuse::renderer::denoise
