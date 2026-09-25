// WP-6.5 radiance cascades (research): CPU reference, brute force and metrics. See rc_reference.hpp.
//
// rc_trace / rc_cascade_record / rc_gather_pixel are the twins of shaders/rc_common.* / rc_cascade.* /
// rc_gather.*: the same f32 operations in the same order (the kernels are built with `precise` /
// -fp-mode precise, so no multiply-add is contracted on either side).
#include <fuse/renderer/research/rc/rc_reference.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fuse::renderer::research::rc {

namespace {
bool isPow2(u32 v) { return v != 0u && (v & (v - 1u)) == 0u; }

inline void load4(const f32* p, u64 index, f32 (&v)[4]) {
    const f32* q = p + index * 4u;
    v[0] = q[0];
    v[1] = q[1];
    v[2] = q[2];
    v[3] = q[3];
}

u32 clampIndex(i32 v, u32 count) {
    if (v < 0) {
        return 0u;
    }
    const u32 u = static_cast<u32>(v);
    return u >= count ? count - 1u : u;
}

/// Mean of the b children bk .. bk + b-1 of direction k on upper probe (x, y) (rgb).
void childAverage(const RcPush& p, const f32* upper, u32 x, u32 y, u32 k, f32 (&out)[3]) {
    const u32 upperDirs = p.dirCount * p.branch;
    const u64 base = (static_cast<u64>(y) * p.upperProbesX + x) * upperDirs + static_cast<u64>(k) * p.branch;
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    for (u32 j = 0; j < p.branch; ++j) {
        f32 v[4];
        load4(upper, base + j, v);
        r = r + v[0];
        g = g + v[1];
        b = b + v[2];
    }
    out[0] = r * p.invBranch;
    out[1] = g * p.invBranch;
    out[2] = b * p.invBranch;
}

struct Bilinear {
    u32 x[2];
    u32 y[2];
    f32 w[4]; ///< (x0, y0), (x1, y0), (x0, y1), (x1, y1)
};

Bilinear bilinear(f32 px, f32 py, f32 spacing, u32 countX, u32 countY) {
    const f32 u = px / spacing - 0.5f;
    const f32 v = py / spacing - 0.5f;
    const f32 fu = std::floor(u);
    const f32 fv = std::floor(v);
    const f32 ax = u - fu;
    const f32 ay = v - fv;
    const i32 ix = static_cast<i32>(fu);
    const i32 iy = static_cast<i32>(fv);
    Bilinear b{};
    b.x[0] = clampIndex(ix, countX);
    b.x[1] = clampIndex(ix + 1, countX);
    b.y[0] = clampIndex(iy, countY);
    b.y[1] = clampIndex(iy + 1, countY);
    const f32 bx = 1.0f - ax;
    const f32 by = 1.0f - ay;
    b.w[0] = bx * by;
    b.w[1] = ax * by;
    b.w[2] = bx * ay;
    b.w[3] = ax * ay;
    return b;
}
} // namespace

// --- scene -------------------------------------------------------------------------------------------------
void RcScene::resize(u32 w, u32 h) {
    width = w;
    height = h;
    texels.assign(static_cast<usize>(w) * h * 4u, 0.0f);
}

void RcScene::clear() { std::fill(texels.begin(), texels.end(), 0.0f); }

void RcScene::set(u32 x, u32 y, f32 r, f32 g, f32 b, bool isOpaque) {
    if (x >= width || y >= height) {
        return;
    }
    f32* t = texels.data() + (static_cast<usize>(y) * width + x) * 4u;
    t[0] = r;
    t[1] = g;
    t[2] = b;
    t[3] = isOpaque ? 1.0f : 0.0f;
}

void RcScene::fillRect(i32 x0, i32 y0, i32 x1, i32 y1, f32 r, f32 g, f32 b, bool isOpaque) {
    const i32 xa = std::max(x0, 0);
    const i32 ya = std::max(y0, 0);
    const i32 xb = std::min(x1, static_cast<i32>(width));
    const i32 yb = std::min(y1, static_cast<i32>(height));
    for (i32 y = ya; y < yb; ++y) {
        for (i32 x = xa; x < xb; ++x) {
            set(static_cast<u32>(x), static_cast<u32>(y), r, g, b, isOpaque);
        }
    }
}

void RcScene::fillDisk(f32 cx, f32 cy, f32 radius, f32 r, f32 g, f32 b, bool isOpaque) {
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const f32 dx = static_cast<f32>(x) + 0.5f - cx;
            const f32 dy = static_cast<f32>(y) + 0.5f - cy;
            if (dx * dx + dy * dy <= radius * radius) {
                set(x, y, r, g, b, isOpaque);
            }
        }
    }
}

// --- layout ------------------------------------------------------------------------------------------------
bool RcLayout::compute(const RcSettings& settings, u32 width, u32 height, RcLayout& out) {
    out = RcLayout{};
    if (width == 0u || height == 0u || !isPow2(settings.probeSpacing0) || settings.dirs0 == 0u ||
        !(settings.interval0 > 0.0f) || !(settings.step > 0.0f) || (settings.branch != 2u && settings.branch != 4u)) {
        return false;
    }
    const f64 diagonal = std::sqrt(static_cast<f64>(width) * width + static_cast<f64>(height) * height);
    u32 n = 0;
    if (settings.cascadeCount != 0u) {
        n = std::min(settings.cascadeCount, kRcMaxCascades);
    } else {
        f64 reach = 0.0;
        f64 length = settings.interval0;
        while (n < kRcMaxCascades && reach < diagonal) {
            reach += length;
            length *= settings.branch;
            ++n;
        }
    }
    u32 spacing = settings.probeSpacing0;
    u32 dirs = settings.dirs0;
    f32 t = 0.0f;
    f32 length = settings.interval0;
    for (u32 i = 0; i < n; ++i) {
        // Stop before a cascade would need more directions than a u32 index can address sensibly.
        if (dirs > (1u << 20)) {
            break;
        }
        out.probesX[i] = (width + spacing - 1u) / spacing;
        out.probesY[i] = (height + spacing - 1u) / spacing;
        out.dirs[i] = dirs;
        out.dirOffset[i] = out.dirTotal;
        out.spacing[i] = static_cast<f32>(spacing);
        out.tStart[i] = t;
        out.tEnd[i] = t + length;
        out.records[i] = static_cast<u64>(out.probesX[i]) * out.probesY[i] * dirs;
        out.maxRecords = std::max(out.maxRecords, out.records[i]);
        out.totalRecords += out.records[i];
        out.dirTotal += dirs;
        out.cascades = i + 1u;
        t = out.tEnd[i];
        length *= static_cast<f32>(settings.branch);
        spacing *= 2u;
        dirs *= settings.branch;
    }
    return out.cascades > 0u;
}

void buildDirectionTable(const RcLayout& layout, std::vector<f32>& out) {
    out.assign(static_cast<usize>(layout.dirTotal) * 4u, 0.0f);
    for (u32 i = 0; i < layout.cascades; ++i) {
        for (u32 k = 0; k < layout.dirs[i]; ++k) {
            const f64 angle = 2.0 * std::numbers::pi * (static_cast<f64>(k) + 0.5) / static_cast<f64>(layout.dirs[i]);
            f32* d = out.data() + (static_cast<usize>(layout.dirOffset[i]) + k) * 4u;
            d[0] = static_cast<f32>(std::cos(angle));
            d[1] = static_cast<f32>(std::sin(angle));
        }
    }
}

RcPush makeCascadePush(const RcLayout& layout, const RcSettings& settings, u32 cascade, u32 width, u32 height) {
    RcPush p{};
    p.width = width;
    p.height = height;
    p.probesX = layout.probesX[cascade];
    p.probesY = layout.probesY[cascade];
    p.dirCount = layout.dirs[cascade];
    p.spacing = layout.spacing[cascade];
    p.tStart = layout.tStart[cascade];
    p.tEnd = layout.tEnd[cascade];
    p.step = settings.step;
    p.invStep = 1.0f / settings.step;
    p.skyR = settings.sky[0];
    p.skyG = settings.sky[1];
    p.skyB = settings.sky[2];
    p.count = static_cast<u32>(layout.records[cascade]);
    p.branch = settings.branch;
    p.invBranch = 1.0f / static_cast<f32>(settings.branch);
    p.flags = settings.bilinearFix ? static_cast<u32>(kRcFlagBilinearFix) : 0u;
    if (cascade + 1u < layout.cascades) {
        p.flags |= kRcFlagHasUpper;
        p.upperProbesX = layout.probesX[cascade + 1u];
        p.upperProbesY = layout.probesY[cascade + 1u];
        p.upperSpacing = layout.spacing[cascade + 1u];
    } else {
        p.upperSpacing = layout.spacing[cascade] * 2.0f;
    }
    return p;
}

RcPush makeGatherPush(const RcLayout& layout, const RcSettings& settings, u32 width, u32 height) {
    RcPush p{};
    p.width = width;
    p.height = height;
    p.probesX = layout.probesX[0];
    p.probesY = layout.probesY[0];
    p.dirCount = layout.dirs[0];
    p.spacing = layout.spacing[0];
    p.step = settings.step;
    p.invStep = 1.0f / settings.step;
    p.count = width * height;
    return p;
}

// --- kernels -----------------------------------------------------------------------------------------------
RcHit rc_trace(const f32* scene, u32 width, u32 height, f32 ax, f32 ay, f32 dx, f32 dy, f32 len, f32 step,
               f32 invStep, u64* samples) {
    RcHit hit{};
    const f32 n = std::ceil(len * invStep);
    const u32 count = n > 0.0f ? static_cast<u32>(n) : 0u;
    const f32 fw = static_cast<f32>(width);
    const f32 fh = static_cast<f32>(height);
    u32 taken = 0;
    for (u32 k = 0; k < count; ++k) {
        const f32 t = static_cast<f32>(k) * step;
        const f32 px = ax + dx * t;
        const f32 py = ay + dy * t;
        const f32 fx = std::floor(px);
        const f32 fy = std::floor(py);
        ++taken;
        if (fx < 0.0f || fy < 0.0f || fx >= fw || fy >= fh) {
            if ((fx < 0.0f && dx <= 0.0f) || (fx >= fw && dx >= 0.0f) || (fy < 0.0f && dy <= 0.0f) ||
                (fy >= fh && dy >= 0.0f)) {
                break;
            }
            continue;
        }
        const u64 i = static_cast<u64>(static_cast<u32>(fy)) * width + static_cast<u32>(fx);
        f32 v[4];
        load4(scene, i, v);
        if (v[3] > 0.5f) {
            hit.r = v[0];
            hit.g = v[1];
            hit.b = v[2];
            hit.t = 0.0f;
            break;
        }
    }
    if (samples != nullptr) {
        *samples += taken;
    }
    return hit;
}

void rc_cascade_record(const RcPush& p, const f32* scene, const f32* dirs, const f32* upper, f32* dst, u32 index,
                       u64* rays, u64* samples) {
    const u32 k = index % p.dirCount;
    const u32 probe = index / p.dirCount;
    const u32 probeX = probe % p.probesX;
    const u32 probeY = probe / p.probesX;
    const f32 px = (static_cast<f32>(probeX) + 0.5f) * p.spacing;
    const f32 py = (static_cast<f32>(probeY) + 0.5f) * p.spacing;
    f32 d[4];
    load4(dirs, k, d);
    const f32 dx = d[0];
    const f32 dy = d[1];
    const f32 ax = px + dx * p.tStart;
    const f32 ay = py + dy * p.tStart;
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    if ((p.flags & kRcFlagHasUpper) == 0u) {
        const RcHit h = rc_trace(scene, p.width, p.height, ax, ay, dx, dy, p.tEnd - p.tStart, p.step, p.invStep, samples);
        if (rays != nullptr) {
            ++*rays;
        }
        r = h.r + h.t * p.skyR;
        g = h.g + h.t * p.skyG;
        b = h.b + h.t * p.skyB;
    } else {
        const Bilinear bl = bilinear(px, py, p.upperSpacing, p.upperProbesX, p.upperProbesY);
        if ((p.flags & kRcFlagBilinearFix) == 0u) {
            const RcHit h =
                rc_trace(scene, p.width, p.height, ax, ay, dx, dy, p.tEnd - p.tStart, p.step, p.invStep, samples);
            if (rays != nullptr) {
                ++*rays;
            }
            f32 fr = 0.0f;
            f32 fg = 0.0f;
            f32 fb = 0.0f;
            for (u32 j = 0; j < 4u; ++j) {
                f32 c[3];
                childAverage(p, upper, bl.x[j & 1u], bl.y[j >> 1u], k, c);
                fr = fr + bl.w[j] * c[0];
                fg = fg + bl.w[j] * c[1];
                fb = fb + bl.w[j] * c[2];
            }
            r = h.r + h.t * fr;
            g = h.g + h.t * fg;
            b = h.b + h.t * fb;
        } else {
            for (u32 j = 0; j < 4u; ++j) {
                const u32 ux = bl.x[j & 1u];
                const u32 uy = bl.y[j >> 1u];
                const f32 qx = (static_cast<f32>(ux) + 0.5f) * p.upperSpacing;
                const f32 qy = (static_cast<f32>(uy) + 0.5f) * p.upperSpacing;
                const f32 bx = qx + dx * p.tEnd;
                const f32 by = qy + dy * p.tEnd;
                const f32 sx = bx - ax;
                const f32 sy = by - ay;
                const f32 l2 = sx * sx + sy * sy;
                RcHit h{};
                if (l2 > 1e-12f) {
                    const f32 len = std::sqrt(l2);
                    const f32 inv = 1.0f / len;
                    h = rc_trace(scene, p.width, p.height, ax, ay, sx * inv, sy * inv, len, p.step, p.invStep, samples);
                    if (rays != nullptr) {
                        ++*rays;
                    }
                }
                f32 c[3];
                childAverage(p, upper, ux, uy, k, c);
                r = r + bl.w[j] * (h.r + h.t * c[0]);
                g = g + bl.w[j] * (h.g + h.t * c[1]);
                b = b + bl.w[j] * (h.b + h.t * c[2]);
            }
        }
    }
    f32* o = dst + static_cast<u64>(index) * 4u;
    o[0] = r;
    o[1] = g;
    o[2] = b;
    o[3] = 0.0f;
}

void rc_gather_pixel(const RcPush& p, const f32* cascade0, f32* out, u32 index) {
    const u32 x = index % p.width;
    const u32 y = index / p.width;
    const f32 px = static_cast<f32>(x) + 0.5f;
    const f32 py = static_cast<f32>(y) + 0.5f;
    const Bilinear bl = bilinear(px, py, p.spacing, p.probesX, p.probesY);
    const f32 inv = 1.0f / static_cast<f32>(p.dirCount);
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    for (u32 j = 0; j < 4u; ++j) {
        const u64 base = (static_cast<u64>(bl.y[j >> 1u]) * p.probesX + bl.x[j & 1u]) * p.dirCount;
        f32 sr = 0.0f;
        f32 sg = 0.0f;
        f32 sb = 0.0f;
        for (u32 k = 0; k < p.dirCount; ++k) {
            f32 v[4];
            load4(cascade0, base + k, v);
            sr = sr + v[0];
            sg = sg + v[1];
            sb = sb + v[2];
        }
        r = r + bl.w[j] * (sr * inv);
        g = g + bl.w[j] * (sg * inv);
        b = b + bl.w[j] * (sb * inv);
    }
    f32* o = out + static_cast<u64>(index) * 4u;
    o[0] = r;
    o[1] = g;
    o[2] = b;
    o[3] = 1.0f;
}

// --- solver ------------------------------------------------------------------------------------------------
bool RcCpuSolver::solve(const RcScene& scene, const RcSettings& settings, std::vector<f32>& out, RcStats* stats) {
    if (!RcLayout::compute(settings, scene.width, scene.height, m_layout)) {
        return false;
    }
    buildDirectionTable(m_layout, m_dirs);
    for (std::vector<f32>& c : m_cascade) {
        if (c.size() < m_layout.maxRecords * 4u) {
            c.resize(static_cast<usize>(m_layout.maxRecords) * 4u);
        }
    }
    out.resize(static_cast<usize>(scene.width) * scene.height * 4u);
    if (stats != nullptr) {
        stats->reset();
    }
    const f32* upper = nullptr;
    for (u32 ci = m_layout.cascades; ci-- > 0u;) {
        const RcPush p = makeCascadePush(m_layout, settings, ci, scene.width, scene.height);
        f32* dst = m_cascade[ci & 1u].data();
        const f32* dirs = m_dirs.data() + static_cast<usize>(m_layout.dirOffset[ci]) * 4u;
        u64 rays = 0;
        u64 samples = 0;
        for (u32 i = 0; i < p.count; ++i) {
            rc_cascade_record(p, scene.texels.data(), dirs, upper, dst, i, stats != nullptr ? &rays : nullptr,
                              stats != nullptr ? &samples : nullptr);
        }
        if (stats != nullptr) {
            stats->raysPerCascade[ci] = rays;
            stats->samplesPerCascade[ci] = samples;
            stats->rays += rays;
            stats->samples += samples;
        }
        upper = dst;
    }
    const RcPush g = makeGatherPush(m_layout, settings, scene.width, scene.height);
    for (u32 i = 0; i < g.count; ++i) {
        rc_gather_pixel(g, upper, out.data(), i);
    }
    return true;
}

void bruteForce(const RcScene& scene, u32 rays, f32 step, const f32 (&sky)[3], std::vector<f32>& out, RcStats* stats) {
    out.assign(static_cast<usize>(scene.width) * scene.height * 4u, 0.0f);
    std::vector<f32> dirs(static_cast<usize>(rays) * 2u);
    for (u32 k = 0; k < rays; ++k) {
        const f64 angle = 2.0 * std::numbers::pi * (static_cast<f64>(k) + 0.5) / static_cast<f64>(rays);
        dirs[k * 2u] = static_cast<f32>(std::cos(angle));
        dirs[k * 2u + 1u] = static_cast<f32>(std::sin(angle));
    }
    const f32 len = 2.0f * static_cast<f32>(scene.width + scene.height);
    const f32 invStep = 1.0f / step;
    u64 samples = 0;
    for (u32 y = 0; y < scene.height; ++y) {
        for (u32 x = 0; x < scene.width; ++x) {
            const f32 ox = static_cast<f32>(x) + 0.5f;
            const f32 oy = static_cast<f32>(y) + 0.5f;
            f64 r = 0.0;
            f64 g = 0.0;
            f64 b = 0.0;
            for (u32 k = 0; k < rays; ++k) {
                const RcHit h = rc_trace(scene.texels.data(), scene.width, scene.height, ox, oy, dirs[k * 2u],
                                         dirs[k * 2u + 1u], len, step, invStep, stats != nullptr ? &samples : nullptr);
                r += h.r + h.t * sky[0];
                g += h.g + h.t * sky[1];
                b += h.b + h.t * sky[2];
            }
            f32* o = out.data() + (static_cast<usize>(y) * scene.width + x) * 4u;
            o[0] = static_cast<f32>(r / rays);
            o[1] = static_cast<f32>(g / rays);
            o[2] = static_cast<f32>(b / rays);
            o[3] = 1.0f;
        }
    }
    if (stats != nullptr) {
        stats->reset();
        stats->rays = static_cast<u64>(rays) * scene.width * scene.height;
        stats->samples = samples;
    }
}

// --- metrics -----------------------------------------------------------------------------------------------
RcErrorMetrics compareImages(const std::vector<f32>& test, const std::vector<f32>& ref, const std::vector<u8>& mask) {
    RcErrorMetrics m{};
    const usize n = std::min(test.size(), ref.size()) / 4u;
    f64 sumAbs = 0.0;
    f64 sumRef = 0.0;
    f64 sumSq = 0.0;
    f64 meanRef = 0.0;
    f64 meanTest = 0.0;
    u64 count = 0;
    for (usize i = 0; i < n; ++i) {
        if (!mask.empty() && mask[i] == 0u) {
            continue;
        }
        for (u32 c = 0; c < 3u; ++c) {
            const f64 a = test[i * 4u + c];
            const f64 b = ref[i * 4u + c];
            const f64 e = std::fabs(a - b);
            sumAbs += e;
            sumRef += std::fabs(b);
            sumSq += e * e;
            m.maxAbs = std::max(m.maxAbs, e);
        }
        meanRef += (static_cast<f64>(ref[i * 4u]) + ref[i * 4u + 1u] + ref[i * 4u + 2u]) / 3.0;
        meanTest += (static_cast<f64>(test[i * 4u]) + test[i * 4u + 1u] + test[i * 4u + 2u]) / 3.0;
        ++count;
    }
    if (count == 0u) {
        return m;
    }
    m.pixels = static_cast<u32>(count);
    m.meanRef = meanRef / static_cast<f64>(count);
    m.meanTest = meanTest / static_cast<f64>(count);
    m.relL1 = sumRef > 0.0 ? sumAbs / sumRef : sumAbs;
    const f64 meanAbsRef = sumRef / (3.0 * static_cast<f64>(count));
    const f64 rmse = std::sqrt(sumSq / (3.0 * static_cast<f64>(count)));
    m.relRmse = meanAbsRef > 0.0 ? rmse / meanAbsRef : rmse;
    return m;
}

std::vector<u8> freeSpaceMask(const RcScene& scene) {
    std::vector<u8> mask(static_cast<usize>(scene.width) * scene.height, 0u);
    for (u32 y = 0; y < scene.height; ++y) {
        for (u32 x = 0; x < scene.width; ++x) {
            mask[static_cast<usize>(y) * scene.width + x] = scene.opaque(x, y) ? 0u : 1u;
        }
    }
    return mask;
}

f64 maskedMean(const std::vector<f32>& image, const std::vector<u8>& mask) {
    f64 sum = 0.0;
    u64 count = 0;
    for (usize i = 0; i < mask.size() && i * 4u + 2u < image.size(); ++i) {
        if (mask[i] != 0u) {
            sum += (static_cast<f64>(image[i * 4u]) + image[i * 4u + 1u] + image[i * 4u + 2u]) / 3.0;
            ++count;
        }
    }
    return count != 0u ? sum / static_cast<f64>(count) : 0.0;
}

f64 ringNonUniformity(const std::vector<f32>& image, const RcScene& scene, f32 cx, f32 cy, u32 r0, u32 r1) {
    f64 total = 0.0;
    u32 radii = 0;
    for (u32 r = r0; r <= r1; ++r) {
        f64 sum = 0.0;
        f64 sumSq = 0.0;
        u32 count = 0;
        for (u32 y = 0; y < scene.height; ++y) {
            for (u32 x = 0; x < scene.width; ++x) {
                const f64 dx = static_cast<f64>(x) + 0.5 - cx;
                const f64 dy = static_cast<f64>(y) + 0.5 - cy;
                const f64 d = std::sqrt(dx * dx + dy * dy);
                if (std::fabs(d - static_cast<f64>(r)) > 0.5 || scene.opaque(x, y)) {
                    continue;
                }
                const usize i = static_cast<usize>(y) * scene.width + x;
                const f64 v = (static_cast<f64>(image[i * 4u]) + image[i * 4u + 1u] + image[i * 4u + 2u]) / 3.0;
                sum += v;
                sumSq += v * v;
                ++count;
            }
        }
        if (count < 8u || sum <= 0.0) {
            continue;
        }
        const f64 mean = sum / count;
        const f64 var = std::max(0.0, sumSq / count - mean * mean);
        total += std::sqrt(var) / mean;
        ++radii;
    }
    return radii != 0u ? total / radii : 0.0;
}

// --- scenes ------------------------------------------------------------------------------------------------
void sceneEmpty(RcScene& scene, u32 w, u32 h) { scene.resize(w, h); }

void sceneDisk(RcScene& scene, u32 w, u32 h, f32 radius) {
    scene.resize(w, h);
    scene.fillDisk(static_cast<f32>(w) * 0.5f, static_cast<f32>(h) * 0.5f, radius, 4.0f, 4.0f, 4.0f, true);
}

void sceneRooms(RcScene& scene, u32 w, u32 h, u32 variant) {
    scene.resize(w, h);
    const i32 W = static_cast<i32>(w);
    const i32 H = static_cast<i32>(h);
    // Outer walls (grey, non-emissive), a vertical and a horizontal partition with doorways.
    scene.fillRect(0, 0, W, 2, 0, 0, 0, true);
    scene.fillRect(0, H - 2, W, H, 0, 0, 0, true);
    scene.fillRect(0, 0, 2, H, 0, 0, 0, true);
    scene.fillRect(W - 2, 0, W, H, 0, 0, 0, true);
    const i32 mx = W * 5 / 9;
    const i32 my = H * 4 / 7;
    scene.fillRect(mx, 0, mx + 2, H * 2 / 7, 0, 0, 0, true);
    scene.fillRect(mx, H * 2 / 7 + H / 8, mx + 2, H, 0, 0, 0, true);
    scene.fillRect(0, my, W / 5, my + 2, 0, 0, 0, true);
    scene.fillRect(W / 5 + W / 8, my, mx, my + 2, 0, 0, 0, true);
    // Blockers.
    scene.fillRect(W / 5, H / 5, W / 5 + W / 10, H / 5 + H / 12, 0, 0, 0, true);
    scene.fillDisk(static_cast<f32>(W) * 0.78f, static_cast<f32>(H) * 0.72f, static_cast<f32>(W) * 0.06f, 0, 0, 0, true);
    // A warm area light (moves with the variant) and a small cool light.
    const i32 lx = W / 12 + static_cast<i32>(variant % 4u) * (W / 16);
    scene.fillRect(lx, H / 2 - H / 20, lx + W / 10, H / 2 - H / 20 + 2, 6.0f, 4.5f, 2.0f, true);
    scene.fillDisk(static_cast<f32>(W) * 0.8f, static_cast<f32>(H) * 0.25f, 1.6f, 1.0f, 3.0f, 8.0f, true);
    // A dim red strip on the lower-right wall.
    scene.fillRect(mx + 2, H - 4, W - 2, H - 2, 1.5f, 0.2f, 0.2f, true);
}

void sceneLeakBox(RcScene& scene, u32 w, u32 h, u32 wall, std::vector<u8>& inside) {
    scene.resize(w, h);
    const i32 W = static_cast<i32>(w);
    const i32 H = static_cast<i32>(h);
    const i32 t = static_cast<i32>(wall);
    const i32 x0 = W / 4;
    const i32 y0 = H / 4;
    const i32 x1 = W * 3 / 4;
    const i32 y1 = H * 3 / 4;
    scene.fillRect(x0, y0, x1, y0 + t, 0, 0, 0, true);
    scene.fillRect(x0, y1 - t, x1, y1, 0, 0, 0, true);
    scene.fillRect(x0, y0, x0 + t, y1, 0, 0, 0, true);
    scene.fillRect(x1 - t, y0, x1, y1, 0, 0, 0, true);
    // Bright emitter strip hugging the left wall's outside, 1 texel of free space in between.
    scene.fillRect(x0 - 3, y0, x0 - 1, y1, 20.0f, 20.0f, 20.0f, true);
    inside.assign(static_cast<usize>(w) * h, 0u);
    for (i32 y = y0 + t; y < y1 - t; ++y) {
        for (i32 x = x0 + t; x < x1 - t; ++x) {
            inside[static_cast<usize>(y) * w + static_cast<usize>(x)] = 1u;
        }
    }
}

} // namespace fuse::renderer::research::rc
