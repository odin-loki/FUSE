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

void DrawList::translateSide(Vec2 d) {
    for (Vec2& p : m_points) {
        p.x += d.x;
        p.y += d.y;
    }
    const Affine back = Affine::translation(-d.x, -d.y);
    for (DrawPaint& p : m_paints) p.toUnit = p.toUnit * back;
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

} // namespace fuse::ui
