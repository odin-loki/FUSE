// Values of the effect and animation properties: transform, filter, clip-path, gradients (mask-image,
// background-image) and the animation-* lists. Parsing, the PropValue encodings, interpolation and geometry.
#include "style_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::ui {

using namespace detail;

namespace {
constexpr f32 kPi = 3.14159265358979f;

/// "12px" / "50%" / "0" -> value, isPercent.
bool lengthOrPct(std::string_view s, f32& v, bool& pct) {
    std::string_view unit;
    if (!parseNumber(s, v, unit)) return false;
    pct = unit == "%";
    return pct || unit.empty() || unit == "px" || unit == "dp";
}
/// A position keyword or length along one axis ("left" = 0%, "center", "right" / "bottom" = 100%).
bool positionTerm(std::string_view s, f32& v, bool& pct, int& axis) { // axis: 0 x, 1 y, -1 either
    axis = -1;
    pct = true;
    if (s == "center") { v = 50.f; return true; }
    if (s == "left") { v = 0.f; axis = 0; return true; }
    if (s == "right") { v = 100.f; axis = 0; return true; }
    if (s == "top") { v = 0.f; axis = 1; return true; }
    if (s == "bottom") { v = 100.f; axis = 1; return true; }
    return lengthOrPct(s, v, pct);
}
/// "<x> <y>" / "<keyword> [<keyword>]" -> (x, y) with percent flags. Defaults to the centre.
bool parsePosition(std::string_view s, f32 out[2], bool pct[2]) {
    out[0] = out[1] = 50.f;
    pct[0] = pct[1] = true;
    int n = 0;
    bool ok = true;
    int given[2] = {-1, -1};
    splitTop(s, ' ', [&](std::string_view w) {
        f32 v;
        bool p;
        int axis;
        if (n >= 2 || !positionTerm(w, v, p, axis)) return ok = false;
        int slot = axis >= 0 ? axis : (n == 0 ? 0 : 1);
        if (given[slot] >= 0) slot = 1 - slot; // "top left": the first keyword took its own axis
        if (given[slot] >= 0) return ok = false;
        given[slot] = n;
        out[slot] = v;
        pct[slot] = p;
        ++n;
        return true;
    });
    return ok && n > 0;
}
void mul(std::array<f32, 6>& m, const f32 f[6]) { // m = m * f
    const f32 a = m[0] * f[0] + m[2] * f[1], b = m[1] * f[0] + m[3] * f[1];
    const f32 c = m[0] * f[2] + m[2] * f[3], d = m[1] * f[2] + m[3] * f[3];
    m[4] += m[0] * f[4] + m[2] * f[5];
    m[5] += m[1] * f[4] + m[3] * f[5];
    m[0] = a; m[1] = b; m[2] = c; m[3] = d;
}
struct Decomposed {
    f32 sx, sy, shear, angle;
};
Decomposed decompose(const std::array<f32, 6>& m) {
    f32 a = m[0], b = m[1], c = m[2], d = m[3];
    Decomposed o{0.f, 0.f, 0.f, 0.f};
    o.sx = std::sqrt(a * a + b * b);
    if (o.sx > 1e-12f) { a /= o.sx; b /= o.sx; }
    else { a = 1.f; b = 0.f; }
    o.shear = a * c + b * d;
    c -= a * o.shear;
    d -= b * o.shear;
    o.sy = std::sqrt(c * c + d * d);
    if (o.sy > 1e-12f) { c /= o.sy; d /= o.sy; o.shear /= o.sy; }
    if (a * d - b * c < 0.f) { o.sy = -o.sy; o.shear = -o.shear; }
    o.angle = std::atan2(b, a);
    return o;
}
f32 mixf(f32 a, f32 b, f32 k) { return a + (b - a) * k; }
f32 identityAmount(FilterKind k) {
    switch (k) {
    case FilterKind::Brightness:
    case FilterKind::Contrast:
    case FilterKind::Saturate:
    case FilterKind::Opacity: return 1.f;
    default: return 0.f;
    }
}
} // namespace

// ---- transform -----------------------------------------------------------------------------------------------------
bool detail::parseTransformValue(std::string_view text, PropValue& out) {
    text = trim(text);
    Transform2D t;
    bool ok = true;
    if (text != "none")
        ok = !text.empty() && splitTop(text, ' ', [&](std::string_view fnText) {
            std::string_view name, args;
            if (!splitFunction(fnText, name, args)) return false;
            f32 v[6] = {0, 0, 0, 0, 0, 0};
            bool pct[6] = {};
            int n = 0;
            bool good = true;
            const bool angle = name == "rotate" || name.starts_with("skew");
            splitTop(args, ',', [&](std::string_view a) {
                if (n >= 6) return good = false;
                if (angle) good = parseAngle(a, v[n]);
                else if (name.starts_with("translate")) good = lengthOrPct(a, v[n], pct[n]);
                else { std::string_view unit; good = parseNumber(a, v[n], unit) && (unit.empty() || (unit == "%" && name.starts_with("scale"))); if (good && unit == "%") v[n] /= 100.f; }
                ++n;
                return good;
            });
            if (!good || n == 0) return false;
            f32 f[6] = {1.f, 0.f, 0.f, 1.f, 0.f, 0.f};
            if (name == "translate" || name == "translateX" || name == "translateY") {
                if (n > (name == "translate" ? 2 : 1)) return false;
                f32 x = name == "translateY" ? 0.f : v[0], y = name == "translateY" ? v[0] : (n > 1 ? v[1] : 0.f);
                const bool px = name == "translateY" ? false : pct[0], py = name == "translateY" ? pct[0] : (n > 1 && pct[1]);
                if (px) { t.pct[0] += t.m[0] * x / 100.f; t.pct[2] += t.m[1] * x / 100.f; x = 0.f; }
                if (py) { t.pct[1] += t.m[2] * y / 100.f; t.pct[3] += t.m[3] * y / 100.f; y = 0.f; }
                f[4] = x;
                f[5] = y;
            } else if (name == "scale" && n <= 2) { f[0] = v[0]; f[3] = n > 1 ? v[1] : v[0]; }
            else if (name == "scaleX" && n == 1) f[0] = v[0];
            else if (name == "scaleY" && n == 1) f[3] = v[0];
            else if (name == "rotate" && n == 1) { f[0] = std::cos(v[0]); f[1] = std::sin(v[0]); f[2] = -f[1]; f[3] = f[0]; }
            else if (name == "skew" && n <= 2) { f[2] = std::tan(v[0]); f[1] = n > 1 ? std::tan(v[1]) : 0.f; }
            else if (name == "skewX" && n == 1) f[2] = std::tan(v[0]);
            else if (name == "skewY" && n == 1) f[1] = std::tan(v[0]);
            else if (name == "matrix" && n == 6) std::copy(v, v + 6, f);
            else return false;
            mul(t.m, f);
            return true;
        });
    if (!ok) return false;
    for (f32 x : t.m)
        if (!std::isfinite(x)) return false;
    for (f32 x : t.pct)
        if (!std::isfinite(x)) return false;
    out = transformValue(t);
    return true;
}

PropValue transformValue(const Transform2D& t) {
    PropValue v;
    v.type = PropType::Raw;
    for (int i = 0; i < 6; ++i) v.f[std::size_t(i)] = t.m[std::size_t(i)];
    for (int i = 0; i < 4; ++i) v.f[std::size_t(6 + i)] = t.pct[std::size_t(i)];
    v.fCount = 10;
    return v;
}
Transform2D detail::transformOf(const PropValue& v) {
    Transform2D t;
    if (v.fCount < 10) return t;
    for (int i = 0; i < 6; ++i) t.m[std::size_t(i)] = v.f[std::size_t(i)];
    for (int i = 0; i < 4; ++i) t.pct[std::size_t(i)] = v.f[std::size_t(6 + i)];
    return t;
}

Transform2D detail::lerpTransform(const Transform2D& a, const Transform2D& b, f32 k) {
    const Decomposed da = decompose(a.m), db = decompose(b.m);
    f32 d = db.angle - da.angle; // the short way round
    while (d > kPi) d -= 2.f * kPi;
    while (d < -kPi) d += 2.f * kPi;
    const f32 angle = da.angle + d * k, sx = mixf(da.sx, db.sx, k), sy = mixf(da.sy, db.sy, k), sh = mixf(da.shear, db.shear, k);
    const f32 c = std::cos(angle), s = std::sin(angle);
    Transform2D o;
    o.m = {c * sx, s * sx, (c * sh - s) * sy, (s * sh + c) * sy, mixf(a.m[4], b.m[4], k), mixf(a.m[5], b.m[5], k)};
    for (std::size_t i = 0; i < 4; ++i) o.pct[i] = mixf(a.pct[i], b.pct[i], k);
    return o;
}

Affine styleTransform(const ComputedStyle& s, const Rect& box) {
    const Transform2D& t = s.transform;
    const f32 ox = box.x + s.originX.resolve(box.w, 0.f), oy = box.y + s.originY.resolve(box.h, 0.f);
    const Affine m{t.m[0], t.m[1], t.m[2], t.m[3], t.m[4] + t.pct[0] * box.w + t.pct[1] * box.h,
                   t.m[5] + t.pct[2] * box.w + t.pct[3] * box.h};
    return Affine::translation(ox, oy) * m * Affine::translation(-ox, -oy);
}

// ---- filter --------------------------------------------------------------------------------------------------------
bool detail::parseFilterValue(std::string_view text, PropValue& out) {
    text = trim(text);
    FilterList fl;
    bool shadow = false;
    bool ok = true;
    if (text != "none")
        ok = !text.empty() && splitTop(text, ' ', [&](std::string_view fnText) {
            std::string_view name, args;
            if (!splitFunction(fnText, name, args) || fl.count >= kMaxFilters) return false;
            args = trim(args);
            FilterList::Fn fn;
            if (name == "drop-shadow") { // <x> <y> [blur] [color], the colour first or last
                if (shadow) return false;
                f32 len[3] = {0, 0, 0};
                int n = 0;
                Color col = Color::rgb(0x000000);
                const bool good = splitTop(args, ' ', [&](std::string_view w) {
                    std::string_view unit;
                    f32 v;
                    if (parseNumber(w, v, unit) && (unit.empty() || unit == "px" || unit == "dp")) {
                        if (n >= 3) return false;
                        len[n++] = v;
                        return true;
                    }
                    return parseColor(w, col);
                });
                if (!good || n < 2 || len[2] < 0.f) return false;
                shadow = true;
                fn = {FilterKind::DropShadow, len[2]};
                fl.shadowOffset = {len[0], len[1]};
                fl.shadowColor = col;
            } else if (name == "blur") {
                std::string_view unit;
                f32 v = 0.f;
                if (!args.empty() && (!parseNumber(args, v, unit) || !(unit.empty() || unit == "px" || unit == "dp") || v < 0.f)) return false;
                fn = {FilterKind::Blur, v};
            } else if (name == "hue-rotate") {
                f32 v = 0.f;
                if (!args.empty() && !parseAngle(args, v)) return false;
                fn = {FilterKind::HueRotate, v};
            } else {
                static constexpr struct { std::string_view name; FilterKind kind; } kFns[] = {
                    {"brightness", FilterKind::Brightness}, {"contrast", FilterKind::Contrast}, {"grayscale", FilterKind::Grayscale},
                    {"sepia", FilterKind::Sepia}, {"saturate", FilterKind::Saturate}, {"invert", FilterKind::Invert},
                    {"opacity", FilterKind::Opacity}};
                bool found = false;
                for (const auto& k : kFns)
                    if (k.name == name) { fn.kind = k.kind; found = true; }
                if (!found) return false;
                f32 v = 1.f; // the default of every one of them
                std::string_view unit;
                if (!args.empty()) {
                    if (!parseNumber(args, v, unit) || !(unit.empty() || unit == "%") || v < 0.f) return false;
                    if (unit == "%") v /= 100.f;
                }
                fn.amount = v;
            }
            fl.fn[fl.count++] = fn;
            return true;
        });
    if (!ok) return false;
    out = filterValue(fl);
    return true;
}
PropValue detail::filterValue(const FilterList& f) {
    PropValue v;
    v.type = PropType::Raw;
    v.fCount = f.count;
    for (u8 i = 0; i < f.count; ++i) {
        v.f[std::size_t(i) * 2] = f32(u8(f.fn[i].kind));
        v.f[std::size_t(i) * 2 + 1] = f.fn[i].amount;
    }
    v.f[8] = f.shadowOffset.x;
    v.f[9] = f.shadowOffset.y;
    v.color = f.shadowColor;
    return v;
}
FilterList detail::filterOf(const PropValue& v) {
    FilterList f;
    f.count = std::min<u8>(v.fCount, kMaxFilters);
    for (u8 i = 0; i < f.count; ++i) {
        const int k = std::clamp(int(v.f[std::size_t(i) * 2]), 0, int(FilterKind::DropShadow));
        f.fn[i] = {FilterKind(k), v.f[std::size_t(i) * 2 + 1]};
    }
    f.shadowOffset = {v.f[8], v.f[9]};
    f.shadowColor = v.color;
    return f;
}
FilterList detail::lerpFilter(const FilterList& a, const FilterList& b, f32 k) {
    // the same functions in the same order blend; a missing list blends from / to each function's identity
    if (k <= 0.f) return a;
    if (k >= 1.f) return b;
    const FilterList& shape = b.count ? b : a;
    FilterList x = a, y = b;
    auto identityOf = [&](FilterList& o) {
        o = shape;
        for (u8 i = 0; i < o.count; ++i) o.fn[i].amount = identityAmount(o.fn[i].kind);
        o.shadowOffset = {};
        o.shadowColor = shape.shadowColor.withAlpha(0.f);
    };
    if (!a.count && b.count) identityOf(x);
    else if (a.count && !b.count) identityOf(y);
    if (x.count != y.count) return k < 0.5f ? a : b;
    for (u8 i = 0; i < x.count; ++i)
        if (x.fn[i].kind != y.fn[i].kind) return k < 0.5f ? a : b;
    FilterList o = y;
    for (u8 i = 0; i < o.count; ++i) o.fn[i].amount = std::max(o.fn[i].kind == FilterKind::HueRotate ? -1e9f : 0.f, mixf(x.fn[i].amount, y.fn[i].amount, k));
    o.shadowOffset = {mixf(x.shadowOffset.x, y.shadowOffset.x, k), mixf(x.shadowOffset.y, y.shadowOffset.y, k)};
    const Color ca = x.shadowColor, cb = y.shadowColor;
    const f32 al = std::clamp(mixf(ca.a, cb.a, k), 0.f, 1.f);
    o.shadowColor = al > 1e-5f ? Color{std::clamp(mixf(ca.r * ca.a, cb.r * cb.a, k) / al, 0.f, 1.f), std::clamp(mixf(ca.g * ca.a, cb.g * cb.a, k) / al, 0.f, 1.f),
                                       std::clamp(mixf(ca.b * ca.a, cb.b * cb.a, k) / al, 0.f, 1.f), al}
                               : Color{cb.r, cb.g, cb.b, 0.f};
    return o;
}

// ---- clip-path -----------------------------------------------------------------------------------------------------
bool detail::parseClipPathValue(std::string_view text, PropValue& out) {
    text = trim(text);
    ClipShape c;
    if (text == "none") { out = clipShapeValue(c); return true; }
    std::string_view name, args;
    if (!splitFunction(text, name, args)) return false;
    auto put = [&](int k, f32 v, bool pct) {
        c.v[std::size_t(k)] = v;
        if (pct) c.pct = u16(c.pct | (1u << k));
    };
    // "<shape args> at <position>"
    std::string_view at;
    if (const auto pos = args.find(" at "); pos != std::string_view::npos && name != "polygon" && name != "inset") {
        at = trim(args.substr(pos + 4));
        args = trim(args.substr(0, pos));
        if (at.empty()) return false;
    } else if (trim(args).starts_with("at ") && name != "polygon" && name != "inset") {
        at = trim(trim(args).substr(3));
        args = {};
    }
    f32 centre[2] = {50.f, 50.f};
    bool centrePct[2] = {true, true};
    if (!at.empty() && !parsePosition(at, centre, centrePct)) return false;
    if (name == "inset") {
        std::string_view round;
        if (const auto pos = args.find("round"); pos != std::string_view::npos) {
            round = trim(args.substr(pos + 5));
            args = trim(args.substr(0, pos));
        }
        f32 v[4];
        bool pct[4];
        int n = 0;
        if (!splitTop(args, ' ', [&](std::string_view w) { return n < 4 && lengthOrPct(w, v[n], pct[n]) && ++n; }) || n == 0) return false;
        const int idx[4] = {0, n > 1 ? 1 : 0, n > 2 ? 2 : 0, n > 3 ? 3 : (n > 1 ? 1 : 0)};
        for (int k = 0; k < 4; ++k) put(k, v[idx[k]], pct[idx[k]]);
        if (!round.empty()) {
            f32 r;
            bool rp;
            if (!lengthOrPct(round, r, rp) || rp || r < 0.f) return false;
            put(4, r, false);
        }
        c.kind = ClipShapeKind::Inset;
    } else if (name == "circle") {
        args = trim(args);
        f32 r = -1.f;
        bool rp = false;
        if (!args.empty() && args != "closest-side" && (!lengthOrPct(args, r, rp) || r < 0.f)) return false;
        put(0, r, rp);
        put(1, centre[0], centrePct[0]);
        put(2, centre[1], centrePct[1]);
        c.kind = ClipShapeKind::Circle;
    } else if (name == "ellipse") {
        f32 v[2] = {50.f, 50.f};
        bool pct[2] = {true, true};
        int n = 0;
        if (!splitTop(args, ' ', [&](std::string_view w) { return n < 2 && lengthOrPct(w, v[n], pct[n]) && v[n] >= 0.f && ++n; })) return false;
        if (n == 1) return false;
        put(0, v[0], pct[0]);
        put(1, v[1], pct[1]);
        put(2, centre[0], centrePct[0]);
        put(3, centre[1], centrePct[1]);
        c.kind = ClipShapeKind::Ellipse;
    } else if (name == "polygon") {
        int points = 0;
        bool first = true;
        const bool ok = splitTop(args, ',', [&](std::string_view pt) {
            if (first) {
                first = false;
                if (pt == "evenodd") { c.evenOdd = true; return true; }
                if (pt == "nonzero") return true;
            }
            if (points >= kMaxClipPoints) return false;
            int n = 0;
            f32 v[2];
            bool pct[2];
            if (!splitTop(pt, ' ', [&](std::string_view w) { return n < 2 && lengthOrPct(w, v[n], pct[n]) && ++n; }) || n != 2) return false;
            put(points * 2, v[0], pct[0]);
            put(points * 2 + 1, v[1], pct[1]);
            ++points;
            return true;
        });
        if (!ok || points < 3) return false;
        c.count = u8(points);
        c.kind = ClipShapeKind::Polygon;
    } else return false;
    out = clipShapeValue(c);
    return true;
}
PropValue detail::clipShapeValue(const ClipShape& c) {
    PropValue v;
    v.type = PropType::Raw;
    v.e = u8(c.kind);
    v.b = c.evenOdd;
    v.trackCount = c.count;
    v.fCount = 16;
    v.f = c.v;
    v.fPct = c.pct;
    return v;
}
ClipShape detail::clipShapeOf(const PropValue& v) {
    ClipShape c;
    c.kind = ClipShapeKind(std::min<u8>(v.e, u8(ClipShapeKind::Polygon)));
    c.evenOdd = v.b;
    c.count = std::min<u8>(v.trackCount, kMaxClipPoints);
    c.v = v.f;
    c.pct = v.fPct;
    return c;
}

namespace {
struct ShapeGeom {
    Rect inset;
    f32 radius = 0.f;
    Vec2 centre;
    f32 rx = 0.f, ry = 0.f;
};
f32 shapeVal(const ClipShape& s, int k, f32 base) { return (s.pct >> k) & 1u ? s.v[std::size_t(k)] * base / 100.f : s.v[std::size_t(k)]; }
ShapeGeom shapeGeom(const ClipShape& s, const Rect& box) {
    ShapeGeom g;
    switch (s.kind) {
    case ClipShapeKind::Inset: {
        const f32 t = shapeVal(s, 0, box.h), r = shapeVal(s, 1, box.w), b = shapeVal(s, 2, box.h), l = shapeVal(s, 3, box.w);
        g.inset = {box.x + l, box.y + t, std::max(0.f, box.w - l - r), std::max(0.f, box.h - t - b)};
        g.radius = std::min(s.v[4], std::min(g.inset.w, g.inset.h) * 0.5f);
        break;
    }
    case ClipShapeKind::Circle: {
        g.centre = {box.x + shapeVal(s, 1, box.w), box.y + shapeVal(s, 2, box.h)};
        if (s.v[0] < 0.f)
            g.rx = std::min(std::min(g.centre.x - box.x, box.right() - g.centre.x), std::min(g.centre.y - box.y, box.bottom() - g.centre.y));
        else g.rx = shapeVal(s, 0, std::sqrt(box.w * box.w + box.h * box.h) / 1.41421356f);
        g.rx = g.ry = std::max(0.f, g.rx);
        break;
    }
    case ClipShapeKind::Ellipse:
        g.rx = shapeVal(s, 0, box.w);
        g.ry = shapeVal(s, 1, box.h);
        g.centre = {box.x + shapeVal(s, 2, box.w), box.y + shapeVal(s, 3, box.h)};
        break;
    default: break;
    }
    return g;
}
Vec2 polyPoint(const ClipShape& s, const Rect& box, int i) {
    return {box.x + shapeVal(s, i * 2, box.w), box.y + shapeVal(s, i * 2 + 1, box.h)};
}
} // namespace

bool clipShapePath(const ClipShape& s, const Rect& box, Path& out) {
    const ShapeGeom g = shapeGeom(s, box);
    switch (s.kind) {
    case ClipShapeKind::None: return false;
    case ClipShapeKind::Inset: out.addRoundedRect(g.inset, g.radius); return true;
    case ClipShapeKind::Circle:
    case ClipShapeKind::Ellipse: out.addEllipse(g.centre, g.rx, g.ry); return true;
    case ClipShapeKind::Polygon:
        for (int i = 0; i < s.count; ++i) {
            if (i == 0) out.moveTo(polyPoint(s, box, i));
            else out.lineTo(polyPoint(s, box, i));
        }
        out.close();
        return true;
    }
    return false;
}

bool clipShapeContains(const ClipShape& s, const Rect& box, Vec2 p) {
    const ShapeGeom g = shapeGeom(s, box);
    switch (s.kind) {
    case ClipShapeKind::None: return true;
    case ClipShapeKind::Inset: {
        if (!g.inset.contains(p)) return false;
        const f32 r = g.radius;
        const f32 dx = std::max({g.inset.x + r - p.x, p.x - (g.inset.right() - r), 0.f});
        const f32 dy = std::max({g.inset.y + r - p.y, p.y - (g.inset.bottom() - r), 0.f});
        return dx * dx + dy * dy <= r * r || dx == 0.f || dy == 0.f;
    }
    case ClipShapeKind::Circle:
    case ClipShapeKind::Ellipse: {
        if (!(g.rx > 0.f) || !(g.ry > 0.f)) return false;
        const f32 dx = (p.x - g.centre.x) / g.rx, dy = (p.y - g.centre.y) / g.ry;
        return dx * dx + dy * dy <= 1.f;
    }
    case ClipShapeKind::Polygon: {
        int winding = 0, crossings = 0;
        for (int i = 0; i < s.count; ++i) {
            const Vec2 a = polyPoint(s, box, i), b = polyPoint(s, box, (i + 1) % s.count);
            if ((a.y <= p.y) == (b.y <= p.y)) continue;
            const f32 x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
            if (x > p.x) {
                ++crossings;
                winding += b.y > a.y ? 1 : -1;
            }
        }
        return s.evenOdd ? (crossings & 1) != 0 : winding != 0;
    }
    }
    return true;
}

// ---- gradients -----------------------------------------------------------------------------------------------------
bool detail::parseGradientValue(std::string_view text, PropValue& out) {
    text = trim(text);
    GradientSpec g;
    if (text == "none") { out = gradientSpecValue(g); return true; }
    std::string_view name, args;
    if (!splitFunction(text, name, args)) return false;
    if (name == "linear-gradient") g.kind = GradientSpecKind::Linear;
    else if (name == "radial-gradient") g.kind = GradientSpecKind::Radial;
    else return false;
    bool first = true, auto_[kMaxGradientStops] = {};
    const bool ok = splitTop(args, ',', [&](std::string_view part) {
        if (first) {
            first = false;
            if (g.kind == GradientSpecKind::Linear) {
                if (parseAngle(part, g.angle)) return true;
                if (part.starts_with("to ")) {
                    bool top = false, bottom = false, left = false, right = false, good = true;
                    splitTop(part.substr(3), ' ', [&](std::string_view w) {
                        if (w == "top") top = true;
                        else if (w == "bottom") bottom = true;
                        else if (w == "left") left = true;
                        else if (w == "right") right = true;
                        else good = false;
                        return good;
                    });
                    if (!good || (top && bottom) || (left && right) || !(top || bottom || left || right)) return false;
                    const f32 x = right ? 1.f : left ? -1.f : 0.f, y = bottom ? 1.f : top ? -1.f : 0.f;
                    g.angle = std::atan2(x, -y); // corners: the diagonal direction (45 degree steps)
                    if (g.angle < 0.f) g.angle += 2.f * kPi;
                    return true;
                }
            } else if (part == "circle" || part == "ellipse" || part == "farthest-corner" || part == "ellipse farthest-corner" ||
                       part == "ellipse at center" || part == "at center") {
                return true;
            }
        }
        if (g.stopCount >= kMaxGradientStops) return false;
        // "<colour> [<percentage>]"
        std::string_view colour = part, pos;
        if (!part.empty() && part.back() == '%') {
            const auto sp = part.find_last_of(" \t");
            if (sp == std::string_view::npos) return false;
            colour = trim(part.substr(0, sp));
            pos = part.substr(sp + 1);
        }
        if (!parseColor(colour, g.color[g.stopCount])) return false;
        if (pos.empty()) auto_[g.stopCount] = true;
        else {
            f32 v;
            std::string_view unit;
            if (!parseNumber(pos, v, unit)) return false;
            g.offset[g.stopCount] = std::clamp(v / 100.f, 0.f, 1.f);
        }
        ++g.stopCount;
        return true;
    });
    if (!ok || g.stopCount < 2) return false;
    // missing offsets: the ends are 0 and 1, the rest are spread evenly between their neighbours
    if (auto_[0]) { g.offset[0] = 0.f; auto_[0] = false; }
    if (auto_[g.stopCount - 1]) { g.offset[g.stopCount - 1] = 1.f; auto_[g.stopCount - 1] = false; }
    for (int i = 1; i < g.stopCount; ++i) {
        if (!auto_[i]) { g.offset[std::size_t(i)] = std::max(g.offset[std::size_t(i)], g.offset[std::size_t(i - 1)]); continue; }
        int j = i;
        while (auto_[j]) ++j;
        const f32 a = g.offset[std::size_t(i - 1)], b = std::max(g.offset[std::size_t(j)], a);
        for (int k = i; k < j; ++k) {
            g.offset[std::size_t(k)] = a + (b - a) * f32(k - i + 1) / f32(j - i + 1);
            auto_[k] = false;
        }
    }
    out = gradientSpecValue(g);
    return true;
}
PropValue detail::gradientSpecValue(const GradientSpec& g) {
    PropValue v;
    v.type = PropType::Raw;
    v.e = u8(g.kind);
    v.num = g.angle;
    v.fCount = g.stopCount;
    v.trackCount = g.stopCount;
    for (u8 i = 0; i < g.stopCount; ++i) {
        v.tracks[i] = Length::px(g.offset[i]);
        v.f[std::size_t(i) * 4] = g.color[i].r;
        v.f[std::size_t(i) * 4 + 1] = g.color[i].g;
        v.f[std::size_t(i) * 4 + 2] = g.color[i].b;
        v.f[std::size_t(i) * 4 + 3] = g.color[i].a;
    }
    return v;
}
GradientSpec detail::gradientSpecOf(const PropValue& v) {
    GradientSpec g;
    g.kind = GradientSpecKind(std::min<u8>(v.e, u8(GradientSpecKind::Radial)));
    g.angle = v.num;
    g.stopCount = std::min<u8>(v.fCount, kMaxGradientStops);
    for (u8 i = 0; i < g.stopCount; ++i) {
        g.offset[i] = v.tracks[i].value;
        g.color[i] = {v.f[std::size_t(i) * 4], v.f[std::size_t(i) * 4 + 1], v.f[std::size_t(i) * 4 + 2], v.f[std::size_t(i) * 4 + 3]};
    }
    if (g.stopCount < 2) g = {};
    return g;
}

Gradient gradientOf(const GradientSpec& g, const Rect& box, GradientStop* stops) {
    for (u8 i = 0; i < g.stopCount; ++i) stops[i] = {g.offset[i], g.color[i]};
    const std::span<const GradientStop> view(stops, g.stopCount);
    const Vec2 c{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    if (g.kind == GradientSpecKind::Radial) {
        Gradient out = Gradient::radial({0.f, 0.f}, 1.f, view);
        out.transform = Affine::translation(c.x, c.y) * Affine::scaling(std::max(box.w * 0.70710678f, 1e-3f), std::max(box.h * 0.70710678f, 1e-3f));
        return out;
    }
    const f32 dx = std::sin(g.angle), dy = -std::cos(g.angle);
    const f32 half = (std::abs(box.w * dx) + std::abs(box.h * dy)) * 0.5f; // the CSS gradient line: corners get 0 / 1
    return Gradient::linear({c.x - dx * half, c.y - dy * half}, {c.x + dx * half, c.y + dy * half}, view);
}

Color gradientColorAt(const GradientSpec& g, const Rect& box, Vec2 p) {
    if (g.kind == GradientSpecKind::None || g.stopCount == 0) return Color{1.f, 1.f, 1.f, 1.f};
    const Vec2 c{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    f32 t;
    if (g.kind == GradientSpecKind::Radial) {
        const f32 x = (p.x - c.x) / std::max(box.w * 0.70710678f, 1e-3f), y = (p.y - c.y) / std::max(box.h * 0.70710678f, 1e-3f);
        t = std::sqrt(x * x + y * y);
    } else {
        const f32 dx = std::sin(g.angle), dy = -std::cos(g.angle);
        const f32 half = std::max((std::abs(box.w * dx) + std::abs(box.h * dy)) * 0.5f, 1e-6f);
        t = ((p.x - c.x) * dx + (p.y - c.y) * dy) / (2.f * half) + 0.5f;
    }
    t = std::clamp(t, 0.f, 1.f);
    u8 i = 0;
    while (i + 1 < g.stopCount && t >= g.offset[i + 1]) ++i;
    Color a = g.color[i];
    if (i + 1 < g.stopCount && t > g.offset[i]) {
        const f32 span = g.offset[i + 1] - g.offset[i], k = span > 0.f ? (t - g.offset[i]) / span : 1.f;
        const Color b = g.color[i + 1]; // premultiplied blend, as the rasteriser does
        const f32 al = mixf(a.a, b.a, k);
        if (al > 1e-6f)
            a = {mixf(a.r * a.a, b.r * b.a, k) / al, mixf(a.g * a.a, b.g * b.a, k) / al, mixf(a.b * a.a, b.b * b.a, k) / al, al};
        else a = {b.r, b.g, b.b, 0.f};
    }
    return a;
}

// ---- animation lists -----------------------------------------------------------------------------------------------
bool detail::parseAnimationList(Prop p, std::string_view text, PropValue& out) {
    out = {};
    out.type = PropType::Raw;
    int n = 0;
    const bool ok = splitTop(text, ',', [&](std::string_view w) {
        if (n >= kMaxAnimations || w.empty()) return false;
        f32 v = 0.f;
        switch (p) {
        case Prop::AnimationName: v = w == "none" ? 0.f : f32(animationNameId(w)); break;
        case Prop::AnimationDuration:
            if (!parseSeconds(w, v)) return false;
            break;
        case Prop::AnimationDelay:
            if (!parseSeconds(w, v, true)) return false;
            break;
        case Prop::AnimationTimingFunction: {
            Easing e;
            if (!parseEasing(w, e)) return false;
            v = f32(u8(e));
            break;
        }
        case Prop::AnimationIterationCount: {
            std::string_view unit;
            if (w == "infinite") v = std::numeric_limits<f32>::infinity();
            else if (!parseNumber(w, v, unit) || !unit.empty() || v < 0.f) return false;
            break;
        }
        case Prop::AnimationDirection:
            if (w == "normal") v = 0.f;
            else if (w == "reverse") v = 1.f;
            else if (w == "alternate") v = 2.f;
            else if (w == "alternate-reverse") v = 3.f;
            else return false;
            break;
        case Prop::AnimationFillMode:
            if (w == "none") v = 0.f;
            else if (w == "forwards") v = 1.f;
            else if (w == "backwards") v = 2.f;
            else if (w == "both") v = 3.f;
            else return false;
            break;
        case Prop::AnimationPlayState:
            if (w == "running") v = 0.f;
            else if (w == "paused") v = 1.f;
            else return false;
            break;
        default: return false;
        }
        out.f[std::size_t(n++)] = v;
        return true;
    });
    out.fCount = u8(n);
    return ok && n > 0;
}

} // namespace fuse::ui
