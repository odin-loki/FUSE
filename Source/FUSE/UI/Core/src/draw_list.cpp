#include "simd/box_blur.hpp"
#include <algorithm>
#include "fuse/ui/draw_list.hpp"

#include <cmath>
#include <cstring>

namespace fuse::ui {

void DrawList::rect(const Rect& r, Color fill, f32 radius) { roundedRect(r, fill, {radius, radius, radius, radius}); }

void DrawList::roundedRect(const Rect& r, Color fill, const std::array<f32, 4>& radii, Color border, f32 bw) {
    DrawCmd c;
    c.op = DrawOp::Rect;
    c.rect = r;
    c.fill = fill;
    c.border = border;
    c.borderWidth = bw;
    c.radius = radii;
    m_cmds.push_back(c);
}

void DrawList::gradientRect(const Rect& r, Color from, Color to, bool vertical, const std::array<f32, 4>& radii) {
    DrawCmd c;
    c.op = DrawOp::Rect;
    c.rect = r;
    c.fill = from;
    c.fill2 = to;
    c.gradient = vertical ? 1 : 2;
    c.radius = radii;
    m_cmds.push_back(c);
}

void DrawList::shadow(const Rect& r, Color col, f32 blur, f32 radius) {
    DrawCmd c;
    c.op = DrawOp::Shadow;
    c.rect = r;
    c.fill = col;
    c.blur = blur;
    c.radius = {radius, radius, radius, radius};
    m_cmds.push_back(c);
}

void DrawList::translate(Vec2 d) {
    for (DrawCmd& c : m_cmds) {
        c.rect.x += d.x;
        c.rect.y += d.y;
        if (c.op == DrawOp::Line) { c.rect.w += d.x; c.rect.h += d.y; }
    }
    for (PathEdge& e : m_edges) { e.x0 += d.x; e.y0 += d.y; e.x1 += d.x; e.y1 += d.y; }
    for (DrawCmd& c : m_cmds)
        if ((c.gradient & 15u) >= kGradientLinearStops) { // toUnit(p - d)
            auto& m = c.gradXf;
            m[4] -= m[0] * d.x + m[2] * d.y;
            m[5] -= m[1] * d.x + m[3] * d.y;
        }
}

void DrawList::backdrop(const Rect& r, f32 blur, const std::array<f32, 4>& radii, Color tint) {
    DrawCmd c;
    c.op = DrawOp::Backdrop;
    c.rect = r;
    c.blur = blur;
    c.radius = radii;
    c.fill = tint;
    m_cmds.push_back(c);
}

void DrawList::line(Vec2 a, Vec2 b, Color col, f32 width) {
    DrawCmd c;
    c.op = DrawOp::Line;
    c.rect = {a.x, a.y, b.x, b.y};
    c.fill = col;
    c.borderWidth = width;
    m_cmds.push_back(c);
}

void scaleDrawList(const DrawList& in, f32 s, DrawList& out) {
    out.clear();
    out.reserve(in.size());
    for (DrawCmd c : in.cmds()) {
        c.rect = {c.rect.x * s, c.rect.y * s, c.rect.w * s, c.rect.h * s};
        for (f32& r : c.radius) r *= s;
        c.borderWidth *= s;
        c.blur *= s;
        if (c.op == DrawOp::Glyph) c.rect = {std::round(c.rect.x), std::round(c.rect.y), f32(c.tw), f32(c.th)};
        if ((c.gradient & 15u) >= kGradientLinearStops) for (int i = 0; i < 4; ++i) c.gradXf[std::size_t(i)] /= s; // toUnit(p / s)
        out.push(c);
    }
    out.m_stops = in.m_stops;
    out.m_edges = in.m_edges;
    for (PathEdge& e : out.m_edges) { e.x0 *= s; e.y0 *= s; e.x1 *= s; e.y1 *= s; }
}
void DrawList::pushClip(const Rect& r, f32 radius) {
    DrawCmd c;
    c.op = DrawOp::PushClip;
    c.rect = r;
    c.radius = {radius, radius, radius, radius};
    m_cmds.push_back(c);
}

void DrawList::popClip() {
    DrawCmd c;
    c.op = DrawOp::PopClip;
    m_cmds.push_back(c);
}

namespace {
inline u64 fnv(u64 h, const void* p, std::size_t n) {
    const auto* b = static_cast<const u8*>(p);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
} // namespace

u64 DrawList::hash() const {
    u64 h = 1469598103934665603ull;
    for (const auto& c : m_cmds) {
        h = fnv(h, &c.op, 1);
        h = fnv(h, &c.gradient, 1);
        h = fnv(h, &c.u, 8);
        h = fnv(h, &c.image, 4);
        h = fnv(h, &c.rect, sizeof(Rect));
        h = fnv(h, &c.fill, sizeof(Color) * 3);
        h = fnv(h, &c.borderWidth, 4);
        h = fnv(h, c.radius.data(), 16);
        h = fnv(h, &c.blur, 4);
        if ((c.gradient & 15u) >= kGradientLinearStops) {
            h = fnv(h, c.gradXf.data(), 24);
            h = fnv(h, &c.stopCount, 2);
            if (std::size_t(c.stops) + c.stopCount <= m_stops.size()) h = fnv(h, m_stops.data() + c.stops, std::size_t(c.stopCount) * sizeof(GradientStop));
        }
    }
    if (!m_edges.empty()) h = fnv(h, m_edges.data(), m_edges.size() * sizeof(PathEdge));
    return h;
}

u64 Image::hash() const { return fnv(1469598103934665603ull, pixels.data(), pixels.size() * 4); }

u32 packPremultiplied(Color c) {
    auto q = [](f32 v) { return u32(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
    const f32 a = std::clamp(c.a, 0.f, 1.f);
    return q(c.r * a) | (q(c.g * a) << 8) | (q(c.b * a) << 16) | (q(a) << 24);
}

u32 CpuRasterizer::addImage(const Image* img) {
    m_images.push_back(img);
    return u32(m_images.size() - 1);
}

namespace {

struct Px {
    f32 r, g, b, a; // premultiplied 0..1
};

inline void blend(u32& dst, Px s) {
    if (s.a <= 0.f && s.r <= 0.f && s.g <= 0.f && s.b <= 0.f) return;
    auto q = [](f32 v) { return u32(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); }; // == lround for v in [0, 1]
    if (s.a >= 1.f) { // opaque: the destination does not contribute
        dst = q(s.r) | (q(s.g) << 8) | (q(s.b) << 16) | (q(s.a) << 24);
        return;
    }
    const f32 inv = 1.f - s.a;
    const f32 dr = f32(dst & 0xFF) / 255.f, dg = f32((dst >> 8) & 0xFF) / 255.f, db = f32((dst >> 16) & 0xFF) / 255.f,
              da = f32(dst >> 24) / 255.f;
    dst = q(s.r + dr * inv) | (q(s.g + dg * inv) << 8) | (q(s.b + db * inv) << 16) | (q(s.a + da * inv) << 24);
}

inline Px premul(Color c, f32 cov) {
    const f32 a = std::clamp(c.a, 0.f, 1.f) * cov;
    return {c.r * a, c.g * a, c.b * a, a};
}

/// Gradient parameter of (px, py) across `r` (GradientKind); the GPU kernel has the same arithmetic.
inline f32 gradientT(u8 kind, f32 px, f32 py, const Rect& r) {
    if (kind == kGradientVertical) return std::clamp((py - r.y) / std::max(r.h, 1.f), 0.f, 1.f);
    if (kind == kGradientHorizontal) return std::clamp((px - r.x) / std::max(r.w, 1.f), 0.f, 1.f);
    const f32 dx = px - (r.x + r.w * 0.5f), dy = py - (r.y + r.h * 0.5f);
    const f32 rad = std::max(std::max(r.w, r.h) * 0.5f, 1.f);
    return std::clamp(std::sqrt(dx * dx + dy * dy) / rad, 0.f, 1.f);
}
inline Color lerpColor(Color a, Color b, f32 t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
/// DrawOp::Path at a pixel: fill coverage from the winding rule at 4x4 sub-samples (exact for overlapping contours and
/// holes), stroke distance to the nearest stroked edge. The GPU kernel runs the same loop.
} // namespace
f32 pathFillCoverage(const PathEdge* e, std::size_t n, f32 px, f32 py, bool evenOdd) {
    int wind[16] = {};
    for (std::size_t i = 0; i < n; ++i) {
        const f32 ax = e[i].x0, ay = e[i].y0, bx = e[i].x1, by = e[i].y1;
        if (ay == by) continue;
        const f32 vx = bx - ax, vy = by - ay;
        const int dir = by > ay ? 1 : -1;
        const f32 lo = std::min(ay, by), hi = std::max(ay, by);
        for (int j = 0; j < 4; ++j) {
            const f32 sy = py - 0.5f + (f32(j) + 0.5f) * 0.25f;
            if (sy < lo || sy >= hi) continue;
            const f32 xi = ax + (sy - ay) * vx / vy;
            for (int k = 0; k < 4; ++k)
                if (xi > px - 0.5f + (f32(k) + 0.5f) * 0.25f) wind[j * 4 + k] += dir;
        }
    }
    int in = 0;
    for (int w : wind) in += evenOdd ? (w & 1) : (w != 0);
    return f32(in) / 16.f;
}
f32 clipEntryCoverage(const PathEdge* e, std::size_t n, f32 px, f32 py, bool evenOdd) {
    if (n == 0 || e[0].fillOnly >= 0.f) return pathFillCoverage(e, n, px, py, evenOdd);
    f32 m = 0.f;
    for (std::size_t i = 0; i < n;) {
        const PathEdge& h = e[i];
        const int g = int(-1.f - h.fillOnly);
        const std::size_t cnt = std::size_t(h.x1);
        std::size_t at = i + 1 + (g ? 2u : 0u);
        if (at + cnt > n) break;
        const f32 cov = pathFillCoverage(e + at, cnt, px, py, h.y0 != 0.f);
        if (cov > 0.f) {
            f32 v = h.x0, a = h.y1;
            if (g) {
                const PathEdge& p0 = e[i + 1];
                const PathEdge& p1 = e[i + 2];
                const f32 ux = p0.x0 * px + p0.x1 * py + p1.x0, uy = p0.y0 * px + p0.y1 * py + p1.y0;
                const f32 t = std::clamp(g == 1 ? ux : std::sqrt(ux * ux + uy * uy), 0.f, 1.f);
                v = v + (p1.x1 - v) * t, a = a + (p1.y1 - a) * t;
            }
            m = v * cov + m * (1.f - a * cov);
        }
        i = at + cnt;
    }
    return std::clamp(m, 0.f, 1.f);
}
namespace {
inline void pathSample(const PathEdge* e, std::size_t n, f32 px, f32 py, bool evenOdd, f32& fillCov, f32& dStroke) {
    int wind[16] = {};
    f32 s2 = 3.0e38f;
    for (std::size_t i = 0; i < n; ++i) {
        const f32 ax = e[i].x0, ay = e[i].y0, bx = e[i].x1, by = e[i].y1;
        const f32 vx = bx - ax, vy = by - ay;
        if (e[i].fillOnly == 0.f) {
            const f32 len2 = std::max(vx * vx + vy * vy, 1e-12f);
            const f32 t = std::clamp(((px - ax) * vx + (py - ay) * vy) / len2, 0.f, 1.f);
            const f32 ex = px - (ax + vx * t), ey = py - (ay + vy * t);
            s2 = std::min(s2, ex * ex + ey * ey);
        }
        if (ay == by) continue;
        const int dir = by > ay ? 1 : -1;
        const f32 lo = std::min(ay, by), hi = std::max(ay, by);
        for (int j = 0; j < 4; ++j) {
            const f32 sy = py - 0.5f + (f32(j) + 0.5f) * 0.25f;
            if (sy < lo || sy >= hi) continue;
            const f32 xi = ax + (sy - ay) * vx / vy;
            for (int k = 0; k < 4; ++k)
                if (xi > px - 0.5f + (f32(k) + 0.5f) * 0.25f) wind[j * 4 + k] += dir;
        }
    }
    int in = 0;
    for (int w : wind) in += evenOdd ? (w & 1) : (w != 0);
    fillCov = f32(in) / 16.f;
    dStroke = std::sqrt(s2);
}

/// Signed distance from p to a rounded rect (negative inside).
inline f32 sdRoundRect(f32 px, f32 py, const Rect& r, const std::array<f32, 4>& rad) {
    const f32 cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    const f32 hx = r.w * 0.5f, hy = r.h * 0.5f;
    const f32 dx = px - cx, dy = py - cy;
    // corner by quadrant: tl, tr, br, bl
    f32 rr = dx < 0 ? (dy < 0 ? rad[0] : rad[3]) : (dy < 0 ? rad[1] : rad[2]);
    rr = std::min(rr, std::min(hx, hy));
    const f32 qx = std::abs(dx) - hx + rr, qy = std::abs(dy) - hy + rr;
    const f32 ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - rr;
}

struct Clip {
    Rect r;
    f32 radius;
    u8 paths = 0; ///< active clip paths (ClipPath) up to this level
};
struct ClipPathRef {
    u32 at, n;
    bool evenOdd;
};

} // namespace

namespace {
// Backdrop blur, the reference: three box passes per axis (~Gaussian, sigma ~ blur / 2) over the pixels under the
// rounded rect, sampled from a margin of `blur` around it (clamped to the target), then mixed in by coverage.
void backdropBlur(const DrawCmd& c, const Rect& clipR, f32 clipRadius, Image& target) {
    const Rect area = c.rect.intersect(clipR).intersect({0.f, 0.f, f32(target.width), f32(target.height)});
    if (area.empty()) return;
    const int r = std::max(1, int(std::lround(c.blur / 3.f)));
    const int x0 = int(std::floor(area.x)), y0 = int(std::floor(area.y));
    const int x1 = int(std::ceil(area.right())), y1 = int(std::ceil(area.bottom()));
    const int m = 3 * r;
    const int sx0 = std::max(0, x0 - m), sy0 = std::max(0, y0 - m);
    const int sx1 = std::min(target.width, x1 + m), sy1 = std::min(target.height, y1 + m);
    const int w = sx1 - sx0, h = sy1 - sy0;
    std::vector<f32> buf(std::size_t(w) * std::size_t(h) * 4), tmp(buf.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const u32 p = target.pixels[std::size_t(sy0 + y) * std::size_t(target.width) + std::size_t(sx0 + x)];
            for (int k = 0; k < 4; ++k) buf[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4 + std::size_t(k)] = f32((p >> (8 * k)) & 0xFFu);
        }
    std::vector<f32> scratch(buf.size());
    auto pass = [&](bool) { simd::boxHorizontal(buf.data(), tmp.data(), scratch.data(), w, h, r, 1.f / f32(2 * r + 1)); };
    auto vertical = [&] { // rows are contiguous: one FORGE-lane vector sweep over all columns x channels
        simd::boxVertical(buf.data(), tmp.data(), w * 4, h, r, 1.f / f32(2 * r + 1));
        buf.swap(tmp);
    };
    (void)pass;
    simd::boxHorizontalN(buf.data(), tmp.data(), scratch.data(), w, h, r, 1.f / f32(2 * r + 1), 3);
    for (int it = 0; it < 3; ++it) vertical();
    const std::array<f32, 4> clipRad{clipRadius, clipRadius, clipRadius, clipRadius};
    const Px tint = premul(c.fill, 1.f);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const f32 px = f32(x) + 0.5f, py = f32(y) + 0.5f;
            f32 cov = std::clamp(0.5f - sdRoundRect(px, py, c.rect, c.radius), 0.f, 1.f);
            if (clipRadius > 0.f) cov *= std::clamp(0.5f - sdRoundRect(px, py, clipR, clipRad), 0.f, 1.f);
            if (cov <= 0.f) continue;
            u32& d = target.pixels[std::size_t(y) * std::size_t(target.width) + std::size_t(x)];
            const f32* b = &buf[(std::size_t(y - sy0) * std::size_t(w) + std::size_t(x - sx0)) * 4];
            u32 o = 0;
            for (int k = 0; k < 4; ++k) {
                const f32 dv = f32((d >> (8 * k)) & 0xFFu);
                o |= u32(std::clamp(std::lround(dv + (b[k] - dv) * cov), 0l, 255l)) << (8 * k);
            }
            d = o;
            if (c.fill.a > 0.f) blend(d, premul(c.fill, cov));
        }
    (void)tint;
}
} // namespace

void CpuRasterizer::render(const DrawList& list, Image& target, Rect scissor) const {
    if (target.width <= 0 || target.height <= 0) return;
    Rect full{0.f, 0.f, f32(target.width), f32(target.height)};
    if (scissor.empty()) scissor = full;
    scissor = scissor.intersect(full);
    Clip stack[32];
    int depth = 0;
    std::vector<PathEdge> rowEdges;
    ClipPathRef clipPaths[32];
    stack[0] = {scissor, 0.f, 0};
    for (const DrawCmd& c : list.cmds()) {
        const Clip& clip = stack[depth];
        switch (c.op) {
        case DrawOp::PushClip:
            if (depth < 31) {
                ++depth;
                stack[depth] = {stack[depth - 1].r.intersect(c.rect), c.radius[0], stack[depth - 1].paths};
                if (stack[depth - 1].radius > 0.f && c.radius[0] <= 0.f) stack[depth].radius = 0.f;
            }
            continue;
        case DrawOp::ClipPath:
            if (depth < 31) {
                ++depth;
                stack[depth] = stack[depth - 1];
                const std::size_t n = std::size_t(c.tw) | (std::size_t(c.th) << 16);
                const bool ok = std::size_t(c.image) + n <= list.pathEdges().size();
                if (!ok || n == 0) stack[depth].r = {};
                else {
                    stack[depth].r = stack[depth].r.intersect({c.rect.x - 1.f, c.rect.y - 1.f, c.rect.w + 2.f, c.rect.h + 2.f});
                    clipPaths[stack[depth].paths++] = {c.image, u32(n), c.v != 0};
                }
            }
            continue;
        case DrawOp::PopClip:
            if (depth > 0) --depth;
            continue;
        case DrawOp::Backdrop: backdropBlur(c, clip.r, clip.radius, target); continue;
        default: break;
        }
        Rect bounds = c.rect;
        if (c.op == DrawOp::Shadow) bounds = {c.rect.x - c.blur * 2.f, c.rect.y - c.blur * 2.f, c.rect.w + c.blur * 4.f, c.rect.h + c.blur * 4.f};
        if (c.op == DrawOp::Line) {
            const f32 hw = c.borderWidth * 0.5f + 1.f;
            bounds = {std::min(c.rect.x, c.rect.w) - hw, std::min(c.rect.y, c.rect.h) - hw,
                      std::abs(c.rect.w - c.rect.x) + 2 * hw, std::abs(c.rect.h - c.rect.y) + 2 * hw};
        }
        const Rect area = bounds.intersect(clip.r);
        if (area.empty()) continue;
        const int x0 = int(std::floor(area.x)), y0 = int(std::floor(area.y));
        const int x1 = int(std::ceil(area.right())), y1 = int(std::ceil(area.bottom()));
        if (c.op == DrawOp::Glyph && clip.radius <= 0.f && clip.paths == 0) {
            // glyph fast path (text-heavy frames are thousands of small quads): straight atlas rows, no per-pixel clip
            // or op dispatch; identical arithmetic to the generic Glyph case (clipCov == 1)
            if (!m_atlas) continue;
            const int ox = int(c.rect.x), oy = int(c.rect.y);
            const int gx0 = std::max(x0, ox), gx1 = std::min(x1, ox + int(c.tw));
            const int gy0 = std::max(y0, oy), gy1 = std::min(y1, oy + int(c.th));
            for (int y = gy0; y < gy1; ++y) {
                u32* row = target.pixels.data() + std::size_t(y) * std::size_t(target.width);
                const u8* src = m_atlas + std::size_t(c.v + (y - oy)) * std::size_t(m_atlasSize) + std::size_t(c.u);
                for (int x = gx0; x < gx1; ++x)
                    if (const u8 a = src[x - ox]) blend(row[x], premul(c.fill, f32(a) / 255.f * 1.f));
            }
            continue;
        }
        const Rect clipOuter = clip.r; // the rounded clip uses the top clip rect's own shape
        const std::array<f32, 4> clipRad{clip.radius, clip.radius, clip.radius, clip.radius};
        // fast path: the interior of an opaque, ungraded rect (pixel centres at least max radius + border + 0.5 inside
        // every edge have full coverage) is a plain store of the packed fill (same bytes blend() would produce)
        int ix0 = 0, ix1 = 0, iy0 = 0, iy1 = 0;
        u32 solid = 0;
        if (c.op == DrawOp::Rect && !c.gradient && c.fill.a >= 1.f && clip.radius <= 0.f && clip.paths == 0) {
            const f32 m = std::max({c.radius[0], c.radius[1], c.radius[2], c.radius[3], 0.f}) +
                          (c.borderWidth > 0.f && c.border.a > 0.f ? c.borderWidth : 0.f) + 0.5f;
            ix0 = int(std::ceil(c.rect.x + m - 0.5f)), ix1 = int(std::floor(c.rect.right() - m - 0.5f)) + 1;
            iy0 = int(std::ceil(c.rect.y + m - 0.5f)), iy1 = int(std::floor(c.rect.bottom() - m - 0.5f)) + 1;
            u32 probe = 0;
            blend(probe, premul(c.fill, 1.f));
            solid = probe;
        }
        for (int y = y0; y < y1; ++y) {
            u32* row = target.pixels.data() + std::size_t(y) * std::size_t(target.width);
            if (c.op == DrawOp::Path) {
                // scanline edge list: only edges that can affect this row's winding sub-samples, or whose stroke
                // distance can reach it (farther stroke edges cannot change a non-zero coverage), so bit-exact
                rowEdges.clear();
                const std::size_t n = std::size_t(c.tw) | (std::size_t(c.th) << 16);
                if (std::size_t(c.image) + n <= list.pathEdges().size()) {
                    const PathEdge* e = list.pathEdges().data() + c.image;
                    const f32 py = f32(y) + 0.5f, reach = c.borderWidth * 0.5f + 1.f;
                    for (std::size_t i = 0; i < n; ++i) {
                        const f32 lo = std::min(e[i].y0, e[i].y1), hi = std::max(e[i].y0, e[i].y1);
                        const bool wind = lo != hi && hi > py - 0.5f && lo <= py + 0.5f;
                        const bool str = e[i].fillOnly == 0.f && c.borderWidth > 0.f && hi >= py - reach && lo <= py + reach;
                        if (wind || str) {
                            PathEdge q = e[i];
                            if (!str) q.fillOnly = 1.f;
                            rowEdges.push_back(q);
                        }
                    }
                }
                if (rowEdges.empty()) continue;
            }
            const bool solidRow = y >= iy0 && y < iy1;
            const int sa = std::max(x0, ix0), sb = std::min(x1, ix1);
            if (solidRow && sa < sb) simd::fillSpan(row + sa, sb - sa, solid);
            for (int x = x0; x < x1; ++x) {
                if (solidRow && x >= ix0 && x < ix1) {
                    x = ix1 - 1; // span stored above
                    continue;
                }
                const f32 px = f32(x) + 0.5f, py = f32(y) + 0.5f;
                f32 clipCov = 1.f;
                if (clip.radius > 0.f) clipCov = std::clamp(0.5f - sdRoundRect(px, py, clipOuter, clipRad), 0.f, 1.f);
                for (u8 q = 0; q < clip.paths && clipCov > 0.f; ++q)
                    clipCov *= clipEntryCoverage(list.pathEdges().data() + clipPaths[q].at, clipPaths[q].n, px, py, clipPaths[q].evenOdd);
                if (clipCov <= 0.f) continue;
                switch (c.op) {
                case DrawOp::Rect: {
                    const f32 d = sdRoundRect(px, py, c.rect, c.radius);
                    const f32 outer = std::clamp(0.5f - d, 0.f, 1.f) * clipCov;
                    if (outer <= 0.f) break;
                    Color fill = c.fill;
                    if (c.gradient >= kGradientLinearStops && c.stopCount && std::size_t(c.stops) + c.stopCount <= list.gradientStops().size())
                        fill = gradientStopsColor(c, list.gradientStops().data() + c.stops, px, py);
                    else if (c.gradient && c.gradient < kGradientLinearStops) fill = lerpColor(fill, c.fill2, gradientT(c.gradient, px, py, c.rect));
                    if (c.borderWidth > 0.f && c.border.a > 0.f) {
                        const f32 inner = std::clamp(0.5f - (d + c.borderWidth), 0.f, 1.f) * clipCov;
                        const Px pf = premul(fill, inner);
                        const Px pb = premul(c.border, outer - inner);
                        blend(row[x], {pf.r + pb.r, pf.g + pb.g, pf.b + pb.b, pf.a + pb.a});
                    } else {
                        blend(row[x], premul(fill, outer));
                    }
                    break;
                }
                case DrawOp::Shadow: {
                    const f32 d = sdRoundRect(px, py, c.rect, c.radius);
                    const f32 b = std::max(c.blur, 0.5f);
                    const f32 t = std::clamp((d + b) / (2.f * b), 0.f, 1.f);
                    const f32 cov = (1.f - t * t * (3.f - 2.f * t)) * clipCov;
                    blend(row[x], premul(c.fill, cov));
                    break;
                }
                case DrawOp::Glyph: {
                    if (!m_atlas) break;
                    const int gx = x - int(c.rect.x), gy = y - int(c.rect.y);
                    if (gx < 0 || gy < 0 || gx >= c.tw || gy >= c.th) break;
                    const u8 a = m_atlas[std::size_t(c.v + gy) * std::size_t(m_atlasSize) + std::size_t(c.u + gx)];
                    if (a) blend(row[x], premul(c.fill, f32(a) / 255.f * clipCov));
                    break;
                }
                case DrawOp::Image: {
                    if (c.image >= m_images.size() || !m_images[c.image]) break;
                    const Image& img = *m_images[c.image];
                    const int sx = std::clamp(int((px - c.rect.x) / c.rect.w * f32(img.width)), 0, img.width - 1);
                    const int sy = std::clamp(int((py - c.rect.y) / c.rect.h * f32(img.height)), 0, img.height - 1);
                    const u32 s = img.at(sx, sy);
                    const f32 k = clipCov * (c.fill.a > 0.f ? c.fill.a : 1.f);
                    if (k >= 1.f && (s >> 24) == 255u) { row[x] = s; break; } // opaque texel: blend() would store it
                    blend(row[x], {f32(s & 0xFF) / 255.f * k, f32((s >> 8) & 0xFF) / 255.f * k,
                                   f32((s >> 16) & 0xFF) / 255.f * k, f32(s >> 24) / 255.f * k});
                    break;
                }
                case DrawOp::Line: {
                    const f32 ax = c.rect.x, ay = c.rect.y, bx = c.rect.w, by = c.rect.h;
                    const f32 vx = bx - ax, vy = by - ay;
                    const f32 len2 = std::max(vx * vx + vy * vy, 1e-6f);
                    const f32 t = std::clamp(((px - ax) * vx + (py - ay) * vy) / len2, 0.f, 1.f);
                    const f32 ex = px - (ax + vx * t), ey = py - (ay + vy * t);
                    const f32 d = std::sqrt(ex * ex + ey * ey) - c.borderWidth * 0.5f;
                    const f32 cov = std::clamp(0.5f - d, 0.f, 1.f) * clipCov;
                    if (cov > 0.f) blend(row[x], premul(c.fill, cov));
                    break;
                }
                case DrawOp::Path: {
                    f32 fc = 0.f, ds = 0.f;
                    pathSample(rowEdges.data(), rowEdges.size(), px, py, c.v != 0, fc, ds);
                    fc *= clipCov;
                    Color fill = c.fill;
                    if (c.gradient >= kGradientLinearStops && c.stopCount && std::size_t(c.stops) + c.stopCount <= list.gradientStops().size())
                        fill = gradientStopsColor(c, list.gradientStops().data() + c.stops, px, py);
                    else if (c.gradient && c.gradient < kGradientLinearStops) fill = lerpColor(fill, c.fill2, gradientT(c.gradient, px, py, c.rect));
                    Px o = premul(fill, fill.a > 0.f || c.gradient ? fc : 0.f);
                    if (c.borderWidth > 0.f) {
                        const f32 sc = std::clamp(0.5f - (ds - c.borderWidth * 0.5f), 0.f, 1.f) * clipCov;
                        // stroke over fill (source-over within the command)
                        const Px pb = premul(c.border, sc);
                        o = {pb.r + o.r * (1.f - pb.a), pb.g + o.g * (1.f - pb.a), pb.b + o.b * (1.f - pb.a), pb.a + o.a * (1.f - pb.a)};
                    }
                    blend(row[x], o);
                    break;
                }
                default: break;
                }
            }
        }
    }
}

} // namespace fuse::ui
