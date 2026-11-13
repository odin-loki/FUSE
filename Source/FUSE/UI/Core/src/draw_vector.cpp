// Vector ops of the draw list and their CPU reference rasterisation: Path (fill / stroke, solid or N-stop gradient),
// NinePatch, PushClipPath and PushLayer groups. See the contract note in draw_list.hpp: each op produces premultiplied
// RGBA8 source texels that are composited with the Image blend equation.
#include "draw_internal.hpp"

#include <cstring>

namespace fuse::ui {

using namespace detail;

namespace {
constexpr f32 kTwoPi = 6.28318530717958f;
/// Flattening tolerance in device pixels (curves and round joins / caps).
constexpr f32 kFlatTolerance = 0.05f;
/// Path coordinates are snapped to 1 / kSub device pixels before rasterising (26.6, as FreeType does): the coverage
/// of a shape then depends only on its position within the pixel grid, which is what the mask cache keys on.
constexpr int kSub = 64;
constexpr f32 kMaxCoord = 1.0e6f;
} // namespace

// ---- Recording -----------------------------------------------------------------------------------------------------
void DrawList::clearSide() {
    m_paths.clear();
    m_verbs.clear();
    m_points.clear();
    m_paints.clear();
    m_stops.clear();
    m_patches.clear();
    m_xforms.clear();
    m_filters.clear();
    m_vector = false;
}

u32 DrawList::addPath(const Path& path, const Affine& xf, Rect& bounds) {
    m_vector = true;
    DrawPath rec;
    rec.firstVerb = u32(m_verbs.size());
    rec.verbCount = u32(path.verbs().size());
    rec.firstPoint = u32(m_points.size());
    rec.pointCount = u32(path.points().size());
    m_verbs.insert(m_verbs.end(), path.verbs().begin(), path.verbs().end());
    const bool id = xf.isIdentity();
    f32 x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    bool any = false;
    for (const Vec2& src : path.points()) {
        const Vec2 p = id ? src : xf.apply(src);
        m_points.push_back(p);
        if (!any) { x0 = x1 = p.x; y0 = y1 = p.y; any = true; }
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    bounds = {x0, y0, x1 - x0, y1 - y0};
    m_paths.push_back(rec);
    return u32(m_paths.size() - 1);
}

i32 DrawList::addPaint(const Brush& brush, const Affine& xf) {
    if (!brush.hasGradient) return -1;
    const Gradient& g = brush.gradient;
    Affine inv;
    if (g.stops.empty() || !(xf * g.transform).invert(inv)) return -2; // nothing to paint
    DrawPaint p;
    p.kind = g.kind;
    p.spread = g.spread;
    p.opacity = std::clamp(brush.opacity, 0.f, 1.f);
    p.firstStop = u32(m_stops.size());
    p.stopCount = u32(g.stops.size());
    f32 last = 0.f;
    for (GradientStop s : g.stops) { // offsets clamped to 0..1 and made monotonic (SVG rule)
        s.offset = std::isfinite(s.offset) ? std::clamp(s.offset, last, 1.f) : last;
        last = s.offset;
        m_stops.push_back(s);
    }
    Affine unit; // gradient geometry -> unit space
    const Affine constOne{0.f, 0.f, 0.f, 0.f, 1.f, 0.f}; // degenerate geometry: t = 1 everywhere (the last stop)
    switch (g.kind) {
    case GradientKind::Linear: {
        const f32 dx = g.p1.x - g.p0.x, dy = g.p1.y - g.p0.y, l2 = dx * dx + dy * dy;
        if (!(l2 > 1e-12f)) { unit = constOne; break; }
        unit = {dx / l2, -dy / l2, dy / l2, dx / l2, 0.f, 0.f};
        const Vec2 o = unit.applyVector(g.p0);
        unit.tx = -o.x;
        unit.ty = -o.y;
        break;
    }
    case GradientKind::Radial: {
        const f32 r = g.p1.x;
        if (!(r > 1e-6f)) { unit = constOne; break; }
        unit = {1.f / r, 0.f, 0.f, 1.f / r, -g.p0.x / r, -g.p0.y / r};
        break;
    }
    case GradientKind::Conic:
        unit = Affine::rotation(-g.p1.x) * Affine::translation(-g.p0.x, -g.p0.y);
        break;
    }
    p.toUnit = unit * inv;
    m_paints.push_back(p);
    return i32(m_paints.size() - 1);
}

void DrawList::fillPath(const Path& path, const Brush& brush, FillRule rule, const Affine& xf) {
    if (path.empty()) return;
    if (!brush.hasGradient && brush.color.a <= 0.f) return;
    const i32 paint = addPaint(brush, xf);
    if (paint == -2) return;
    DrawCmd c;
    c.op = DrawOp::Path;
    c.data = addPath(path, xf, c.rect);
    c.rect = {c.rect.x - 1.f, c.rect.y - 1.f, c.rect.w + 2.f, c.rect.h + 2.f};
    c.fill = brush.color;
    m_paths[c.data].paint = paint;
    m_paths[c.data].rule = rule;
    m_cmds.push_back(c);
}

namespace {
f32 strokeReach(const StrokeStyle& s) { // how far the stroke can extend from the path, in units of its half width
    f32 k = 1.f;
    if (s.join == LineJoin::Miter) k = std::max(k, s.miterLimit);
    if (s.cap == LineCap::Square) k = std::max(k, 1.4143f);
    return k;
}
} // namespace

void DrawList::strokePath(const Path& path, const StrokeStyle& stroke, const Brush& brush, const Affine& xf) {
    if (path.empty() || !(stroke.width > 0.f)) return;
    if (!brush.hasGradient && brush.color.a <= 0.f) return;
    const f32 k = xf.meanScale();
    if (!(k > 0.f) || !std::isfinite(k)) return;
    const i32 paint = addPaint(brush, xf);
    if (paint == -2) return;
    DrawCmd c;
    c.op = DrawOp::Path;
    c.data = addPath(path, xf, c.rect);
    DrawPath& rec = m_paths[c.data];
    rec.paint = paint;
    rec.stroked = true;
    rec.stroke = stroke;
    rec.stroke.width *= k;
    rec.stroke.dashOffset *= k;
    for (f32& d : rec.stroke.dashes) d *= k;
    const f32 pad = rec.stroke.width * 0.5f * strokeReach(rec.stroke) + 1.f;
    c.rect = {c.rect.x - pad, c.rect.y - pad, c.rect.w + 2.f * pad, c.rect.h + 2.f * pad};
    c.fill = brush.color;
    c.borderWidth = 0.f;
    m_cmds.push_back(c);
}

void DrawList::path(const Path& path, Color fill, bool evenOdd, Color stroke, f32 strokeWidth) {
    if (path.empty()) return;
    if (fill.a > 0.f) fillPath(path, Brush(fill), evenOdd ? FillRule::EvenOdd : FillRule::NonZero);
    if (strokeWidth > 0.f && stroke.a > 0.f) {
        StrokeStyle s;
        s.width = strokeWidth;
        s.join = LineJoin::Round;
        s.cap = LineCap::Round;
        strokePath(path, s, Brush(stroke));
    }
}

Color gradientStopsAt(std::span<const GradientStop> stops, f32 t) {
    if (stops.empty()) return {};
    if (t <= stops.front().offset) return stops.front().color;
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].offset) {
            const GradientStop& a = stops[i - 1];
            const GradientStop& b = stops[i];
            const f32 span = b.offset - a.offset;
            const f32 k = span > 0.f ? (t - a.offset) / span : 1.f;
            return {a.color.r + (b.color.r - a.color.r) * k, a.color.g + (b.color.g - a.color.g) * k,
                    a.color.b + (b.color.b - a.color.b) * k, a.color.a + (b.color.a - a.color.a) * k};
        }
    }
    return stops.back().color;
}

void DrawList::gradientRect(const Rect& r, const Gradient& g, const std::array<f32, 4>& radii) {
    if (r.empty()) return;
    thread_local Path scratch;
    scratch.clear();
    scratch.addRoundedRect(r, radii);
    fillPath(scratch, Brush(g));
}

void DrawList::ninePatch(u32 image, const Rect& dst, const Edges& srcInsets, const Rect& src, const Edges* dstInsets, f32 opacity,
                         bool smooth) {
    if (dst.empty() || !(opacity > 0.f)) return;
    m_vector = true;
    DrawCmd c;
    c.op = DrawOp::NinePatch;
    c.image = image;
    c.rect = dst;
    c.fill = Color{1.f, 1.f, 1.f, std::min(opacity, 1.f)};
    c.data = u32(m_patches.size());
    m_patches.push_back({src, srcInsets, dstInsets ? *dstInsets : srcInsets, smooth});
    m_cmds.push_back(c);
}

void DrawList::pushClipRounded(const Rect& r, const std::array<f32, 4>& radii) {
    if (radii[0] == radii[1] && radii[1] == radii[2] && radii[2] == radii[3]) return pushClip(r, radii[0]);
    thread_local Path scratch;
    scratch.clear();
    scratch.addRoundedRect(r, radii);
    pushClipPath(scratch);
}

void DrawList::pushClipPath(const Path& path, FillRule rule, const Affine& xf) {
    DrawCmd c;
    c.op = DrawOp::PushClipPath;
    c.data = addPath(path, xf, c.rect);
    c.rect = {c.rect.x - 1.f, c.rect.y - 1.f, c.rect.w + 2.f, c.rect.h + 2.f};
    m_paths[c.data].rule = rule;
    m_cmds.push_back(c);
}

void DrawList::pushLayer(f32 opacity, const Rect& bounds) {
    m_vector = true;
    DrawCmd c;
    c.op = DrawOp::PushLayer;
    c.rect = bounds;
    c.fill = Color{1.f, 1.f, 1.f, std::clamp(opacity, 0.f, 1.f)};
    m_cmds.push_back(c);
}

namespace {
/// Box sizes (odd) of the three box blurs that approximate a Gaussian of standard deviation `sigma` (Kovesi).
void blurBoxes(f32 sigma, int out[3]) {
    out[0] = out[1] = out[2] = 1;
    if (!(sigma > 0.05f) || !std::isfinite(sigma)) return;
    sigma = std::min(sigma, 512.f);
    const f32 ideal = std::sqrt(4.f * sigma * sigma + 1.f);
    int wl = int(std::floor(ideal));
    if (wl % 2 == 0) --wl;
    wl = std::max(wl, 1);
    const f32 m = (12.f * sigma * sigma - 3.f * f32(wl) * f32(wl) - 12.f * f32(wl) - 9.f) / (-4.f * f32(wl) - 4.f);
    const int lower = std::clamp(int(std::lround(m)), 0, 3);
    for (int k = 0; k < 3; ++k) out[k] = k < lower ? wl : wl + 2;
}
/// Below this deviation three box blurs are a poor Gaussian: the kernel itself is used (it is small).
constexpr f32 kExactBlurBelow = 2.f;
int blurReachPx(f32 sigma) {
    if (!(sigma > 0.05f) || !std::isfinite(sigma)) return 0;
    if (sigma < kExactBlurBelow) return int(std::ceil(sigma * 3.f));
    int b[3];
    blurBoxes(sigma, b);
    return (b[0] + b[1] + b[2] - 3) / 2;
}
} // namespace

namespace {
/// Bounding rect of `r` mapped by `xf`, one unit larger on each side (the antialiased edge); empty stays empty.
Rect mappedBounds(const Affine& xf, const Rect& r) {
    if (r.empty()) return {};
    f32 x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    const Vec2 corners[4] = {{r.x, r.y}, {r.right(), r.y}, {r.right(), r.bottom()}, {r.x, r.bottom()}};
    for (int k = 0; k < 4; ++k) {
        const Vec2 p = xf.apply(corners[k]);
        if (k == 0) { x0 = x1 = p.x; y0 = y1 = p.y; }
        x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
    }
    return {x0 - 1.f, y0 - 1.f, x1 - x0 + 2.f, y1 - y0 + 2.f};
}
} // namespace

f32 filterReach(std::span<const FilterOp> ops) {
    f32 reach = 0.f;
    for (const FilterOp& f : ops) {
        if (f.kind == FilterKind::Blur) reach += f32(blurReachPx(f.amount));
        else if (f.kind == FilterKind::DropShadow) {
            const f32 off = std::max(std::abs(f.offset.x), std::abs(f.offset.y));
            reach += f32(blurReachPx(f.amount)) + (std::isfinite(off) ? std::ceil(std::min(off, 1.0e6f)) : 0.f) + 1.f;
        }
    }
    return reach;
}

void DrawList::pushTransform(const Affine& xf, const Rect& localBounds) {
    m_vector = true;
    DrawCmd c;
    c.op = DrawOp::PushTransform;
    c.data = u32(m_xforms.size());
    m_xforms.push_back({xf, localBounds});
    c.rect = mappedBounds(xf, localBounds);
    m_cmds.push_back(c);
}

void DrawList::pushFilter(std::span<const FilterOp> ops, const Rect& bounds) {
    m_vector = true;
    DrawCmd c;
    c.op = DrawOp::PushFilter;
    c.data = u32(m_filters.size());
    c.u = u16(std::min<std::size_t>(ops.size(), 64));
    m_filters.insert(m_filters.end(), ops.begin(), ops.begin() + c.u);
    c.rect = bounds;
    m_cmds.push_back(c);
}

void DrawList::pushMask(const Gradient& g, const Rect& bounds, MaskMode mode) {
    m_vector = true;
    DrawCmd c;
    c.op = DrawOp::PushMask;
    const i32 paint = addPaint(Brush(g), {});
    c.data = paint >= 0 ? u32(paint) : 0xFFFFFFFFu; // no usable gradient: masks everything out
    c.gradient = u8(mode);
    c.rect = bounds;
    m_cmds.push_back(c);
}

void DrawList::setGroupBounds(std::size_t cmdIndex, const Rect& bounds) {
    if (cmdIndex >= m_cmds.size()) return;
    DrawCmd& c = m_cmds[cmdIndex];
    if (c.op == DrawOp::PushTransform && c.data < m_xforms.size()) {
        m_xforms[c.data].src = bounds;
        c.rect = mappedBounds(m_xforms[c.data].xf, bounds);
    } else if (c.op == DrawOp::PushFilter || c.op == DrawOp::PushMask || c.op == DrawOp::PushLayer) {
        c.rect = bounds;
    }
}

Rect DrawList::groupBounds(const DrawCmd& c) const {
    if (c.op != DrawOp::PushFilter || std::size_t(c.data) + c.u > m_filters.size()) return c.rect;
    const f32 e = filterReach(std::span<const FilterOp>(m_filters).subspan(c.data, c.u));
    return {c.rect.x - e, c.rect.y - e, c.rect.w + 2.f * e, c.rect.h + 2.f * e};
}

void DrawList::translateSide(Vec2 d) {
    for (Vec2& p : m_points) {
        p.x += d.x;
        p.y += d.y;
    }
    const Affine back = Affine::translation(-d.x, -d.y);
    for (DrawPaint& p : m_paints) p.toUnit = p.toUnit * back;
    for (DrawTransform& t : m_xforms) { // both spaces move: xf' = T(d) xf T(-d)
        t.xf = Affine::translation(d.x, d.y) * t.xf * back;
        t.src.x += d.x;
        t.src.y += d.y;
    }
}

void DrawList::scaleSide(f32 s, bool snap) {
    for (Vec2& p : m_points) {
        p.x *= s;
        p.y *= s;
    }
    for (DrawPath& p : m_paths) {
        if (!p.stroked) continue;
        p.stroke.width *= s;
        p.stroke.dashOffset *= s;
        for (f32& d : p.stroke.dashes) d *= s;
    }
    const Affine back = Affine::scaling(1.f / s, 1.f / s);
    for (DrawPaint& p : m_paints) p.toUnit = p.toUnit * back;
    for (DrawTransform& t : m_xforms) { // xf' = S xf S^-1: the linear part stays, the translation scales
        t.xf.tx *= s;
        t.xf.ty *= s;
        t.src = {t.src.x * s, t.src.y * s, t.src.w * s, t.src.h * s};
    }
    for (FilterOp& f : m_filters) {
        if (f.kind == FilterKind::Blur || f.kind == FilterKind::DropShadow) f.amount *= s;
        f.offset = {f.offset.x * s, f.offset.y * s};
    }
    for (DrawPatch& p : m_patches) {
        for (f32* v : {&p.dstInsets.l, &p.dstInsets.t, &p.dstInsets.r, &p.dstInsets.b}) {
            const f32 scaled = *v * s;
            *v = snap ? (*v > 0.f ? std::max(1.f, std::round(scaled)) : 0.f) : scaled;
        }
    }
}

// ---- Path coverage -------------------------------------------------------------------------------------------------
namespace {

struct Mask { // coverage of the device rect [x0, x0 + w) x [y0, y0 + h)
    int x0 = 0, y0 = 0, w = 0, h = 0;
    const u8* data = nullptr;
};

struct CacheEntry {
    u64 h1 = 0, h2 = 0;
    u32 offset = 0;
    u16 w = 0, h = 0;
    bool used = false;
};

constexpr u32 kCacheSlots = 2048;      // open addressing, cleared when half full
constexpr u32 kCacheMaxEntries = 1024;
constexpr std::size_t kCacheMaxBytes = 4u << 20;
constexpr int kCacheMaxPixels = 128 * 128; // larger masks are rasterised each time
constexpr u32 kCacheMaxPoints = 4096;

struct PathScratch {
    std::vector<i32> q;      // snapped coordinates, 1 / kSub px, interleaved x y
    std::vector<Vec2> local; // points relative to the mask origin
    Polyline flat, stroked;
    std::vector<f32> acc;
    std::vector<u8> mask;
    std::vector<CacheEntry> table;
    std::vector<u8> arena;
    u32 entries = 0;
    PathCacheStats stats;
    std::vector<std::vector<u32>> layers; // by nesting level
};
PathScratch& scratch() {
    thread_local PathScratch s;
    return s;
}

inline int floorDiv(i32 v) { return v >= 0 ? v / kSub : -((-v + kSub - 1) / kSub); }
inline int ceilDiv(i32 v) { return -floorDiv(-v); }
inline void mix(u64& h1, u64& h2, u32 v) {
    h1 = (h1 ^ v) * 0x9E3779B97F4A7C15ull;
    h1 ^= h1 >> 29;
    h2 = (h2 + v) * 0xC2B2AE3D27D4EB4Full;
    h2 ^= h2 >> 31;
}
inline u32 bits(f32 f) {
    u32 u;
    std::memcpy(&u, &f, 4);
    return u;
}

// A path ready to be turned into coverage: either the whole mask from the cache, or the flattened geometry to
// rasterise row by row (large paths never get a bounding-box mask).
struct PathGeom {
    bool cached = false;
    Mask mask;                      // cached: device rect of the whole mask
    const Polyline* poly = nullptr; // else: geometry relative to (bx0, by0)
    FillRule rule = FillRule::NonZero;
    int bx0 = 0, by0 = 0; // integer origin of the path's bounds
    int mx0 = 0, mw = 0;  // device column window: the bounds clamped to the target's width
    int ry0 = 0, ry1 = 0; // device rows wanted
};

// Prepares the path of `rec` for the device rows [rows0, rows1). False when nothing can be covered.
bool preparePath(const DrawList& list, const DrawPath& rec, int rows0, int rows1, int rootW, PathGeom& g) {
    const auto verbs = list.pathVerbs();
    const auto points = list.pathPoints();
    if (rec.verbCount == 0 || std::size_t(rec.firstVerb) + rec.verbCount > verbs.size() ||
        std::size_t(rec.firstPoint) + rec.pointCount > points.size() || rec.pointCount == 0)
        return false;
    PathScratch& S = scratch();
    S.q.resize(std::size_t(rec.pointCount) * 2);
    i32 minx = 0, miny = 0, maxx = 0, maxy = 0;
    for (u32 i = 0; i < rec.pointCount; ++i) {
        const Vec2 p = points[rec.firstPoint + i];
        if (!(std::abs(p.x) < kMaxCoord) || !(std::abs(p.y) < kMaxCoord)) return false; // also NaN
        const i32 qx = i32(std::lround(p.x * f32(kSub))), qy = i32(std::lround(p.y * f32(kSub)));
        S.q[i * 2] = qx;
        S.q[i * 2 + 1] = qy;
        if (i == 0) { minx = maxx = qx; miny = maxy = qy; }
        minx = std::min(minx, qx);
        maxx = std::max(maxx, qx);
        miny = std::min(miny, qy);
        maxy = std::max(maxy, qy);
    }
    if (rec.stroked) {
        if (!(rec.stroke.width > 0.f) || !(rec.stroke.width < kMaxCoord)) return false;
        const i32 e = i32(std::ceil(rec.stroke.width * 0.5f * strokeReach(rec.stroke) * f32(kSub))) + kSub;
        minx -= e;
        miny -= e;
        maxx += e;
        maxy += e;
    }
    const int bx0 = floorDiv(minx), by0 = floorDiv(miny), bx1 = ceilDiv(maxx), by1 = ceilDiv(maxy);
    const int mx0 = std::max(bx0, 0), mx1 = std::min(bx1, rootW);
    if (mx1 <= mx0 || by1 <= by0) return false;
    int ry0 = std::max(by0, rows0), ry1 = std::min(by1, rows1);
    if (ry1 <= ry0) return false;
    const bool cacheable = bx0 >= 0 && bx1 <= rootW && (bx1 - bx0) <= 4096 && (by1 - by0) <= 4096 &&
                           (bx1 - bx0) * (by1 - by0) <= kCacheMaxPixels && rec.pointCount <= kCacheMaxPoints;
    u64 h1 = 0x243F6A8885A308D3ull, h2 = 0x13198A2E03707344ull;
    u32 slot = 0;
    if (cacheable) {
        mix(h1, h2, u32(rec.rule) | (u32(rec.stroked) << 8) | (u32(bx1 - bx0) << 16));
        mix(h1, h2, u32(by1 - by0) | (rec.verbCount << 16));
        if (rec.stroked) {
            mix(h1, h2, u32(rec.stroke.cap) | (u32(rec.stroke.join) << 8) | (u32(rec.stroke.dashCount) << 16));
            mix(h1, h2, bits(rec.stroke.width));
            mix(h1, h2, bits(rec.stroke.miterLimit));
            for (u8 i = 0; i < rec.stroke.dashCount; ++i) mix(h1, h2, bits(rec.stroke.dashes[i]));
            if (rec.stroke.dashCount) mix(h1, h2, bits(rec.stroke.dashOffset));
        }
        for (u32 i = 0; i < rec.verbCount; ++i) mix(h1, h2, u32(verbs[rec.firstVerb + i]));
        const i32 ox = bx0 * kSub, oy = by0 * kSub;
        for (u32 i = 0; i < rec.pointCount; ++i) {
            mix(h1, h2, u32(S.q[i * 2] - ox));
            mix(h1, h2, u32(S.q[i * 2 + 1] - oy));
        }
        if (S.table.empty()) S.table.resize(kCacheSlots);
        slot = u32(h1) & (kCacheSlots - 1);
        while (S.table[slot].used) {
            const CacheEntry& e = S.table[slot];
            if (e.h1 == h1 && e.h2 == h2 && e.w == bx1 - bx0 && e.h == by1 - by0) {
                ++S.stats.hits;
                g.cached = true;
                g.mask = {bx0, by0, e.w, e.h, S.arena.data() + e.offset};
                return true;
            }
            slot = (slot + 1) & (kCacheSlots - 1);
        }
        ++S.stats.misses;
        ry0 = by0; // the whole mask goes into the cache
        ry1 = by1;
    } else {
        ++S.stats.uncached;
    }
    // geometry relative to the bounds origin (exact in f32: the coordinates are small multiples of 1 / kSub)
    S.local.resize(rec.pointCount);
    for (u32 i = 0; i < rec.pointCount; ++i)
        S.local[i] = {f32(S.q[i * 2] - bx0 * kSub) / f32(kSub), f32(S.q[i * 2 + 1] - by0 * kSub) / f32(kSub)};
    S.flat.clear();
    flattenPath(verbs.subspan(rec.firstVerb, rec.verbCount), S.local, kFlatTolerance, S.flat);
    const Polyline* poly = &S.flat;
    FillRule rule = rec.rule;
    if (rec.stroked) {
        S.stroked.clear();
        strokePolyline(S.flat, rec.stroke, kFlatTolerance, S.stroked);
        poly = &S.stroked;
        rule = FillRule::NonZero;
    }
    g.poly = poly;
    g.rule = rule;
    g.bx0 = bx0;
    g.by0 = by0;
    g.mx0 = mx0;
    g.mw = mx1 - mx0;
    g.ry0 = ry0;
    g.ry1 = ry1;
    if (!cacheable) return true;
    const int w = mx1 - mx0, rows = ry1 - ry0;
    S.mask.resize(std::size_t(w) * std::size_t(rows));
    rasterizeCoverage(*poly, rule, mx0 - bx0, w, ry0 - by0, ry1 - by0, S.mask.data(), S.acc);
    if (S.entries >= kCacheMaxEntries || S.arena.size() + S.mask.size() > kCacheMaxBytes) { // full: start over
        std::fill(S.table.begin(), S.table.end(), CacheEntry{});
        S.arena.clear();
        S.entries = 0;
        slot = u32(h1) & (kCacheSlots - 1);
    }
    S.table[slot] = {h1, h2, u32(S.arena.size()), u16(w), u16(rows), true};
    S.arena.insert(S.arena.end(), S.mask.begin(), S.mask.end());
    ++S.entries;
    g.cached = true;
    g.mask = {mx0, ry0, w, rows, S.mask.data()};
    return true;
}

// Calls f(y, xBegin, xEnd, coverage) for rows of the prepared path, top to bottom; coverage[0] is column xBegin.
// `f` must not record or rasterise other paths (the geometry lives in the thread's scratch).
template <class F>
void coverageRows(const PathGeom& g, int rows0, int rows1, F&& f) {
    if (g.cached) {
        const Mask& m = g.mask;
        for (int y = std::max(m.y0, rows0); y < std::min(m.y0 + m.h, rows1); ++y)
            f(y, m.x0, m.x0 + m.w, m.data + std::size_t(y - m.y0) * std::size_t(m.w));
        return;
    }
    struct Ctx {
        F* f;
        int bx0, by0;
    } ctx{&f, g.bx0, g.by0};
    rasterizeCoverageRows(
        *g.poly, g.rule, g.mx0 - g.bx0, g.mw, g.ry0 - g.by0, g.ry1 - g.by0,
        [](void* u, int row, int xb, int xe, const u8* cov) {
            const Ctx& c = *static_cast<const Ctx*>(u);
            (*c.f)(row + c.by0, xb + c.bx0, xe + c.bx0, cov);
        },
        &ctx, scratch().acc);
}

inline u32 q8(f32 v) { return u32(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); }
inline u32 packPx(Px p) { return q8(p.r) | (q8(p.g) << 8) | (q8(p.b) << 16) | (q8(p.a) << 24); }

// Premultiplied colour of a gradient at the device pixel centre (px, py).
inline Px gradientAt(const DrawPaint& p, const GradientStop* st, f32 px, f32 py) {
    const f32 u = p.toUnit.a * px + p.toUnit.c * py + p.toUnit.tx, v = p.toUnit.b * px + p.toUnit.d * py + p.toUnit.ty;
    f32 t = 0.f;
    switch (p.kind) {
    case GradientKind::Linear: t = u; break;
    case GradientKind::Radial: t = std::sqrt(u * u + v * v); break;
    case GradientKind::Conic:
        t = std::atan2(v, u) / kTwoPi;
        if (t < 0.f) t += 1.f;
        break;
    }
    if (!(t == t)) t = 0.f;
    switch (p.spread) {
    case GradientSpread::Pad: t = std::clamp(t, 0.f, 1.f); break;
    case GradientSpread::Repeat: t -= std::floor(t); break;
    case GradientSpread::Reflect:
        t -= 2.f * std::floor(t * 0.5f);
        if (t > 1.f) t = 2.f - t;
        break;
    }
    const u32 n = p.stopCount;
    u32 i = 0;
    while (i + 1 < n && t >= st[i + 1].offset) ++i;
    Px c = premul(st[i].color, 1.f);
    if (i + 1 < n && t > st[i].offset) {
        const f32 span = st[i + 1].offset - st[i].offset;
        const f32 f = span > 0.f ? (t - st[i].offset) / span : 1.f;
        const Px d = premul(st[i + 1].color, 1.f);
        c = {c.r + (d.r - c.r) * f, c.g + (d.g - c.g) * f, c.b + (d.b - c.b) * f, c.a + (d.a - c.a) * f};
    }
    return {c.r * p.opacity, c.g * p.opacity, c.b * p.opacity, c.a * p.opacity};
}

struct IntArea {
    int x0, y0, x1, y1;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};
inline IntArea toArea(const Rect& area) {
    return {int(std::floor(area.x)), int(std::floor(area.y)), int(std::ceil(area.right())), int(std::ceil(area.bottom()))};
}

} // namespace

PathCacheStats pathCacheStats() {
    PathScratch& S = scratch();
    PathCacheStats st = S.stats;
    st.entries = S.entries;
    st.bytes = S.arena.size();
    return st;
}

void clearPathCache() {
    PathScratch& S = scratch();
    std::fill(S.table.begin(), S.table.end(), CacheEntry{});
    S.arena.clear();
    S.entries = 0;
    S.stats = {};
}

// ---- Rasterisation -------------------------------------------------------------------------------------------------
void detail::drawPathCmd(const RasterEnv& env, const DrawList& list, const DrawCmd& c, const Clip& clip, const Surface& surf) {
    if (c.data >= list.paths().size()) return;
    const DrawPath& rec = list.paths()[c.data];
    const Rect areaR = c.rect.intersect(clip.r);
    if (areaR.empty()) return;
    const IntArea a = toArea(areaR);
    const DrawPaint* paint = nullptr;
    const GradientStop* stops = nullptr;
    if (rec.paint >= 0) {
        if (std::size_t(rec.paint) >= list.paints().size()) return;
        paint = &list.paints()[std::size_t(rec.paint)];
        if (paint->stopCount == 0 || std::size_t(paint->firstStop) + paint->stopCount > list.stops().size()) return;
        stops = list.stops().data() + paint->firstStop;
    }
    PathGeom g;
    if (!preparePath(list, rec, a.y0, a.y1, env.rootW, g)) return;
    const Px solid = premul(c.fill, 1.f);
    const u32 solidTexel = packPx(solid);
    const std::array<f32, 4> clipRad{clip.radius, clip.radius, clip.radius, clip.radius};
    coverageRows(g, a.y0, a.y1, [&](int y, int xb, int xe, const u8* cov) {
        if (y < a.y0 || y >= a.y1) return;
        const int x0 = std::max(xb, a.x0), x1 = std::min(xe, a.x1);
        if (x1 <= x0) return;
        u32* row = surf.row(y) - surf.ox;
        cov -= xb; // indexed by device x
        const f32 py = f32(y) + 0.5f;
        for (int x = x0; x < x1; ++x) {
            const u8 cv = cov[x];
            if (!cv) continue;
            const f32 px = f32(x) + 0.5f;
            f32 clipCov = 1.f;
            if (clip.radius > 0.f) {
                clipCov = std::clamp(0.5f - sdRoundRect(px, py, clip.r, clipRad), 0.f, 1.f);
                if (clipCov <= 0.f) continue;
            }
            u32 texel;
            if (paint) {
                const Px gc = gradientAt(*paint, stops, px, py);
                const f32 k = f32(cv) / 255.f;
                texel = packPx({gc.r * k, gc.g * k, gc.b * k, gc.a * k});
            } else if (cv == 255u) {
                texel = solidTexel;
            } else {
                const f32 k = f32(cv) / 255.f;
                texel = packPx({solid.r * k, solid.g * k, solid.b * k, solid.a * k});
            }
            if (texel) compositeTexel(row[x], texel, clipCov);
        }
    });
}

void detail::drawPatchCmd(const RasterEnv& env, const DrawList& list, const DrawCmd& c, const Clip& clip, const Surface& surf) {
    if (c.data >= list.patches().size() || !env.src.images || c.image >= env.src.imageCount) return;
    const Image* imgp = env.src.images[c.image];
    if (!imgp || imgp->width <= 0 || imgp->height <= 0 ||
        imgp->pixels.size() < std::size_t(imgp->width) * std::size_t(imgp->height))
        return;
    const Image& img = *imgp;
    const DrawPatch& rec = list.patches()[c.data];
    const Rect whole{0.f, 0.f, f32(img.width), f32(img.height)};
    const Rect src = rec.src.empty() ? whole : rec.src.intersect(whole);
    const Rect dst = c.rect;
    const Rect areaR = dst.intersect(clip.r);
    if (src.empty() || areaR.empty()) return;
    const IntArea a = toArea(areaR);
    // insets: non-negative, and never more than the side they cut
    auto fit = [](f32& lo, f32& hi, f32 size) {
        lo = std::max(lo, 0.f);
        hi = std::max(hi, 0.f);
        if (lo + hi > size && lo + hi > 0.f) {
            const f32 k = size / (lo + hi);
            lo *= k;
            hi *= k;
        }
    };
    Edges si = rec.srcInsets, di = rec.dstInsets;
    fit(si.l, si.r, src.w);
    fit(si.t, si.b, src.h);
    fit(di.l, di.r, dst.w);
    fit(di.t, di.b, dst.h);
    struct Axis {
        f32 d0, dA, dB, dSize, s0, sA, sB, sSize;
        int limit;
        // source coordinate of destination coordinate p, and the texel range [lo, hi) of the cell it falls in
        f32 map(f32 p, int& lo, int& hi) const {
            f32 u, flo, fhi;
            if (p < d0 + dA) {
                u = s0 + (p - d0) * (sA / dA);
                flo = s0;
                fhi = s0 + sA;
            } else if (p >= d0 + dSize - dB) {
                u = s0 + sSize - sB + (p - (d0 + dSize - dB)) * (sB / dB);
                flo = s0 + sSize - sB;
                fhi = s0 + sSize;
            } else {
                const f32 mid = dSize - dA - dB;
                u = s0 + sA + (p - d0 - dA) * ((sSize - sA - sB) / std::max(mid, 1e-6f));
                flo = s0 + sA;
                fhi = s0 + sSize - sB;
            }
            lo = std::clamp(int(std::floor(flo + 1e-4f)), 0, limit - 1);
            hi = std::clamp(int(std::ceil(fhi - 1e-4f)), lo + 1, limit);
            return u;
        }
    };
    const Axis ax{dst.x, di.l, di.r, dst.w, src.x, si.l, si.r, src.w, img.width};
    const Axis ay{dst.y, di.t, di.b, dst.h, src.y, si.t, si.b, src.h, img.height};
    const f32 opacity = c.fill.a > 0.f ? c.fill.a : 1.f;
    const std::array<f32, 4> clipRad{clip.radius, clip.radius, clip.radius, clip.radius};
    for (int y = a.y0; y < a.y1; ++y) {
        u32* row = surf.row(y) - surf.ox;
        const f32 py = f32(y) + 0.5f;
        int ylo, yhi;
        const f32 v = ay.map(py, ylo, yhi);
        for (int x = a.x0; x < a.x1; ++x) {
            const f32 px = f32(x) + 0.5f;
            f32 clipCov = 1.f;
            if (clip.radius > 0.f) {
                clipCov = std::clamp(0.5f - sdRoundRect(px, py, clip.r, clipRad), 0.f, 1.f);
                if (clipCov <= 0.f) continue;
            }
            int xlo, xhi;
            const f32 u = ax.map(px, xlo, xhi);
            u32 s;
            if (!rec.smooth) {
                s = img.at(std::clamp(int(std::floor(u)), xlo, xhi - 1), std::clamp(int(std::floor(v)), ylo, yhi - 1));
            } else { // bilinear inside the cell (never across a slice boundary)
                const f32 uu = u - 0.5f, vv = v - 0.5f;
                const f32 fx0 = std::floor(uu), fy0 = std::floor(vv), fx = uu - fx0, fy = vv - fy0;
                const int sx0 = std::clamp(int(fx0), xlo, xhi - 1), sx1 = std::clamp(int(fx0) + 1, xlo, xhi - 1);
                const int sy0 = std::clamp(int(fy0), ylo, yhi - 1), sy1 = std::clamp(int(fy0) + 1, ylo, yhi - 1);
                const u32 t00 = img.at(sx0, sy0), t10 = img.at(sx1, sy0), t01 = img.at(sx0, sy1), t11 = img.at(sx1, sy1);
                s = 0;
                for (int k = 0; k < 32; k += 8) {
                    const f32 top = f32((t00 >> k) & 0xFF) * (1.f - fx) + f32((t10 >> k) & 0xFF) * fx;
                    const f32 bot = f32((t01 >> k) & 0xFF) * (1.f - fx) + f32((t11 >> k) & 0xFF) * fx;
                    s |= u32(std::clamp(top * (1.f - fy) + bot * fy + 0.5f, 0.f, 255.f)) << k;
                }
            }
            if (s) compositeTexel(row[x], s, clipCov * opacity);
        }
    }
}

std::size_t detail::drawGroup(const RasterEnv& env, const DrawList& list, std::size_t i, std::size_t last, const Clip& clip,
                              const Surface& surf, int level, bool opacityOne) {
    const DrawCmd& c = list.cmds()[i];
    const std::size_t end = std::min(matchingPop(list, i), last);
    const bool isLayer = c.op == DrawOp::PushLayer;
    const f32 opacity = isLayer && !opacityOne ? c.fill.a : 1.f;
    const Rect areaR = (isLayer && c.rect.empty()) ? clip.r : c.rect.intersect(clip.r);
    if (areaR.empty() || !(opacity > 0.f) || level >= 16) return end;
    const IntArea a = toArea(areaR);
    const int w = a.x1 - a.x0, h = a.y1 - a.y0;
    {
        PathScratch& S = scratch();
        if (S.layers.size() <= std::size_t(level)) S.layers.resize(std::size_t(level) + 1);
        S.layers[std::size_t(level)].assign(std::size_t(w) * std::size_t(h), 0u);
    }
    u32* const px = scratch().layers[std::size_t(level)].data(); // stable: deeper levels use their own buffers
    const Surface layer{px, a.x0, a.y0, w, h};
    renderRange(env, list, i + 1, end, layer, layer.rect(), level + 1, false);
    if (!isLayer) { // through the clip path's coverage: texel x coverage, in integers
        int next = a.y0; // rows before `next` are done
        auto clearRows = [&](int to) {
            for (; next < to; ++next) std::fill(layer.row(next), layer.row(next) + w, 0u);
        };
        PathGeom g;
        if (c.data < list.paths().size() && preparePath(list, list.paths()[c.data], a.y0, a.y1, env.rootW, g))
            coverageRows(g, a.y0, a.y1, [&](int y, int xb, int xe, const u8* cov) {
                if (y < a.y0 || y >= a.y1) return;
                clearRows(y); // rows the path does not reach
                next = y + 1;
                u32* row = layer.row(y) - a.x0; // indexed by device x
                const int x0 = std::clamp(xb, a.x0, a.x1), x1 = std::clamp(xe, x0, a.x1);
                std::fill(row + a.x0, row + x0, 0u);
                std::fill(row + x1, row + a.x1, 0u);
                cov -= xb;
                for (int x = x0; x < x1; ++x) {
                    u32& t = row[x];
                    const u32 cv = cov[x];
                    if (!t || cv == 255u) continue;
                    if (cv == 0u) {
                        t = 0;
                        continue;
                    }
                    u32 o = 0;
                    for (int k = 0; k < 32; k += 8) o |= ((((t >> k) & 0xFFu) * cv + 127u) / 255u) << k;
                    t = o;
                }
            });
        clearRows(a.y1);
    }
    const std::array<f32, 4> clipRad{clip.radius, clip.radius, clip.radius, clip.radius};
    for (int y = a.y0; y < a.y1; ++y) {
        u32* row = surf.row(y) - surf.ox;
        const u32* lrow = layer.row(y) - a.x0;
        for (int x = a.x0; x < a.x1; ++x) {
            const u32 s = lrow[x];
            if (!s) continue;
            f32 clipCov = 1.f;
            if (clip.radius > 0.f) {
                clipCov = std::clamp(0.5f - sdRoundRect(f32(x) + 0.5f, f32(y) + 0.5f, clip.r, clipRad), 0.f, 1.f);
                if (clipCov <= 0.f) continue;
            }
            compositeTexel(row[x], s, clipCov * opacity);
        }
    }
    return end;
}

// ---- Effect groups: transform, filter, mask ------------------------------------------------------------------------
namespace {
struct FxScratch {
    std::vector<std::vector<u32>> layers; // one per nesting level
    std::vector<u32> tmp;
    std::vector<u8> alpha, alphaTmp;
};
FxScratch& fxScratch() {
    thread_local FxScratch s;
    return s;
}
u32* fxLayer(int level, std::size_t texels) {
    FxScratch& S = fxScratch();
    if (S.layers.size() <= std::size_t(level)) S.layers.resize(std::size_t(level) + 1);
    S.layers[std::size_t(level)].assign(texels, 0u);
    return S.layers[std::size_t(level)].data();
}

// One box blur pass (window `n`, odd) along x or y; texels outside the buffer count as transparent.
template <class T, int Channels>
void boxPass(const T* src, T* dst, int w, int h, int n, bool vertical) {
    const int r = n / 2;
    const int len = vertical ? h : w, lines = vertical ? w : h;
    const std::ptrdiff_t step = vertical ? w : 1;
    for (int line = 0; line < lines; ++line) {
        const T* s = src + (vertical ? line : std::ptrdiff_t(line) * w);
        T* d = dst + (vertical ? line : std::ptrdiff_t(line) * w);
        u32 sum[4] = {0, 0, 0, 0};
        auto add = [&](int i, bool plus) {
            if (i < 0 || i >= len) return;
            const u32 v = u32(s[i * step]);
            for (int c = 0; c < Channels; ++c) {
                const u32 ch = (v >> (c * 8)) & 0xFFu;
                sum[c] = plus ? sum[c] + ch : sum[c] - ch;
            }
        };
        for (int i = 0; i <= r; ++i) add(i, true);
        for (int i = 0; i < len; ++i) {
            u32 o = 0;
            for (int c = 0; c < Channels; ++c) o |= ((sum[c] + u32(n) / 2u) / u32(n)) << (c * 8);
            d[i * step] = T(o);
            add(i + r + 1, true);
            add(i - r, false);
        }
    }
}
// One pass of the Gaussian kernel itself (16.16 fixed point weights that sum to 1), for small deviations.
template <class T, int Channels>
void kernelPass(const T* src, T* dst, int w, int h, const u32* weights, int r, bool vertical) {
    const int len = vertical ? h : w, lines = vertical ? w : h;
    const std::ptrdiff_t step = vertical ? w : 1;
    for (int line = 0; line < lines; ++line) {
        const T* s = src + (vertical ? line : std::ptrdiff_t(line) * w);
        T* d = dst + (vertical ? line : std::ptrdiff_t(line) * w);
        for (int i = 0; i < len; ++i) {
            u32 sum[4] = {0, 0, 0, 0};
            for (int k = std::max(-r, -i); k <= r && i + k < len; ++k) {
                const u32 v = u32(s[(i + k) * step]);
                if (!v) continue;
                for (int c = 0; c < Channels; ++c) sum[c] += weights[k + r] * ((v >> (c * 8)) & 0xFFu);
            }
            u32 o = 0;
            for (int c = 0; c < Channels; ++c) o |= std::min(255u, (sum[c] + 32768u) >> 16) << (c * 8);
            d[i * step] = T(o);
        }
    }
}
template <class T, int Channels>
void gaussianBlur(T* buf, T* tmp, int w, int h, f32 sigma) {
    if (!(sigma > 0.05f) || !std::isfinite(sigma)) return;
    if (sigma < kExactBlurBelow) {
        const int r = blurReachPx(sigma);
        u32 weights[16];
        f32 real[16], total = 0.f;
        for (int k = -r; k <= r; ++k) total += real[k + r] = std::exp(-f32(k * k) / (2.f * sigma * sigma));
        u32 sum = 0;
        for (int k = 0; k <= 2 * r; ++k) sum += weights[k] = u32(real[k] / total * 65536.f + 0.5f);
        weights[r] += 65536u - sum; // exactly 1 in total: a flat area stays what it was
        kernelPass<T, Channels>(buf, tmp, w, h, weights, r, false);
        kernelPass<T, Channels>(tmp, buf, w, h, weights, r, true);
        return;
    }
    int boxes[3];
    blurBoxes(sigma, boxes);
    if (boxes[2] <= 1) return;
    for (const bool vertical : {false, true}) {
        boxPass<T, Channels>(buf, tmp, w, h, boxes[0], vertical);
        boxPass<T, Channels>(tmp, buf, w, h, boxes[1], vertical);
        boxPass<T, Channels>(buf, tmp, w, h, boxes[2], vertical);
        std::memcpy(buf, tmp, std::size_t(w) * std::size_t(h) * sizeof(T));
    }
}

// Colour functions as rgb' = M rgb + offset on straight colour, alpha' = alpha * alphaScale.
struct ColorFn {
    f32 m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    f32 off = 0.f, alpha = 1.f;
};
bool colorFn(const FilterOp& f, ColorFn& o) {
    const f32 a = f.amount;
    auto sat = [&](f32 s) { // the feColorMatrix "saturate" matrix
        const f32 k[9] = {0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s, 0.213f - 0.213f * s, 0.715f + 0.285f * s,
                          0.072f - 0.072f * s, 0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s};
        std::memcpy(o.m, k, sizeof k);
    };
    switch (f.kind) {
    case FilterKind::Brightness: o.m[0] = o.m[4] = o.m[8] = std::max(a, 0.f); return true;
    case FilterKind::Contrast: o.m[0] = o.m[4] = o.m[8] = std::max(a, 0.f); o.off = 0.5f - 0.5f * std::max(a, 0.f); return true;
    case FilterKind::Grayscale: sat(1.f - std::clamp(a, 0.f, 1.f)); return true;
    case FilterKind::Saturate: sat(std::max(a, 0.f)); return true;
    case FilterKind::Sepia: {
        const f32 t = 1.f - std::clamp(a, 0.f, 1.f);
        const f32 k[9] = {0.393f + 0.607f * t, 0.769f - 0.769f * t, 0.189f - 0.189f * t, 0.349f - 0.349f * t, 0.686f + 0.314f * t,
                          0.168f - 0.168f * t, 0.272f - 0.272f * t, 0.534f - 0.534f * t, 0.131f + 0.869f * t};
        std::memcpy(o.m, k, sizeof k);
        return true;
    }
    case FilterKind::HueRotate: {
        const f32 c = std::cos(a), s = std::sin(a);
        const f32 k[9] = {0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f,
                          0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f,
                          0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f};
        std::memcpy(o.m, k, sizeof k);
        return true;
    }
    case FilterKind::Invert: {
        const f32 t = std::clamp(a, 0.f, 1.f);
        o.m[0] = o.m[4] = o.m[8] = 1.f - 2.f * t;
        o.off = t;
        return true;
    }
    case FilterKind::Opacity: o.alpha = std::clamp(a, 0.f, 1.f); return true;
    default: return false;
    }
}
void applyColorFn(u32* px, std::size_t count, const ColorFn& fn) {
    for (std::size_t i = 0; i < count; ++i) {
        const u32 t = px[i];
        const u32 a8 = t >> 24;
        if (!a8) continue;
        const f32 a = f32(a8) / 255.f, inv = 1.f / a;
        const f32 r = f32(t & 0xFF) / 255.f * inv, g = f32((t >> 8) & 0xFF) / 255.f * inv, b = f32((t >> 16) & 0xFF) / 255.f * inv;
        const f32 na = a * fn.alpha;
        const f32 nr = std::clamp(fn.m[0] * r + fn.m[1] * g + fn.m[2] * b + fn.off, 0.f, 1.f);
        const f32 ng = std::clamp(fn.m[3] * r + fn.m[4] * g + fn.m[5] * b + fn.off, 0.f, 1.f);
        const f32 nb = std::clamp(fn.m[6] * r + fn.m[7] * g + fn.m[8] * b + fn.off, 0.f, 1.f);
        px[i] = packPx({nr * na, ng * na, nb * na, na});
    }
}
// drop-shadow: the layer's alpha, moved by `offset` (whole pixels), blurred, tinted, composited under the layer.
void dropShadow(u32* px, int w, int h, const FilterOp& f) {
    FxScratch& S = fxScratch();
    const std::size_t count = std::size_t(w) * std::size_t(h);
    S.alpha.assign(count, 0);
    S.alphaTmp.resize(count);
    if (!std::isfinite(f.offset.x) || !std::isfinite(f.offset.y)) return;
    const int dx = int(std::lround(std::clamp(f.offset.x, -1.0e6f, 1.0e6f))), dy = int(std::lround(std::clamp(f.offset.y, -1.0e6f, 1.0e6f)));
    for (int y = 0; y < h; ++y) {
        const int sy = y - dy;
        if (sy < 0 || sy >= h) continue;
        for (int x = 0; x < w; ++x) {
            const int sx = x - dx;
            if (sx >= 0 && sx < w) S.alpha[std::size_t(y) * w + x] = u8(px[std::size_t(sy) * w + sx] >> 24);
        }
    }
    gaussianBlur<u8, 1>(S.alpha.data(), S.alphaTmp.data(), w, h, f.amount);
    const Px col = premul(f.color, 1.f);
    for (std::size_t i = 0; i < count; ++i) {
        const u32 sa = S.alpha[i];
        if (!sa) continue;
        const f32 k = f32(sa) / 255.f;
        u32 under = packPx({col.r * k, col.g * k, col.b * k, col.a * k});
        if (px[i]) compositeTexel(under, px[i], 1.f);
        px[i] = under;
    }
}
inline Rect inflated(const Rect& r, f32 e) { return {r.x - e, r.y - e, r.w + 2.f * e, r.h + 2.f * e}; }
} // namespace

std::size_t detail::drawFxGroup(const RasterEnv& env, const DrawList& list, std::size_t i, std::size_t last, const Clip& clip,
                                const Surface& surf, int level) {
    const DrawCmd& c = list.cmds()[i];
    const std::size_t end = std::min(matchingPop(list, i), last);
    if (level >= 16) return end;
    const Rect bounds = list.groupBounds(c);
    if (!std::isfinite(bounds.x + bounds.y + bounds.w + bounds.h)) return end;
    const Rect visR = bounds.intersect(clip.r);
    if (visR.empty()) return end;
    const IntArea a = toArea(visR);
    const std::array<f32, 4> clipRad{clip.radius, clip.radius, clip.radius, clip.radius};
    auto clipCoverage = [&](int x, int y) {
        return clip.radius > 0.f ? std::clamp(0.5f - sdRoundRect(f32(x) + 0.5f, f32(y) + 0.5f, clip.r, clipRad), 0.f, 1.f) : 1.f;
    };
    if (c.op == DrawOp::PushTransform) {
        if (c.data >= list.transforms().size()) return end;
        const DrawTransform& t = list.transforms()[c.data];
        Affine inv;
        if (!std::isfinite(t.xf.a + t.xf.b + t.xf.c + t.xf.d + t.xf.tx + t.xf.ty) || !t.xf.invert(inv) || t.src.empty()) return end;
        if (!std::isfinite(inv.a + inv.b + inv.c + inv.d + inv.tx + inv.ty)) return end;
        if (t.xf.a == 1.f && t.xf.b == 0.f && t.xf.c == 0.f && t.xf.d == 1.f && clip.radius <= 0.f) {
            // a translation: the commands are drawn straight onto the target, moved by whole pixels
            const int dx = int(std::lround(t.xf.tx)), dy = int(std::lround(t.xf.ty));
            const Surface moved{surf.px, surf.ox - dx, surf.oy - dy, surf.w, surf.h};
            const Rect sc{clip.r.x - f32(dx), clip.r.y - f32(dy), clip.r.w, clip.r.h};
            renderRange(env, list, i + 1, end, moved, sc.intersect(moved.rect()), level + 1, false);
            return end;
        }
        // the part of the local space the visible destination pixels read (one texel more for the bilinear taps)
        f32 x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        const Vec2 corners[4] = {{f32(a.x0), f32(a.y0)}, {f32(a.x1), f32(a.y0)}, {f32(a.x1), f32(a.y1)}, {f32(a.x0), f32(a.y1)}};
        for (int k = 0; k < 4; ++k) {
            const Vec2 p = inv.apply(corners[k]);
            if (k == 0) { x0 = x1 = p.x; y0 = y1 = p.y; }
            x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
        }
        const Rect need = inflated({x0, y0, x1 - x0, y1 - y0}, 1.5f).intersect(inflated(t.src, 1.f));
        if (need.empty() || !(need.w * need.h <= 16.0e6f)) return end; // (a layer that large is a degenerate matrix)
        const IntArea s = toArea(need);
        const int w = s.x1 - s.x0, h = s.y1 - s.y0;
        u32* const px = fxLayer(level, std::size_t(w) * std::size_t(h));
        const Surface layer{px, s.x0, s.y0, w, h};
        renderRange(env, list, i + 1, end, layer, layer.rect(), level + 1, false);
        auto texel = [&](int x, int y) -> u32 { return (x < 0 || y < 0 || x >= w || y >= h) ? 0u : px[std::size_t(y) * w + x]; };
        for (int y = a.y0; y < a.y1; ++y) {
            u32* row = surf.row(y) - surf.ox;
            for (int x = a.x0; x < a.x1; ++x) {
                const Vec2 p = inv.apply({f32(x) + 0.5f, f32(y) + 0.5f});
                const f32 u = p.x - 0.5f - f32(s.x0), v = p.y - 0.5f - f32(s.y0);
                const f32 fu = std::floor(u), fv = std::floor(v);
                if (!(fu >= -1.f && fv >= -1.f && fu < f32(w) && fv < f32(h))) continue;
                const int ix = int(fu), iy = int(fv);
                const u32 t00 = texel(ix, iy), t10 = texel(ix + 1, iy), t01 = texel(ix, iy + 1), t11 = texel(ix + 1, iy + 1);
                if (!(t00 | t10 | t01 | t11)) continue;
                const f32 fx = u - fu, fy = v - fv;
                u32 o = 0;
                if (t00 == t10 && t00 == t01 && t00 == t11) o = t00;
                else
                    for (int k = 0; k < 32; k += 8) {
                        const f32 top = f32((t00 >> k) & 0xFF) * (1.f - fx) + f32((t10 >> k) & 0xFF) * fx;
                        const f32 bot = f32((t01 >> k) & 0xFF) * (1.f - fx) + f32((t11 >> k) & 0xFF) * fx;
                        o |= u32(std::clamp(top * (1.f - fy) + bot * fy + 0.5f, 0.f, 255.f)) << k;
                    }
                if (!o) continue;
                const f32 cov = clipCoverage(x, y);
                if (cov > 0.f) compositeTexel(row[x], o, cov);
            }
        }
        return end;
    }
    // filter / mask: the layer covers the visible part, plus what the filter needs around it
    f32 reach = 0.f;
    std::span<const FilterOp> ops;
    if (c.op == DrawOp::PushFilter && std::size_t(c.data) + c.u <= list.filters().size()) {
        ops = list.filters().subspan(c.data, c.u);
        reach = filterReach(ops);
    }
    const IntArea l = reach > 0.f ? toArea(bounds.intersect(inflated(visR, reach + 1.f))) : a;
    const int w = l.x1 - l.x0, h = l.y1 - l.y0;
    if (w <= 0 || h <= 0 || f32(w) * f32(h) > 16.0e6f) return end;
    u32* const px = fxLayer(level, std::size_t(w) * std::size_t(h));
    const Surface layer{px, l.x0, l.y0, w, h};
    renderRange(env, list, i + 1, end, layer, layer.rect(), level + 1, false);
    if (c.op == DrawOp::PushFilter) {
        for (const FilterOp& f : ops) {
            ColorFn fn;
            if (f.kind == FilterKind::Blur) {
                FxScratch& S = fxScratch();
                S.tmp.resize(std::size_t(w) * std::size_t(h));
                gaussianBlur<u32, 4>(px, S.tmp.data(), w, h, f.amount);
            } else if (f.kind == FilterKind::DropShadow) {
                dropShadow(px, w, h, f);
            } else if (colorFn(f, fn)) {
                applyColorFn(px, std::size_t(w) * std::size_t(h), fn);
            }
        }
    } else { // mask
        const bool ok = c.data < list.paints().size() && list.paints()[c.data].stopCount > 0 &&
                        std::size_t(list.paints()[c.data].firstStop) + list.paints()[c.data].stopCount <= list.stops().size();
        if (!ok) return end;
        const DrawPaint& paint = list.paints()[c.data];
        const GradientStop* stops = list.stops().data() + paint.firstStop;
        const bool luminance = c.gradient == u8(MaskMode::Luminance);
        for (int y = a.y0; y < a.y1; ++y) {
            u32* row = layer.row(y) - l.x0;
            for (int x = a.x0; x < a.x1; ++x) {
                u32& t = row[x];
                if (!t) continue;
                const Px g = gradientAt(paint, stops, f32(x) + 0.5f, f32(y) + 0.5f);
                const f32 m = std::clamp(luminance ? 0.2126f * g.r + 0.7152f * g.g + 0.0722f * g.b : g.a, 0.f, 1.f);
                const u32 k = u32(m * 255.f + 0.5f);
                if (k == 255u) continue;
                u32 o = 0;
                for (int sh = 0; sh < 32; sh += 8) o |= ((((t >> sh) & 0xFFu) * k + 127u) / 255u) << sh;
                t = o;
            }
        }
    }
    for (int y = a.y0; y < a.y1; ++y) {
        u32* row = surf.row(y) - surf.ox;
        const u32* lrow = layer.row(y) - l.x0;
        for (int x = a.x0; x < a.x1; ++x) {
            const u32 s = lrow[x];
            if (!s) continue;
            const f32 cov = clipCoverage(x, y);
            if (cov > 0.f) compositeTexel(row[x], s, cov);
        }
    }
    return end;
}

} // namespace fuse::ui
