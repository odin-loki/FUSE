#include "simd/box_blur.hpp"
#include <algorithm>
#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/color_space.hpp"

#include <cmath>
#include <cstring>
#include <type_traits>

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

namespace {
/// A transform layer's pixel -> local map `g` after both spaces go through q = p * s + o: g' = S g S^-1.
void conjugateLayer(std::array<f32, 6>& g, Vec2 s, Vec2 o) {
    if (g[0] == 0.f && g[1] == 0.f && g[2] == 0.f && g[3] == 0.f) return; // singular marker
    // local' = s * (A ((p - o) / s) + t) + o
    const f32 A0 = g[0], A1 = g[1], A2 = g[2], A3 = g[3];
    const f32 ix = o.x / s.x, iy = o.y / s.y;
    const f32 tx = g[4] - (A0 * ix + A2 * iy), ty = g[5] - (A1 * ix + A3 * iy);
    g[0] = A0;
    g[1] = A1 * s.y / s.x;
    g[2] = A2 * s.x / s.y;
    g[3] = A3;
    g[4] = s.x * tx + o.x;
    g[5] = s.y * ty + o.y;
}
} // namespace

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
        } else if (isTransformLayer(c)) conjugateLayer(c.gradXf, {1.f, 1.f}, d);
}

void DrawList::transformRange(std::size_t fromCmd, std::size_t fromEdge, Vec2 sc, Vec2 o) {
    const f32 m = std::min(std::fabs(sc.x), std::fabs(sc.y));
    auto px = [&](f32 x) { return x * sc.x + o.x; };
    auto py = [&](f32 y) { return y * sc.y + o.y; };
    for (std::size_t i = fromCmd; i < m_cmds.size(); ++i) {
        DrawCmd& c = m_cmds[i];
        if (c.op == DrawOp::Line) {
            c.rect = {px(c.rect.x), py(c.rect.y), px(c.rect.w), py(c.rect.h)};
            c.borderWidth *= m;
            continue;
        }
        Rect r{px(c.rect.x), py(c.rect.y), c.rect.w * sc.x, c.rect.h * sc.y};
        if (r.w < 0.f) r.x += r.w, r.w = -r.w; // mirrored: keep a positive extent
        if (r.h < 0.f) r.y += r.h, r.h = -r.h;
        if (c.op == DrawOp::Glyph) r = {std::round(r.x), std::round(r.y), c.rect.w, c.rect.h};
        c.rect = r;
        for (f32& rr : c.radius) rr *= m;
        c.borderWidth *= m;
        c.blur *= m;
        if ((c.gradient & 15u) >= kGradientLinearStops) { // toUnit((p' - o) / s)
            auto& g = c.gradXf;
            g[0] /= sc.x, g[1] /= sc.x, g[2] /= sc.y, g[3] /= sc.y;
            g[4] -= g[0] * o.x + g[2] * o.y;
            g[5] -= g[1] * o.x + g[3] * o.y;
        } else if (isTransformLayer(c)) conjugateLayer(c.gradXf, sc, o);
    }
    for (std::size_t i = fromEdge; i < m_edges.size(); ++i) {
        PathEdge& e = m_edges[i];
        if (e.fillOnly < 0.f) { // pushClipLayers header (+ two gradient parameter edges): not coordinates
            if (e.fillOnly < -1.5f && i + 2 < m_edges.size()) {
                PathEdge& a = m_edges[i + 1];
                PathEdge& b = m_edges[i + 2];
                a.x0 /= sc.x, a.y0 /= sc.x, a.x1 /= sc.y, a.y1 /= sc.y; // toUnit((p' - o) / s)
                b.x0 -= a.x0 * o.x + a.x1 * o.y;
                b.y0 -= a.y0 * o.x + a.y1 * o.y;
                i += 2;
            }
            continue;
        }
        e.x0 = px(e.x0), e.y0 = py(e.y0), e.x1 = px(e.x1), e.y1 = py(e.y1);
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

void DrawList::putFilter(DrawCmd& c, const ColorMatrix& filter, const DropShadow& shadow) {
    c.stopCount = 0;
    if (filter.identity() && !shadow.visible()) return;
    c.stops = u32(m_stops.size());
    c.stopCount = shadow.visible() ? 6 : 4;
    for (int k = 0; k < 4; ++k) {
        const f32* row = filter.m.data() + k * 5;
        m_stops.push_back({row[4], Color{row[0], row[1], row[2], row[3]}});
    }
    if (shadow.visible()) {
        m_stops.push_back({shadow.blur, shadow.color});
        m_stops.push_back({0.f, Color{shadow.offset.x, shadow.offset.y, 0.f, 0.f}});
    }
}

void DrawList::backdrop(const Rect& r, f32 blur, const std::array<f32, 4>& radii, const ColorMatrix& filter, Color tint) {
    backdrop(r, blur, radii, filter, DropShadow{}, tint);
}

void DrawList::backdrop(const Rect& r, f32 blur, const std::array<f32, 4>& radii, const ColorMatrix& filter, const DropShadow& shadow,
                        Color tint) {
    backdrop(r, blur, radii, tint);
    putFilter(m_cmds.back(), filter, shadow);
}

namespace {
/// pixels a command can touch (layer ink bounds)
Rect inkBounds(const DrawCmd& c) {
    switch (c.op) {
    case DrawOp::PushClip: case DrawOp::PopClip: case DrawOp::ClipPath: case DrawOp::PopFilter: return {};
    case DrawOp::Shadow: return {c.rect.x - c.blur * 2.f, c.rect.y - c.blur * 2.f, c.rect.w + c.blur * 4.f, c.rect.h + c.blur * 4.f};
    case DrawOp::Line: {
        const f32 hw = c.borderWidth * 0.5f + 1.f;
        const f32 x0 = std::min(c.rect.x, c.rect.w), x1 = std::max(c.rect.x, c.rect.w);
        const f32 y0 = std::min(c.rect.y, c.rect.h), y1 = std::max(c.rect.y, c.rect.h);
        return {x0 - hw, y0 - hw, x1 - x0 + 2.f * hw, y1 - y0 + 2.f * hw};
    }
    case DrawOp::Path: {
        const f32 e = c.borderWidth + 1.f;
        return {c.rect.x - e, c.rect.y - e, c.rect.w + 2.f * e, c.rect.h + 2.f * e};
    }
    default: return {c.rect.x - 1.f, c.rect.y - 1.f, c.rect.w + 2.f, c.rect.h + 2.f};
    }
}
Rect unite(const Rect& a, const Rect& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const f32 x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    return {x0, y0, std::max(a.right(), b.right()) - x0, std::max(a.bottom(), b.bottom()) - y0};
}
} // namespace

void matchFilters(const DrawList& list, std::vector<u32>& partner) {
    const auto& cmds = list.cmds();
    partner.assign(cmds.size(), ~0u);
    std::vector<u32> open;
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        if (cmds[i].op == DrawOp::PushFilter) open.push_back(u32(i));
        else if (cmds[i].op == DrawOp::PopFilter && !open.empty()) {
            partner[i] = open.back();
            partner[open.back()] = u32(i);
            open.pop_back();
        }
    }
}

void DrawList::pushFilter(const ColorMatrix& matrix, f32 blur, const DropShadow& shadow, bool chained) {
    DrawCmd c;
    c.op = DrawOp::PushFilter;
    c.blur = std::max(0.f, blur);
    c.v = chained ? 1 : 0;
    putFilter(c, matrix, shadow);
    m_filterStack.push_back(u32(m_cmds.size()));
    m_cmds.push_back(c);
}

bool isFilterProgram(const DrawCmd& c) { return c.u == 1 && c.stopCount >= 9; }

void DrawList::pushTransform(const std::array<f32, 6>& m) {
    DrawCmd c;
    c.op = DrawOp::PushFilter;
    c.u = 2;
    const double det = double(m[0]) * double(m[3]) - double(m[1]) * double(m[2]);
    if (std::isfinite(det) && std::fabs(det) > 1e-12) { // inverse: local = A^-1 (p - t)
        const double i0 = double(m[3]) / det, i1 = -double(m[1]) / det, i2 = -double(m[2]) / det, i3 = double(m[0]) / det;
        c.gradXf = {f32(i0), f32(i1), f32(i2), f32(i3), f32(-(i0 * double(m[4]) + i2 * double(m[5]))), f32(-(i1 * double(m[4]) + i3 * double(m[5])))};
    } // else all zero: singular, dropped at popFilter
    m_filterStack.push_back(u32(m_cmds.size()));
    m_cmds.push_back(c);
}

void sampleLayerBilinear(const f32* buf, int w, int h, f32 fx, f32 fy, f32 out[4]) {
    const f32 sx = std::clamp(fx - 0.5f, -2.f, f32(w) + 1.f), sy = std::clamp(fy - 0.5f, -2.f, f32(h) + 1.f);
    const f32 flx = std::floor(sx), fly = std::floor(sy);
    const int x0 = int(flx), y0 = int(fly);
    const f32 tx = sx - flx, ty = sy - fly;
    const f32 zero[4] = {0.f, 0.f, 0.f, 0.f};
    auto at = [&](int x, int y) -> const f32* {
        return x >= 0 && y >= 0 && x < w && y < h ? buf + (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4 : zero;
    };
    const f32 *p00 = at(x0, y0), *p10 = at(x0 + 1, y0), *p01 = at(x0, y0 + 1), *p11 = at(x0 + 1, y0 + 1);
    for (int k = 0; k < 4; ++k) {
        const f32 top = p00[k] + (p10[k] - p00[k]) * tx;
        const f32 bot = p01[k] + (p11[k] - p01[k]) * tx;
        out[k] = top + (bot - top) * ty;
    }
}

void DrawList::pushFilter(const FilterProgram& program, bool chained, const Rect& bbox) {
    DrawCmd c;
    c.op = DrawOp::PushFilter;
    c.v = chained ? 1 : 0;
    if (!program.empty()) {
        c.u = 1;
        m_pendingPrograms.push_back({u32(m_cmds.size()), program, bbox});
    }
    m_filterStack.push_back(u32(m_cmds.size()));
    m_cmds.push_back(c);
}

void DrawList::backdrop(const Rect& r, const FilterProgram& program, const std::array<f32, 4>& radii, Color tint) {
    DrawCmd c;
    c.op = DrawOp::Backdrop;
    c.rect = r;
    c.fill = tint;
    c.radius = radii;
    if (program.empty()) { m_cmds.push_back(c); return; }
    const FilterProgram res = program.resolved ? program : program.resolve(r);
    u16 count = 0;
    const u32 at = u32(m_stops.size());
    if (encodeFilterProgram(res, m_stops, count)) c.u = 1, c.stops = at, c.stopCount = count;
    m_cmds.push_back(c);
}

void DrawList::popFilter() {
    if (m_filterStack.empty()) return;
    const u32 at = m_filterStack.back();
    m_filterStack.pop_back();
    Rect ink{};
    for (std::size_t i = at + 1; i < m_cmds.size(); ++i) ink = unite(ink, inkBounds(m_cmds[i]));
    DrawCmd c = m_cmds[at];
    if (c.u == 2) { // transform layer: the local ink plus its image under the forward map
        const auto& g = c.gradXf;
        const double det = double(g[0]) * double(g[3]) - double(g[1]) * double(g[2]);
        if (det == 0.0) { // singular (CSS: not rendered)
            m_cmds.resize(at);
            return;
        }
        if (ink.empty()) c.rect = {};
        else {
            const double f0 = double(g[3]) / det, f1 = -double(g[1]) / det, f2 = -double(g[2]) / det, f3 = double(g[0]) / det;
            const double f4 = -(f0 * double(g[4]) + f2 * double(g[5])), f5 = -(f1 * double(g[4]) + f3 * double(g[5]));
            double lx = 1e30, ly = 1e30, hx = -1e30, hy = -1e30;
            for (int k = 0; k < 4; ++k) {
                const double x = (k & 1) ? double(ink.right()) : double(ink.x), y = (k & 2) ? double(ink.bottom()) : double(ink.y);
                const double X = f0 * x + f2 * y + f4, Y = f1 * x + f3 * y + f5;
                lx = std::min(lx, X), ly = std::min(ly, Y), hx = std::max(hx, X), hy = std::max(hy, Y);
            }
            const Rect dst{f32(lx) - 1.f, f32(ly) - 1.f, f32(hx - lx) + 2.f, f32(hy - ly) + 2.f};
            const Rect u = unite(ink, dst);
            const f32 x0 = std::floor(u.x), y0 = std::floor(u.y);
            c.rect = {x0, y0, std::ceil(u.right()) - x0, std::ceil(u.bottom()) - y0};
        }
    } else if (c.u == 1) { // SVG program: resolve against the bbox (ink by default); the layer box is the filter region
        c.stopCount = 0;
        c.rect = {};
        if (!m_pendingPrograms.empty() && m_pendingPrograms.back().cmd == at) {
            const PendingProgram pp = std::move(m_pendingPrograms.back());
            m_pendingPrograms.pop_back();
            const Rect bb = pp.bbox.empty() ? ink : pp.bbox;
            if (!bb.empty() || pp.program.resolved) {
                const FilterProgram res = pp.program.resolved ? pp.program : pp.program.resolve(bb);
                u16 count = 0;
                const u32 st = u32(m_stops.size());
                if (encodeFilterProgram(res, m_stops, count) && !res.regionPx.empty()) {
                    c.stops = st, c.stopCount = count;
                    const Rect& g = res.regionPx;
                    const f32 x0 = std::floor(g.x), y0 = std::floor(g.y);
                    c.rect = {x0, y0, std::ceil(g.right()) - x0, std::ceil(g.bottom()) - y0};
                }
            }
        }
        if (c.stopCount == 0) c.u = 0; // nothing to run: an empty layer box
    } else if (!ink.empty()) {
        const int r = c.blur > 0.f ? std::max(1, int(std::lround(c.blur / 3.f))) : 0;
        int reach = 3 * r + 1;
        if (isFilterProgram(c) && std::size_t(c.stops) + c.stopCount <= m_stops.size()) reach = int(m_stops[c.stops].color.g);
        else if (c.stopCount == 6 && std::size_t(c.stops) + 6 <= m_stops.size()) {
            const GradientStop* sh = m_stops.data() + c.stops + 4;
            const int r2 = sh[0].offset > 0.f ? std::max(1, int(std::lround(sh[0].offset / 6.f))) : 0;
            reach += 3 * r2 + int(std::max(std::abs(std::lround(sh[1].color.r)), std::abs(std::lround(sh[1].color.g))));
        }
        const f32 x0 = std::floor(ink.x) - f32(reach), y0 = std::floor(ink.y) - f32(reach);
        c.rect = {x0, y0, std::ceil(ink.right()) + f32(reach) - x0, std::ceil(ink.bottom()) + f32(reach) - y0};
    } else c.rect = {};
    m_cmds[at].rect = c.rect;
    m_cmds[at].u = c.u, m_cmds[at].stops = c.stops, m_cmds[at].stopCount = c.stopCount;
    c.op = DrawOp::PopFilter;
    m_cmds.push_back(c);
}

// ---- ColorMatrix (Filter Effects 1, section 13 shorthand equivalents) ----
namespace {
ColorMatrix rgb3(const f32 a[9], f32 off = 0.f) {
    ColorMatrix c;
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) c.m[std::size_t(r * 5 + k)] = a[r * 3 + k];
        c.m[std::size_t(r * 5 + 4)] = off;
    }
    return c;
}
} // namespace
ColorMatrix ColorMatrix::brightness(f32 a) {
    a = std::max(a, 0.f);
    const f32 v[9] = {a, 0, 0, 0, a, 0, 0, 0, a};
    return rgb3(v);
}
ColorMatrix ColorMatrix::contrast(f32 a) {
    a = std::max(a, 0.f);
    const f32 v[9] = {a, 0, 0, 0, a, 0, 0, 0, a};
    return rgb3(v, 0.5f - 0.5f * a);
}
ColorMatrix ColorMatrix::saturate(f32 s) {
    s = std::max(s, 0.f);
    const f32 v[9] = {0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s,
                      0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s,
                      0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s};
    return rgb3(v);
}
ColorMatrix ColorMatrix::grayscale(f32 a) {
    const f32 g = 1.f - std::clamp(a, 0.f, 1.f);
    const f32 v[9] = {0.2126f + 0.7874f * g, 0.7152f - 0.7152f * g, 0.0722f - 0.0722f * g,
                      0.2126f - 0.2126f * g, 0.7152f + 0.2848f * g, 0.0722f - 0.0722f * g,
                      0.2126f - 0.2126f * g, 0.7152f - 0.7152f * g, 0.0722f + 0.9278f * g};
    return rgb3(v);
}
ColorMatrix ColorMatrix::sepia(f32 a) {
    const f32 g = 1.f - std::clamp(a, 0.f, 1.f);
    const f32 v[9] = {0.393f + 0.607f * g, 0.769f - 0.769f * g, 0.189f - 0.189f * g,
                      0.349f - 0.349f * g, 0.686f + 0.314f * g, 0.168f - 0.168f * g,
                      0.272f - 0.272f * g, 0.534f - 0.534f * g, 0.131f + 0.869f * g};
    return rgb3(v);
}
ColorMatrix ColorMatrix::hueRotate(f32 deg) {
    const f32 r = deg * 3.14159265358979f / 180.f, c = std::cos(r), s = std::sin(r);
    const f32 v[9] = {0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f,
                      0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f,
                      0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f};
    return rgb3(v);
}
ColorMatrix ColorMatrix::invert(f32 a) {
    a = std::clamp(a, 0.f, 1.f);
    const f32 k = 1.f - 2.f * a;
    const f32 v[9] = {k, 0, 0, 0, k, 0, 0, 0, k};
    return rgb3(v, a);
}
ColorMatrix ColorMatrix::opacity(f32 a) {
    ColorMatrix c;
    c.m[18] = std::clamp(a, 0.f, 1.f);
    return c;
}
ColorMatrix ColorMatrix::then(const ColorMatrix& n) const {
    ColorMatrix o;
    for (int r = 0; r < 4; ++r) {
        for (int k = 0; k < 5; ++k) {
            f32 acc = k == 4 ? n.m[std::size_t(r * 5 + 4)] : 0.f;
            for (int j = 0; j < 4; ++j) acc += n.m[std::size_t(r * 5 + j)] * m[std::size_t(j * 5 + k)];
            o.m[std::size_t(r * 5 + k)] = acc;
        }
    }
    return o;
}
void ColorMatrix::apply(const f32 in[4], f32 out[4]) const {
    for (int r = 0; r < 4; ++r) {
        const f32* w = m.data() + r * 5;
        out[r] = std::clamp(w[0] * in[0] + w[1] * in[1] + w[2] * in[2] + w[3] * in[3] + w[4], 0.f, 1.f);
    }
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
        else if (isTransformLayer(c)) conjugateLayer(c.gradXf, {s, s}, {});
        out.push(c);
    }
    out.m_stops = in.m_stops;
    for (const DrawCmd& c : in.cmds()) // drop-shadow blur / offset (stops 4 / 5; PopFilter shares its PushFilter's)
        if ((c.op == DrawOp::Backdrop || c.op == DrawOp::PushFilter) && c.stopCount == 6 && std::size_t(c.stops) + 6 <= out.m_stops.size()) {
            out.m_stops[c.stops + 4].offset *= s;
            out.m_stops[c.stops + 5].color.r *= s;
            out.m_stops[c.stops + 5].color.g *= s;
        }
    for (const DrawCmd& c : in.cmds()) // filter programs: lengths, subregions, derived words (PopFilter shares the stops)
        if ((c.op == DrawOp::PushFilter || c.op == DrawOp::Backdrop) && isFilterProgram(c) &&
            std::size_t(c.stops) + c.stopCount <= out.m_stops.size())
            scaleFilterProgram(out.m_stops.data() + c.stops, s);
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
        if ((c.op == DrawOp::Backdrop || c.op == DrawOp::PushFilter || c.op == DrawOp::PopFilter) && c.stopCount &&
            std::size_t(c.stops) + c.stopCount <= m_stops.size())
            h = fnv(h, m_stops.data() + c.stops, std::size_t(c.stopCount) * sizeof(GradientStop));
        if (isTransformLayer(c)) h = fnv(h, c.gradXf.data(), 24);
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

/// Float target blend (ui_draw_list.glsl blend() under UI_HDR): source-over without quantisation, rgb kept >= 0
/// (above 1 = brighter than SDR white), alpha clamped to [0, 1].
inline void blend(PixelF& d, Px s) {
    if (s.a <= 0.f && s.r <= 0.f && s.g <= 0.f && s.b <= 0.f) return;
    const f32 inv = 1.f - s.a;
    d = {std::max(s.r + d.r * inv, 0.f), std::max(s.g + d.g * inv, 0.f), std::max(s.b + d.b * inv, 0.f),
         std::clamp(s.a + d.a * inv, 0.f, 1.f)};
}
// backdrop / layer helpers per target pixel: premultiplied 0..255 float in, and the store back (ui_backdrop.glsl)
inline void load255(u32 p, f32* o) {
    for (int k = 0; k < 4; ++k) o[k] = f32((p >> (8 * k)) & 0xFFu);
}
inline void load255(const PixelF& p, f32* o) {
    o[0] = p.r * 255.f, o[1] = p.g * 255.f, o[2] = p.b * 255.f, o[3] = p.a * 255.f;
}
/// blurred backdrop `b` (0..255) mixed over the pixel by `cov`
inline void backdropMix(u32& d, const f32* b, f32 cov) {
    u32 o = 0;
    for (int k = 0; k < 4; ++k) {
        const f32 dv = f32((d >> (8 * k)) & 0xFFu);
        o |= u32(std::clamp(std::lround(dv + (b[k] - dv) * cov), 0l, 255l)) << (8 * k);
    }
    d = o;
}
inline void backdropMix(PixelF& d, const f32* b, f32 cov) {
    f32* v = &d.r;
    for (int k = 0; k < 4; ++k) v[k] = std::max(v[k] + (b[k] / 255.f - v[k]) * cov, 0.f);
    d.a = std::min(d.a, 1.f);
}
/// filtered layer `o` (0..255) over the saved pixel `u`, mixed by the clip coverage `cov` (> 0)
inline u32 layerMix(u32 u, const f32* o, f32 cov) {
    const f32 inv = 1.f - o[3] / 255.f;
    u32 res = 0;
    for (int k = 0; k < 4; ++k) {
        const f32 bv = f32((u >> (8 * k)) & 0xFFu);
        const f32 rv = o[k] + bv * inv;
        const f32 m = bv + (rv - bv) * cov;
        res |= u32(std::clamp(std::lround(m), 0l, 255l)) << (8 * k);
    }
    return res;
}
inline PixelF layerMix(const PixelF& under, const f32* o, f32 cov) {
    const f32 inv = 1.f - o[3] / 255.f;
    f32 u[4], res[4];
    load255(under, u);
    for (int k = 0; k < 4; ++k) {
        const f32 rv = o[k] + u[k] * inv;
        const f32 m = u[k] + (rv - u[k]) * cov;
        res[k] = std::max(m / 255.f, 0.f);
    }
    return {res[0], res[1], res[2], std::min(res[3], 1.f)};
}
inline void storeSpan(u32* p, int n, u32 v) { simd::fillSpan(p, n, v); }
inline void storeSpan(PixelF* p, int n, PixelF v) { std::fill_n(p, n, v); }
/// Image texel at (sx, sy) blended by k (clip coverage x opacity)
inline void imageTexel(u32& d, const Image& img, int sx, int sy, f32 k) {
    const u32 s = img.at(sx, sy);
    if (k >= 1.f && (s >> 24) == 255u) { d = s; return; } // opaque texel: blend() would store it
    blend(d, {f32(s & 0xFF) / 255.f * k, f32((s >> 8) & 0xFF) / 255.f * k, f32((s >> 16) & 0xFF) / 255.f * k, f32(s >> 24) / 255.f * k});
}
inline void imageTexel(PixelF& d, const Image& img, int sx, int sy, f32 k) {
    if (img.hasHdr()) { // RGBA16F premultiplied texels: values above 1 kept (ui_draw_list.glsl, ext 0x100)
        const u64 h = img.hdrPixels[std::size_t(sy) * std::size_t(img.width) + std::size_t(sx)];
        const f32 r = std::max(color::halfToFloat(u16(h)), 0.f), g = std::max(color::halfToFloat(u16(h >> 16)), 0.f),
                  b = std::max(color::halfToFloat(u16(h >> 32)), 0.f), a = std::clamp(color::halfToFloat(u16(h >> 48)), 0.f, 1.f);
        blend(d, {r * k, g * k, b * k, a * k});
        return;
    }
    const u32 s = img.at(sx, sy);
    blend(d, {f32(s & 0xFF) / 255.f * k, f32((s >> 8) & 0xFF) / 255.f * k, f32((s >> 16) & 0xFF) / 255.f * k, f32(s >> 24) / 255.f * k});
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
// backdrop-filter colour matrix on one premultiplied 0..255 pixel (rows as GradientStops: colour = r g b a, offset)
// (hdr: the float target keeps rgb above 1, as ui_backdrop.glsl under UI_HDR)
void filterPixel(const GradientStop* rows, const f32* b, f32* out, bool hdr) {
    const f32 a = b[3] / 255.f;
    f32 st[4] = {0.f, 0.f, 0.f, a};
    if (a > 0.f)
        for (int k = 0; k < 3; ++k) st[k] = std::clamp(b[k] / 255.f / a, 0.f, 1.f);
    f32 res[4];
    for (int k = 0; k < 4; ++k) {
        const GradientStop& w = rows[k];
        const f32 v = w.color.r * st[0] + w.color.g * st[1] + w.color.b * st[2] + w.color.a * st[3] + w.offset;
        res[k] = hdr && k < 3 ? std::max(v, 0.f) : std::clamp(v, 0.f, 1.f);
    }
    for (int k = 0; k < 3; ++k) out[k] = res[k] * res[3] * 255.f;
    out[3] = res[3] * 255.f;
}
// three box passes per axis over a w x h x 4 float box (clamp to edge), the shared CPU blur of backdrop and filter
void boxBlur3(std::vector<f32>& buf, std::vector<f32>& tmp, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<f32> scratch(buf.size());
    const f32 inv = 1.f / f32(2 * r + 1);
    simd::boxHorizontalN(buf.data(), tmp.data(), scratch.data(), w, h, r, inv, 3);
    for (int it = 0; it < 3; ++it) { // rows are contiguous: one FORGE-lane vector sweep over all columns x channels
        simd::boxVertical(buf.data(), tmp.data(), w * 4, h, r, inv);
        buf.swap(tmp);
    }
}
/// filter matrix rows + drop shadow of a Backdrop / PushFilter / PopFilter command (stops 0..3, 4..5)
struct FilterData {
    const GradientStop* rows = nullptr;
    bool shadow = false;
    int dx = 0, dy = 0, r2 = 0;
    Color color{};
};
FilterData filterData(const DrawCmd& c, const GradientStop* stops, std::size_t n) {
    FilterData f;
    if (!stops || (c.stopCount != 4 && c.stopCount != 6) || std::size_t(c.stops) + c.stopCount > n) return f;
    f.rows = stops + c.stops;
    if (c.stopCount == 6) {
        const GradientStop* sh = f.rows + 4;
        f.shadow = true;
        f.color = sh[0].color;
        f.r2 = sh[0].offset > 0.f ? std::max(1, int(std::lround(sh[0].offset / 6.f))) : 0;
        f.dx = int(std::lround(sh[1].color.r));
        f.dy = int(std::lround(sh[1].color.g));
    }
    return f;
}
/// matrix over every pixel of the box; with a shadow, `plane` = the filtered box blurred by the shadow radius
void applyFilterBox(const FilterData& f, std::vector<f32>& buf, std::vector<f32>& plane, std::vector<f32>& tmp, int w, int h, bool hdr) {
    if (!f.rows) return;
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        f32 o[4];
        filterPixel(f.rows, &buf[i], o, hdr);
        std::memcpy(&buf[i], o, 16);
    }
    if (!f.shadow) return;
    plane = buf;
    boxBlur3(plane, tmp, w, h, f.r2);
}
/// premultiplied 0..255 pixel `b` at box (x, y) over the drop shadow sampled at (x - dx, y - dy)
void withShadow(const FilterData& f, const std::vector<f32>& plane, int x, int y, int w, int h, f32* b) {
    const int qx = x - f.dx, qy = y - f.dy;
    const f32 sa = qx >= 0 && qy >= 0 && qx < w && qy < h ? plane[(std::size_t(qy) * std::size_t(w) + std::size_t(qx)) * 4 + 3] : 0.f;
    const f32 sA = f.color.a * (sa / 255.f);
    const f32 S[4] = {f.color.r * sA * 255.f, f.color.g * sA * 255.f, f.color.b * sA * 255.f, sA * 255.f};
    const f32 inv = 1.f - b[3] / 255.f;
    for (int k = 0; k < 4; ++k) b[k] = b[k] + S[k] * inv;
}
// Clip paths active at the backdrop (DrawOp::ClipPath / pushClipLayers) multiply its coverage like any other command.
template <class T>
void backdropBlur(const DrawCmd& c, const Rect& clipR, f32 clipRadius, T& target, const PathEdge* edges = nullptr,
                  const ClipPathRef* paths = nullptr, u8 pathCount = 0, const GradientStop* filterRows = nullptr,
                  std::size_t filterRowCount = 0) {
    const Rect area = c.rect.intersect(clipR).intersect({0.f, 0.f, f32(target.width), f32(target.height)});
    if (area.empty()) return;
    const int r = c.blur > 0.f ? std::max(1, int(std::lround(c.blur / 3.f))) : 0; // 0: filter only, no blur
    const int x0 = int(std::floor(area.x)), y0 = int(std::floor(area.y));
    const int x1 = int(std::ceil(area.right())), y1 = int(std::ceil(area.bottom()));
    const int m = 3 * r;
    int sx0 = std::max(0, x0 - m), sy0 = std::max(0, y0 - m);
    int sx1 = std::min(target.width, x1 + m), sy1 = std::min(target.height, y1 + m);
    const bool prog = isFilterProgram(c) && filterRows && std::size_t(c.stops) + c.stopCount <= filterRowCount;
    if (prog) { // SVG program: the source box is the filter region (as gpu_draw_data's Backdrop pass)
        const Color& g = filterRows[c.stops + 1].color;
        sx0 = std::max(0, int(std::floor(g.r))), sy0 = std::max(0, int(std::floor(g.g)));
        sx1 = std::min(target.width, int(std::ceil(g.r + g.b))), sy1 = std::min(target.height, int(std::ceil(g.g + g.a)));
    }
    const int w = sx1 - sx0, h = sy1 - sy0;
    if (w <= 0 || h <= 0) return;
    std::vector<f32> buf(std::size_t(w) * std::size_t(h) * 4), tmp(buf.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            load255(target.pixels[std::size_t(sy0 + y) * std::size_t(target.width) + std::size_t(sx0 + x)],
                    &buf[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4]);
    constexpr bool hdr = std::is_same_v<std::decay_t<decltype(target.pixels[0])>, PixelF>;
    FilterData fd{};
    std::vector<f32> plane;
    if (prog) runFilterProgram(filterRows + c.stops, buf, w, h, sx0, sy0, hdr);
    else {
        boxBlur3(buf, tmp, w, h, r);
        // colour matrix over the whole source box, then the drop shadow plane (ui_backdrop.glsl modes 5 and 3)
        fd = filterData(c, filterRows, filterRowCount);
        applyFilterBox(fd, buf, plane, tmp, w, h, hdr);
    }
    const std::array<f32, 4> clipRad{clipRadius, clipRadius, clipRadius, clipRadius};
    for (int y = std::max(y0, sy0); y < std::min(y1, sy1); ++y)
        for (int x = std::max(x0, sx0); x < std::min(x1, sx1); ++x) {
            const f32 px = f32(x) + 0.5f, py = f32(y) + 0.5f;
            f32 cov = std::clamp(0.5f - sdRoundRect(px, py, c.rect, c.radius), 0.f, 1.f);
            if (clipRadius > 0.f) cov *= std::clamp(0.5f - sdRoundRect(px, py, clipR, clipRad), 0.f, 1.f);
            for (u8 q = 0; q < pathCount && cov > 0.f; ++q) cov *= clipEntryCoverage(edges + paths[q].at, paths[q].n, px, py, paths[q].evenOdd);
            if (cov <= 0.f) continue;
            auto& d = target.pixels[std::size_t(y) * std::size_t(target.width) + std::size_t(x)];
            f32 b[4];
            std::memcpy(b, &buf[(std::size_t(y - sy0) * std::size_t(w) + std::size_t(x - sx0)) * 4], 16);
            if (fd.shadow) withShadow(fd, plane, x - sx0, y - sy0, w, h, b);
            backdropMix(d, b, cov);
            if (c.fill.a > 0.f) blend(d, premul(c.fill, cov));
        }
}

/// An open CSS filter layer (PushFilter): its pixel box and the pixels it covered (restored under the result).
template <class P>
struct Layer {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<P> under;
};
template <class T, class P>
void layerBegin(const DrawCmd& c, const Rect& clipR, T& target, Layer<P>& L) {
    const Rect area = c.rect.intersect(clipR).intersect({0.f, 0.f, f32(target.width), f32(target.height)});
    L = {};
    if (area.empty()) return;
    L.x0 = int(std::floor(area.x)), L.y0 = int(std::floor(area.y));
    L.x1 = int(std::ceil(area.right())), L.y1 = int(std::ceil(area.bottom()));
    L.under.resize(std::size_t(L.x1 - L.x0) * std::size_t(L.y1 - L.y0));
    for (int y = L.y0; y < L.y1; ++y)
        for (int x = L.x0; x < L.x1; ++x) {
            P& d = target.pixels[std::size_t(y) * std::size_t(target.width) + std::size_t(x)];
            L.under[std::size_t(y - L.y0) * std::size_t(L.x1 - L.x0) + std::size_t(x - L.x0)] = d;
            d = P{};
        }
}
// ui_backdrop.glsl modes 0, 1 / 2, 5, 6: the layer as float, blurred, filtered, shadowed, then over the saved pixels
// (mixed by the clip coverage: outside the clip the saved pixel comes back unchanged)
template <class T, class P>
void layerEnd(const DrawCmd& c, const Rect& clipR, f32 clipRadius, T& target, const Layer<P>& L, const PathEdge* edges,
              const ClipPathRef* paths, u8 pathCount, const GradientStop* filterRows, std::size_t filterRowCount) {
    const int w = L.x1 - L.x0, h = L.y1 - L.y0;
    if (w <= 0 || h <= 0) return;
    std::vector<f32> buf(std::size_t(w) * std::size_t(h) * 4), tmp(buf.size()), plane;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            load255(target.pixels[std::size_t(L.y0 + y) * std::size_t(target.width) + std::size_t(L.x0 + x)],
                    &buf[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4]);
    constexpr bool hdr = std::is_same_v<P, PixelF>;
    FilterData fd{};
    if (isFilterProgram(c) && filterRows && std::size_t(c.stops) + c.stopCount <= filterRowCount) {
        runFilterProgram(filterRows + c.stops, buf, w, h, L.x0, L.y0, hdr);
    } else {
        const int r = c.blur > 0.f ? std::max(1, int(std::lround(c.blur / 3.f))) : 0;
        boxBlur3(buf, tmp, w, h, r);
        fd = filterData(c, filterRows, filterRowCount);
        applyFilterBox(fd, buf, plane, tmp, w, h, hdr);
    }
    if (isTransformLayer(c)) { // resample through the pixel -> local map (ui_backdrop.glsl mode 6)
        const auto& g = c.gradXf;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const f32 px = f32(L.x0 + x) + 0.5f, py = f32(L.y0 + y) + 0.5f;
                const f32 qx = g[0] * px + g[2] * py + g[4], qy = g[1] * px + g[3] * py + g[5];
                sampleLayerBilinear(buf.data(), w, h, qx - f32(L.x0), qy - f32(L.y0), &tmp[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4]);
            }
        buf.swap(tmp);
    }
    const std::array<f32, 4> clipRad{clipRadius, clipRadius, clipRadius, clipRadius};
    const bool chained = c.v != 0; // inner layer of a chain: the outermost applies the clip coverage
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const f32 px = f32(L.x0 + x) + 0.5f, py = f32(L.y0 + y) + 0.5f;
            f32 cov = 1.f;
            if (!chained && clipRadius > 0.f) cov *= std::clamp(0.5f - sdRoundRect(px, py, clipR, clipRad), 0.f, 1.f);
            for (u8 q = 0; q < (chained ? 0 : pathCount) && cov > 0.f; ++q) cov *= clipEntryCoverage(edges + paths[q].at, paths[q].n, px, py, paths[q].evenOdd);
            const std::size_t i = std::size_t(y) * std::size_t(w) + std::size_t(x);
            P& d = target.pixels[std::size_t(L.y0 + y) * std::size_t(target.width) + std::size_t(L.x0 + x)];
            const P& u = L.under[i];
            if (cov <= 0.f) { d = u; continue; }
            f32 o[4];
            std::memcpy(o, &buf[i * 4], 16);
            if (fd.shadow) withShadow(fd, plane, x, y, w, h, o);
            d = layerMix(u, o, cov);
        }
}

// The rasteriser over one target pixel type (u32 RGBA8 or PixelF); every per-pixel store goes through the overloads
// above, so the RGBA8 instantiation is the 8-bit path unchanged.
template <class T>
void rasterize(const u8* m_atlas, int m_atlasSize, const std::vector<const Image*>& m_images, const DrawList& list, T& target, Rect scissor) {
    using P = typename decltype(T::pixels)::value_type;
    Rect full{0.f, 0.f, f32(target.width), f32(target.height)};
    if (scissor.empty()) scissor = full;
    scissor = scissor.intersect(full);
    Clip stack[32];
    int depth = 0;
    std::vector<PathEdge> rowEdges;
    ClipPathRef clipPaths[32];
    stack[0] = {scissor, 0.f, 0};
    std::vector<u32> partner;
    matchFilters(list, partner);
    std::vector<Layer<P>> layers;
    for (std::size_t ci = 0; ci < list.cmds().size(); ++ci) {
        const DrawCmd& c = list.cmds()[ci];
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
        case DrawOp::Backdrop:
            backdropBlur(c, clip.r, clip.radius, target, list.pathEdges().data(), clipPaths, clip.paths, list.gradientStops().data(),
                         list.gradientStops().size());
            continue;
        case DrawOp::PushFilter:
            if (partner[ci] != ~0u) layerBegin(c, clip.r, target, layers.emplace_back());
            continue;
        case DrawOp::PopFilter:
            if (partner[ci] != ~0u && !layers.empty()) {
                layerEnd(c, clip.r, clip.radius, target, layers.back(), list.pathEdges().data(), clipPaths, clip.paths,
                         list.gradientStops().data(), list.gradientStops().size());
                layers.pop_back();
            }
            continue;
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
                P* row = target.pixels.data() + std::size_t(y) * std::size_t(target.width);
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
        P solid{};
        if (c.op == DrawOp::Rect && !c.gradient && c.fill.a >= 1.f && clip.radius <= 0.f && clip.paths == 0) {
            const f32 m = std::max({c.radius[0], c.radius[1], c.radius[2], c.radius[3], 0.f}) +
                          (c.borderWidth > 0.f && c.border.a > 0.f ? c.borderWidth : 0.f) + 0.5f;
            ix0 = int(std::ceil(c.rect.x + m - 0.5f)), ix1 = int(std::floor(c.rect.right() - m - 0.5f)) + 1;
            iy0 = int(std::ceil(c.rect.y + m - 0.5f)), iy1 = int(std::floor(c.rect.bottom() - m - 0.5f)) + 1;
            blend(solid, premul(c.fill, 1.f));
        }
        for (int y = y0; y < y1; ++y) {
            P* row = target.pixels.data() + std::size_t(y) * std::size_t(target.width);
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
            if (solidRow && sa < sb) storeSpan(row + sa, sb - sa, solid);
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
                    imageTexel(row[x], img, sx, sy, clipCov * (c.fill.a > 0.f ? c.fill.a : 1.f));
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
} // namespace

void CpuRasterizer::render(const DrawList& list, Image& target, Rect scissor) const {
    if (target.width <= 0 || target.height <= 0) return;
    rasterize(m_atlas, m_atlasSize, m_images, list, target, scissor);
}

void CpuRasterizer::render(const DrawList& list, HdrImage& target, Rect scissor) const {
    if (target.width <= 0 || target.height <= 0 || target.pixels.size() != std::size_t(target.width) * std::size_t(target.height)) return;
    rasterize(m_atlas, m_atlasSize, m_images, list, target, scissor);
    if (target.format != PixelFormat::Rgba16F) return;
    // RGBA16F storage: what was drawn rounded to binary16 (the GPU HDR canvas stores each pass that way)
    Rect full{0.f, 0.f, f32(target.width), f32(target.height)};
    const Rect r = scissor.empty() ? full : scissor.intersect(full);
    const int x0 = int(std::floor(r.x)), y0 = int(std::floor(r.y)), x1 = int(std::ceil(r.right())), y1 = int(std::ceil(r.bottom()));
    for (int y = y0; y < y1; ++y) {
        f32* v = &target.pixels[std::size_t(y) * std::size_t(target.width) + std::size_t(x0)].r;
        for (std::size_t i = 0, n = std::size_t(std::max(0, x1 - x0)) * 4; i < n; ++i) v[i] = color::quantizeHalf(v[i]);
    }
}

void HdrImage::packHalfRow(int y, int x0, int x1, u64* out) const {
    const PixelF* p = pixels.data() + std::size_t(y) * std::size_t(width);
    for (int x = x0; x < x1; ++x)
        *out++ = u64(color::floatToHalf(p[x].r)) | (u64(color::floatToHalf(p[x].g)) << 16) |
                 (u64(color::floatToHalf(p[x].b)) << 32) | (u64(color::floatToHalf(p[x].a)) << 48);
}

void HdrImage::toRgba8(Image& out) const {
    out.resize(width, height);
    auto q = [](f32 v) { return u32(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const PixelF& p = pixels[i];
        out.pixels[i] = q(p.r) | (q(p.g) << 8) | (q(p.b) << 16) | (q(p.a) << 24);
    }
}

} // namespace fuse::ui
