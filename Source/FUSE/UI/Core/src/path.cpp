#include "fuse/ui/path.hpp"

#include "svg_scan.hpp"

#include <algorithm>
#include <cmath>

namespace fuse::ui {

namespace {
constexpr f32 kPi = 3.14159265358979f;
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, f32 s) { return {a.x * s, a.y * s}; }
inline f32 dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline f32 cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline f32 len(Vec2 a) { return std::sqrt(dot(a, a)); }
inline bool finite(Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }
} // namespace

// ---- Affine --------------------------------------------------------------------------------------------------------
bool Affine::invert(Affine& out) const {
    const f32 dt = det();
    if (!(std::abs(dt) > 1e-20f) || !std::isfinite(dt)) return false;
    const f32 i = 1.f / dt;
    Affine r;
    r.a = d * i;
    r.b = -b * i;
    r.c = -c * i;
    r.d = a * i;
    r.tx = -(r.a * tx + r.c * ty);
    r.ty = -(r.b * tx + r.d * ty);
    out = r;
    return true;
}

f32 Affine::maxScale() const {
    const f32 s = a * a + b * b + c * c + d * d, dt = det();
    return std::sqrt(std::max(0.f, (s + std::sqrt(std::max(0.f, s * s - 4.f * dt * dt))) * 0.5f));
}

bool Affine::isSimilarity() const {
    const f32 e = 1e-4f * std::max({std::abs(a), std::abs(b), std::abs(c), std::abs(d), 1e-12f});
    return (std::abs(a - d) <= e && std::abs(b + c) <= e) || (std::abs(a + d) <= e && std::abs(b - c) <= e);
}

StrokeStyle& StrokeStyle::setDashes(std::span<const f32> pattern, f32 offset) {
    dashCount = u8(std::min<std::size_t>(pattern.size(), kMaxDashes));
    dashes = {};
    for (u8 i = 0; i < dashCount; ++i) dashes[i] = pattern[i];
    dashOffset = offset;
    return *this;
}

// ---- Path ----------------------------------------------------------------------------------------------------------
void Path::ensureStart() {
    if (m_open) return;
    const Vec2 p = m_points.empty() ? Vec2{} : m_start; // after close() the current point is the sub-path start
    m_verbs.push_back(PathVerb::Move);
    m_points.push_back(p);
    m_start = p;
    m_open = true;
}

Path& Path::moveTo(Vec2 p) {
    m_verbs.push_back(PathVerb::Move);
    m_points.push_back(p);
    m_start = p;
    m_open = true;
    return *this;
}
Path& Path::lineTo(Vec2 p) {
    ensureStart();
    m_verbs.push_back(PathVerb::Line);
    m_points.push_back(p);
    return *this;
}
Path& Path::quadTo(Vec2 c, Vec2 p) {
    ensureStart();
    m_verbs.push_back(PathVerb::Quad);
    m_points.push_back(c);
    m_points.push_back(p);
    return *this;
}
Path& Path::cubicTo(Vec2 c1, Vec2 c2, Vec2 p) {
    ensureStart();
    m_verbs.push_back(PathVerb::Cubic);
    m_points.push_back(c1);
    m_points.push_back(c2);
    m_points.push_back(p);
    return *this;
}
Path& Path::close() {
    if (!m_open) return *this;
    m_verbs.push_back(PathVerb::Close);
    m_open = false;
    return *this;
}

namespace {
// Arc of the ellipse (rx, ry) rotated by `rot` around `c`, from angle `start` over `sweep`, as cubics of <= 90 degrees.
// The current point is assumed to be the arc's start.
void arcCubics(Path& path, Vec2 c, f32 rx, f32 ry, f32 rot, f32 start, f32 sweep) {
    const int n = std::clamp(int(std::ceil(std::abs(sweep) / (kPi * 0.5f) - 1e-4f)), 1, 8);
    const f32 delta = sweep / f32(n), k = 4.f / 3.f * std::tan(delta * 0.25f);
    const f32 cr = std::cos(rot), sr = std::sin(rot);
    auto map = [&](f32 ux, f32 uy) {
        const f32 x = ux * rx, y = uy * ry;
        return Vec2{c.x + x * cr - y * sr, c.y + x * sr + y * cr};
    };
    f32 a0 = start;
    for (int i = 0; i < n; ++i) {
        const f32 a1 = start + delta * f32(i + 1);
        const f32 c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
        path.cubicTo(map(c0 - k * s0, s0 + k * c0), map(c1 + k * s1, s1 - k * c1), map(c1, s1));
        a0 = a1;
    }
}
} // namespace

Path& Path::addArc(Vec2 centre, f32 rx, f32 ry, f32 start, f32 sweep, bool connect) {
    const Vec2 p0{centre.x + rx * std::cos(start), centre.y + ry * std::sin(start)};
    if (connect && m_open) lineTo(p0);
    else moveTo(p0);
    if (sweep != 0.f) arcCubics(*this, centre, rx, ry, 0.f, start, std::clamp(sweep, -2.f * kPi, 2.f * kPi));
    return *this;
}

Path& Path::arcTo(f32 rx, f32 ry, f32 xRotation, bool largeArc, bool sweep, Vec2 to) {
    ensureStart();
    const Vec2 from = m_points.back();
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (from == to) return *this;
    if (rx < 1e-6f || ry < 1e-6f) return lineTo(to);
    // SVG implementation notes F.6.5: endpoint -> centre parameterisation
    const f32 cr = std::cos(xRotation), sr = std::sin(xRotation);
    const f32 dx = (from.x - to.x) * 0.5f, dy = (from.y - to.y) * 0.5f;
    const f32 x1 = cr * dx + sr * dy, y1 = -sr * dx + cr * dy;
    const f32 lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1.f) {
        const f32 s = std::sqrt(lambda);
        rx *= s;
        ry *= s;
    }
    const f32 num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const f32 den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    f32 co = den > 0.f ? std::sqrt(std::max(0.f, num / den)) : 0.f;
    if (largeArc == sweep) co = -co;
    const f32 cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
    const Vec2 c{cr * cxp - sr * cyp + (from.x + to.x) * 0.5f, sr * cxp + cr * cyp + (from.y + to.y) * 0.5f};
    const f32 a0 = std::atan2((y1 - cyp) / ry, (x1 - cxp) / rx);
    const f32 a1 = std::atan2((-y1 - cyp) / ry, (-x1 - cxp) / rx);
    f32 da = a1 - a0;
    if (sweep && da < 0.f) da += 2.f * kPi;
    if (!sweep && da > 0.f) da -= 2.f * kPi;
    if (!std::isfinite(da) || !finite(c)) return lineTo(to);
    arcCubics(*this, c, rx, ry, xRotation, a0, da);
    m_points.back() = to; // exact end point
    return *this;
}

Path& Path::addRect(const Rect& r) {
    moveTo({r.x, r.y});
    lineTo({r.right(), r.y});
    lineTo({r.right(), r.bottom()});
    lineTo({r.x, r.bottom()});
    return close();
}

Path& Path::addRoundedRect(const Rect& r, const std::array<f32, 4>& radii) {
    const f32 lim = std::max(0.f, std::min(r.w, r.h) * 0.5f);
    const f32 tl = std::clamp(radii[0], 0.f, lim), tr = std::clamp(radii[1], 0.f, lim);
    const f32 br = std::clamp(radii[2], 0.f, lim), bl = std::clamp(radii[3], 0.f, lim);
    if (tl <= 0.f && tr <= 0.f && br <= 0.f && bl <= 0.f) return addRect(r);
    constexpr f32 k = 0.44771525f; // 1 - 4/3 (sqrt 2 - 1): control point distance from the corner, per unit radius
    const f32 x0 = r.x, y0 = r.y, x1 = r.right(), y1 = r.bottom();
    moveTo({x0 + tl, y0});
    lineTo({x1 - tr, y0});
    if (tr > 0.f) cubicTo({x1 - tr * k, y0}, {x1, y0 + tr * k}, {x1, y0 + tr});
    lineTo({x1, y1 - br});
    if (br > 0.f) cubicTo({x1, y1 - br * k}, {x1 - br * k, y1}, {x1 - br, y1});
    lineTo({x0 + bl, y1});
    if (bl > 0.f) cubicTo({x0 + bl * k, y1}, {x0, y1 - bl * k}, {x0, y1 - bl});
    lineTo({x0, y0 + tl});
    if (tl > 0.f) cubicTo({x0, y0 + tl * k}, {x0 + tl * k, y0}, {x0 + tl, y0});
    return close();
}

Path& Path::addEllipse(Vec2 c, f32 rx, f32 ry) {
    moveTo({c.x + rx, c.y});
    arcCubics(*this, c, rx, ry, 0.f, 0.f, 2.f * kPi);
    m_points.back() = {c.x + rx, c.y};
    return close();
}

Path& Path::addPolyline(std::span<const Vec2> pts, bool closed) {
    if (pts.empty()) return *this;
    moveTo(pts[0]);
    for (std::size_t i = 1; i < pts.size(); ++i) lineTo(pts[i]);
    if (closed) close();
    return *this;
}

Path& Path::addPath(const Path& o, const Affine* xf) {
    const std::size_t first = m_points.size();
    m_verbs.insert(m_verbs.end(), o.m_verbs.begin(), o.m_verbs.end());
    m_points.insert(m_points.end(), o.m_points.begin(), o.m_points.end());
    if (xf)
        for (std::size_t i = first; i < m_points.size(); ++i) m_points[i] = xf->apply(m_points[i]);
    m_start = xf ? xf->apply(o.m_start) : o.m_start;
    m_open = o.m_open;
    return *this;
}

void Path::transform(const Affine& xf) {
    for (Vec2& p : m_points) p = xf.apply(p);
    m_start = xf.apply(m_start);
}

Rect Path::bounds() const {
    if (m_points.empty()) return {};
    f32 x0 = m_points[0].x, y0 = m_points[0].y, x1 = x0, y1 = y0;
    for (const Vec2& p : m_points) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

std::expected<Path, PathParseError> Path::fromSvg(std::string_view d) {
    Path path;
    const char* const begin = d.data();
    const char* p = begin;
    const char* const end = begin + d.size();
    auto fail = [&]() { return std::unexpected(PathParseError{u32(p - begin)}); };
    char cmd = 0;
    Vec2 cur{}, start{}, lastCtrl{};
    char lastCmd = 0;
    bool first = true;
    using detail::svgNumber;
    using detail::svgSkipSep;
    auto num = [&](f32& v) {
        svgSkipSep(p, end);
        return svgNumber(p, end, v);
    };
    auto flag = [&](bool& v) {
        svgSkipSep(p, end);
        if (p >= end || (*p != '0' && *p != '1')) return false;
        v = *p++ == '1';
        return true;
    };
    for (;;) {
        while (p < end && detail::svgSpace(*p)) ++p;
        if (p >= end) break;
        const char ch = *p;
        const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (letter) {
            cmd = ch;
            ++p;
        } else {
            if (!cmd || cmd == 'Z' || cmd == 'z') return fail(); // numbers need a command to repeat
            if (ch == ',') { // a separator before a repeated argument group
                svgSkipSep(p, end);
                if (p >= end) return fail();
            }
            if (cmd == 'M') cmd = 'L'; // implicit lineto after a moveto
            else if (cmd == 'm') cmd = 'l';
        }
        if (first && cmd != 'M' && cmd != 'm') return fail();
        first = false;
        const bool rel = cmd >= 'a';
        const Vec2 base = rel ? cur : Vec2{};
        f32 a[6]{};
        switch (cmd) {
        case 'M': case 'm':
            if (!num(a[0]) || !num(a[1])) return fail();
            cur = {base.x + a[0], base.y + a[1]};
            start = cur;
            path.moveTo(cur);
            break;
        case 'L': case 'l':
            if (!num(a[0]) || !num(a[1])) return fail();
            cur = {base.x + a[0], base.y + a[1]};
            path.lineTo(cur);
            break;
        case 'H': case 'h':
            if (!num(a[0])) return fail();
            cur.x = base.x + a[0];
            path.lineTo(cur);
            break;
        case 'V': case 'v':
            if (!num(a[0])) return fail();
            cur.y = base.y + a[0];
            path.lineTo(cur);
            break;
        case 'C': case 'c': {
            for (f32& v : a)
                if (!num(v)) return fail();
            const Vec2 c1{base.x + a[0], base.y + a[1]}, c2{base.x + a[2], base.y + a[3]};
            cur = {base.x + a[4], base.y + a[5]};
            path.cubicTo(c1, c2, cur);
            lastCtrl = c2;
            break;
        }
        case 'S': case 's': {
            for (int i = 0; i < 4; ++i)
                if (!num(a[i])) return fail();
            const bool smooth = lastCmd == 'C' || lastCmd == 'c' || lastCmd == 'S' || lastCmd == 's';
            const Vec2 c1 = smooth ? Vec2{2.f * cur.x - lastCtrl.x, 2.f * cur.y - lastCtrl.y} : cur;
            const Vec2 c2{base.x + a[0], base.y + a[1]};
            cur = {base.x + a[2], base.y + a[3]};
            path.cubicTo(c1, c2, cur);
            lastCtrl = c2;
            break;
        }
        case 'Q': case 'q': {
            for (int i = 0; i < 4; ++i)
                if (!num(a[i])) return fail();
            const Vec2 c{base.x + a[0], base.y + a[1]};
            cur = {base.x + a[2], base.y + a[3]};
            path.quadTo(c, cur);
            lastCtrl = c;
            break;
        }
        case 'T': case 't': {
            if (!num(a[0]) || !num(a[1])) return fail();
            const bool smooth = lastCmd == 'Q' || lastCmd == 'q' || lastCmd == 'T' || lastCmd == 't';
            const Vec2 c = smooth ? Vec2{2.f * cur.x - lastCtrl.x, 2.f * cur.y - lastCtrl.y} : cur;
            cur = {base.x + a[0], base.y + a[1]};
            path.quadTo(c, cur);
            lastCtrl = c;
            break;
        }
        case 'A': case 'a': {
            bool large = false, sweep = false;
            if (!num(a[0]) || !num(a[1]) || !num(a[2]) || !flag(large) || !flag(sweep) || !num(a[3]) || !num(a[4])) return fail();
            const Vec2 to{base.x + a[3], base.y + a[4]};
            path.arcTo(a[0], a[1], a[2] * kPi / 180.f, large, sweep, to);
            cur = to;
            break;
        }
        case 'Z': case 'z':
            path.close();
            cur = start;
            break;
        default: --p; return fail();
        }
        lastCmd = cmd;
    }
    return path;
}

// ---- Flattening ----------------------------------------------------------------------------------------------------
std::size_t Polyline::segmentCount() const {
    std::size_t n = 0;
    for (const Contour& c : contours) n += c.count < 2 ? 0 : (c.closed ? c.count : c.count - 1);
    return n;
}

void flattenPath(std::span<const PathVerb> verbs, std::span<const Vec2> pts, f32 tolerance, Polyline& out, const Affine* xf) {
    const f32 tol = std::max(tolerance, 1e-4f);
    std::size_t pi = 0;
    u32 first = u32(out.points.size());
    bool drawn = false; // the open contour has a drawing verb (a lone Move produces nothing)
    auto finish = [&](bool closed) {
        const u32 n = u32(out.points.size()) - first;
        if (n >= 1 && drawn) out.contours.push_back({first, n, closed});
        else out.points.resize(first);
        first = u32(out.points.size());
        drawn = false;
    };
    auto at = [&](std::size_t i) { return xf ? xf->apply(pts[i]) : pts[i]; };
    Vec2 cur{}, start{};
    for (const PathVerb v : verbs) {
        switch (v) {
        case PathVerb::Move:
            if (pi + 1 > pts.size()) return finish(false);
            finish(false);
            cur = start = at(pi++);
            out.points.push_back(cur);
            break;
        case PathVerb::Line:
            if (pi + 1 > pts.size()) return finish(false);
            if (out.points.size() == first) out.points.push_back(cur);
            cur = at(pi++);
            out.points.push_back(cur);
            drawn = true;
            break;
        case PathVerb::Quad: {
            if (pi + 2 > pts.size()) return finish(false);
            if (out.points.size() == first) out.points.push_back(cur);
            const Vec2 c = at(pi), p = at(pi + 1);
            pi += 2;
            const f32 dd = len(cur - c * 2.f + p);
            const int n = std::isfinite(dd) ? std::clamp(int(std::ceil(std::sqrt(dd / (4.f * tol)))), 1, 512) : 1;
            for (int i = 1; i <= n; ++i) {
                const f32 t = f32(i) / f32(n), u = 1.f - t;
                out.points.push_back(i == n ? p : cur * (u * u) + c * (2.f * u * t) + p * (t * t));
            }
            cur = p;
            drawn = true;
            break;
        }
        case PathVerb::Cubic: {
            if (pi + 3 > pts.size()) return finish(false);
            if (out.points.size() == first) out.points.push_back(cur);
            const Vec2 c1 = at(pi), c2 = at(pi + 1), p = at(pi + 2);
            pi += 3;
            const f32 dd = std::max(len(cur - c1 * 2.f + c2), len(c1 - c2 * 2.f + p));
            const int n = std::isfinite(dd) ? std::clamp(int(std::ceil(std::sqrt(dd * 0.75f / tol))), 1, 512) : 1;
            for (int i = 1; i <= n; ++i) {
                const f32 t = f32(i) / f32(n), u = 1.f - t;
                out.points.push_back(i == n ? p : cur * (u * u * u) + c1 * (3.f * u * u * t) + c2 * (3.f * u * t * t) + p * (t * t * t));
            }
            cur = p;
            drawn = true;
            break;
        }
        case PathVerb::Close:
            if (out.points.size() == first) break; // nothing open
            drawn = true;
            if (out.points.size() - first > 1 && out.points.back() == out.points[first]) out.points.pop_back();
            finish(true);
            cur = start;
            break;
        }
    }
    finish(false);
}

// ---- Dashing -------------------------------------------------------------------------------------------------------
void dashPolyline(const Polyline& in, std::span<const f32> dashes, f32 offset, Polyline& out) {
    const std::size_t n = dashes.size() % 2 ? dashes.size() * 2 : dashes.size();
    f32 total = 0.f;
    bool ok = !dashes.empty() && std::isfinite(offset);
    for (std::size_t i = 0; i < n && ok; ++i) {
        const f32 d = dashes[i % dashes.size()];
        if (!(d >= 0.f) || !std::isfinite(d)) ok = false;
        total += d;
    }
    if (!ok || !(total > 1e-4f)) { // solid
        for (const Polyline::Contour& c : in.contours) {
            const u32 first = u32(out.points.size());
            out.points.insert(out.points.end(), in.points.begin() + c.first, in.points.begin() + c.first + c.count);
            out.contours.push_back({first, c.count, c.closed});
        }
        return;
    }
    // pattern position at the start of every contour
    f32 phase = std::fmod(offset, total);
    if (phase < 0.f) phase += total;
    std::size_t startIdx = 0;
    while (phase >= dashes[startIdx % dashes.size()] && startIdx < n) {
        phase -= dashes[startIdx % dashes.size()];
        ++startIdx;
    }
    if (startIdx >= n) { startIdx = 0; phase = 0.f; }
    constexpr std::size_t kMaxPieces = 1u << 18; // a tiny dash on a huge path must not explode
    std::size_t pieces = 0;
    for (const Polyline::Contour& c : in.contours) {
        if (c.count < 2) continue; // a zero-length sub-path has no dashes
        std::size_t idx = startIdx;
        f32 remain = dashes[idx % dashes.size()] - phase;
        bool on = idx % 2 == 0;
        u32 first = u32(out.points.size());
        const u32 segs = c.closed ? c.count : c.count - 1;
        if (on) out.points.push_back(in.points[c.first]);
        for (u32 s = 0; s < segs && pieces < kMaxPieces; ++s) {
            const Vec2 a = in.points[c.first + s], b = in.points[c.first + (s + 1) % c.count];
            const f32 l = len(b - a);
            if (!(l > 0.f) || !std::isfinite(l)) continue;
            f32 pos = 0.f;
            while (l - pos > remain && pieces < kMaxPieces) { // the dash boundary falls inside this segment
                pos += remain;
                const Vec2 q = a + (b - a) * (pos / l);
                if (on) {
                    out.points.push_back(q);
                    out.contours.push_back({first, u32(out.points.size()) - first, false});
                    ++pieces;
                    first = u32(out.points.size());
                } else {
                    first = u32(out.points.size());
                    out.points.push_back(q);
                }
                on = !on;
                ++idx;
                remain = dashes[idx % dashes.size()];
                if (idx >= n) idx -= n;
            }
            remain -= l - pos;
            if (on) out.points.push_back(b);
        }
        if (on && out.points.size() - first >= 2) out.contours.push_back({first, u32(out.points.size()) - first, false});
        else out.points.resize(first);
    }
}

// ---- Stroking ------------------------------------------------------------------------------------------------------
namespace {
struct Stroker {
    Polyline& out;
    f32 hw, tol, stepAngle;
    const StrokeStyle& st;

    // appends a closed polygon, reversed when needed so that every piece winds the same way
    void poly(const Vec2* p, int n) {
        f32 area = 0.f; // relative to p[0], to keep precision far from the origin
        for (int i = 1; i + 1 < n; ++i) area += cross(p[i] - p[0], p[i + 1] - p[0]);
        if (!(std::abs(area) > 1e-9f)) return;
        const u32 first = u32(out.points.size());
        if (area > 0.f) out.points.insert(out.points.end(), p, p + n);
        else
            for (int i = n - 1; i >= 0; --i) out.points.push_back(p[i]);
        out.contours.push_back({first, u32(n), true});
    }
    // fan around `c` from direction `from` (unit) rotated over `angle` radians, radius hw
    void fan(Vec2 c, Vec2 from, f32 angle) {
        const int steps = std::clamp(int(std::ceil(std::abs(angle) / stepAngle)), 1, 96);
        Vec2 buf[100];
        int n = 0;
        buf[n++] = c;
        const f32 rIn = hw * arcComp(std::abs(angle) / f32(steps));
        for (int i = 0; i <= steps; ++i) {
            const f32 t = angle * f32(i) / f32(steps), co = std::cos(t), si = std::sin(t);
            const f32 r = (i == 0 || i == steps) ? hw : rIn; // the ends meet the neighbouring pieces exactly
            buf[n++] = {c.x + r * (from.x * co - from.y * si), c.y + r * (from.x * si + from.y * co)};
        }
        poly(buf, n);
    }
    // Radius factor that makes a polygonal arc with steps of `theta` cover the same area as the true arc (the
    // vertices move out by as much as the chords cut in), so round caps, joins and dots do not come out thin.
    static f32 arcComp(f32 theta) { return theta > 1e-3f ? std::sqrt(theta / std::sin(theta)) : 1.f; }
    void join(Vec2 p, Vec2 d0, Vec2 d1) {
        const f32 cr = cross(d0, d1), dt = dot(d0, d1);
        if (std::abs(cr) < 1e-6f && dt > 0.f) return; // collinear
        const f32 s = cr > 0.f ? -1.f : 1.f;            // the outer side is opposite to the turn
        const Vec2 n0{-d0.y * s, d0.x * s}, n1{-d1.y * s, d1.x * s};
        const Vec2 a = p + n0 * hw, b = p + n1 * hw;
        const f32 cosHalf = std::sqrt(std::max(0.f, (1.f + dt) * 0.5f)); // cos of half the turn angle
        LineJoin j = st.join;
        if (hw * (1.f - cosHalf) < 0.1f * tol) j = LineJoin::Bevel; // a gap far below a pixel: any join looks the same
        if (j == LineJoin::Miter && (cosHalf < 1e-4f || 1.f / cosHalf > std::max(st.miterLimit, 1.f))) j = LineJoin::Bevel;
        if (j == LineJoin::Round) {
            fan(p, n0, std::atan2(cross(n0, n1), dot(n0, n1)));
        } else if (j == LineJoin::Miter) {
            const Vec2 m = p + (n0 + n1) * (hw / (1.f + dt));
            const Vec2 q[4]{p, a, m, b};
            poly(q, 4);
        } else {
            const Vec2 q[3]{p, a, b};
            poly(q, 3);
        }
    }
    void cap(Vec2 p, Vec2 d) { // d points out of the line
        const Vec2 n{-d.y, d.x};
        if (st.cap == LineCap::Square) {
            const Vec2 q[4]{p + n * hw, p + n * hw + d * hw, p - n * hw + d * hw, p - n * hw};
            poly(q, 4);
        } else if (st.cap == LineCap::Round) {
            // half disc: the fan's first / last points lie on the segment's end edge
            const int steps = std::clamp(int(std::ceil(kPi / stepAngle)), 2, 96);
            Vec2 buf[100];
            int k = 0;
            const f32 rIn = hw * arcComp(kPi / f32(steps));
            for (int i = 0; i <= steps; ++i) {
                const f32 t = kPi * f32(i) / f32(steps), co = std::cos(t), si = std::sin(t);
                const f32 r = (i == 0 || i == steps) ? hw : rIn;
                buf[k++] = p + n * (r * co) + d * (r * si);
            }
            poly(buf, k);
        }
    }
    void dot_(Vec2 p) { // zero-length sub-path
        if (st.cap == LineCap::Square) {
            const Vec2 q[4]{{p.x - hw, p.y - hw}, {p.x + hw, p.y - hw}, {p.x + hw, p.y + hw}, {p.x - hw, p.y + hw}};
            poly(q, 4);
        } else if (st.cap == LineCap::Round) {
            const int steps = std::clamp(int(std::ceil(2.f * kPi / stepAngle)), 4, 96);
            Vec2 buf[100];
            const f32 r = hw * arcComp(2.f * kPi / f32(steps));
            for (int i = 0; i < steps; ++i) {
                const f32 t = 2.f * kPi * f32(i) / f32(steps);
                buf[i] = {p.x + r * std::cos(t), p.y + r * std::sin(t)};
            }
            poly(buf, steps);
        }
    }
    void contour(const Vec2* pts, u32 count, bool closed, std::vector<Vec2>& tmp) {
        tmp.clear();
        for (u32 i = 0; i < count; ++i) {
            if (!finite(pts[i])) return;
            if (tmp.empty() || len(pts[i] - tmp.back()) > 1e-6f) tmp.push_back(pts[i]);
        }
        if (closed && tmp.size() > 1 && len(tmp.back() - tmp.front()) <= 1e-6f) tmp.pop_back();
        if (tmp.empty()) return;
        if (tmp.size() == 1) return dot_(tmp[0]);
        const u32 n = u32(tmp.size());
        if (n == 2) closed = false; // a closed two-point contour is a line there and back
        const u32 segs = closed ? n : n - 1;
        auto dir = [&](u32 s) {
            const Vec2 v = tmp[(s + 1) % n] - tmp[s];
            return v * (1.f / len(v));
        };
        auto segLen = [&](u32 s) { return len(tmp[(s + 1) % n] - tmp[s]); };
        // The joint between segments `sa` and `sb` at their shared vertex `p`. On the inner side of the turn the two
        // segment bodies would overlap; when the turn is gentle enough for the segments' lengths, both are cut at the
        // inner miter point instead (`inner`), so the pieces only abut and the summed coverage is the exact union.
        struct Joint {
            bool cut = false;
            f32 side = 1.f; ///< +1: the outer side is the left normal (-d.y, d.x), -1: the right
            Vec2 inner;
        };
        auto jointOf = [&](u32 sa, u32 sb, Vec2 p) {
            Joint j;
            const Vec2 d0 = dir(sa), d1 = dir(sb);
            const f32 cr = cross(d0, d1), dt = dot(d0, d1);
            if (std::abs(cr) < 1e-6f || dt < -0.99f) return j;
            j.side = cr > 0.f ? -1.f : 1.f;
            const f32 pull = hw * std::abs(cr) / (1.f + dt); // hw tan(turn / 2): how far the inner corner moves back
            if (pull > 0.5f * std::min(segLen(sa), segLen(sb))) return j;
            const Vec2 n0{-d0.y, d0.x}, n1{-d1.y, d1.x};
            j.inner = p - (n0 + n1) * (j.side * hw / (1.f + dot(n0, n1)));
            j.cut = true;
            return j;
        };
        for (u32 s = 0; s < segs; ++s) {
            const Vec2 a = tmp[s], b = tmp[(s + 1) % n], d = dir(s), nn{-d.y * hw, d.x * hw};
            const bool hasPrev = s > 0 || closed, hasNext = s + 1 < segs || closed;
            const Joint j0 = hasPrev ? jointOf(s == 0 ? segs - 1 : s - 1, s, a) : Joint{};
            const Joint j1 = hasNext ? jointOf(s, s + 1 < segs ? s + 1 : 0, b) : Joint{};
            Vec2 q[6];
            int k = 0;
            q[k++] = j0.cut && j0.side < 0.f ? j0.inner : a + nn;
            q[k++] = j1.cut && j1.side < 0.f ? j1.inner : b + nn;
            if (j1.cut) q[k++] = b;
            q[k++] = j1.cut && j1.side > 0.f ? j1.inner : b - nn;
            q[k++] = j0.cut && j0.side > 0.f ? j0.inner : a - nn;
            if (j0.cut) q[k++] = a;
            poly(q, k);
            if (hasNext) join(b, d, dir(s + 1 < segs ? s + 1 : 0));
        }
        if (!closed) {
            cap(tmp[0], dir(0) * -1.f);
            cap(tmp[n - 1], dir(segs - 1));
        }
    }
};
} // namespace

void strokePolyline(const Polyline& in, const StrokeStyle& style, f32 tolerance, Polyline& out) {
    if (!(style.width > 0.f) || !std::isfinite(style.width)) return;
    thread_local Polyline dashed;
    thread_local std::vector<Vec2> tmp;
    const Polyline* src = &in;
    if (style.dashCount > 0) {
        dashed.clear();
        dashPolyline(in, {style.dashes.data(), style.dashCount}, style.dashOffset, dashed);
        src = &dashed;
    }
    const f32 hw = style.width * 0.5f, tol = std::max(tolerance, 1e-4f);
    const f32 stepAngle = hw > tol ? 2.f * std::acos(1.f - tol / hw) : kPi * 0.5f;
    Stroker s{out, hw, tol, std::max(stepAngle, 0.02f), style};
    for (const Polyline::Contour& c : src->contours) s.contour(src->points.data() + c.first, c.count, c.closed, tmp);
}

// ---- Coverage ------------------------------------------------------------------------------------------------------
namespace {
// Adds the signed area of the trapezoid right of the edge piece (xl..xr within one pixel row, height d) to `a`.
inline void coverSpan(f32* a, f32 xl, f32 xr, f32 d) {
    const f32 x0floor = std::floor(xl);
    const int x0i = int(x0floor);
    const f32 x1ceil = std::ceil(xr);
    const int x1i = int(x1ceil);
    if (x1i <= x0i + 1) {
        const f32 xmf = 0.5f * (xl + xr) - x0floor;
        a[x0i] += d - d * xmf;
        a[x0i + 1] += d * xmf;
        return;
    }
    const f32 s = 1.f / (xr - xl);
    const f32 x0f = xl - x0floor;
    const f32 a0 = 0.5f * s * (1.f - x0f) * (1.f - x0f);
    const f32 x1f = xr - x1ceil + 1.f;
    const f32 am = 0.5f * s * x1f * x1f;
    a[x0i] += d * a0;
    if (x1i == x0i + 2) {
        a[x0i + 1] += d * (1.f - a0 - am);
    } else {
        const f32 a1 = s * (1.5f - x0f);
        a[x0i + 1] += d * (a1 - a0);
        for (int xi = x0i + 2; xi < x1i - 1; ++xi) a[xi] += d * s;
        const f32 a2 = a1 + f32(x1i - x0i - 3) * s;
        a[x1i - 1] += d * (1.f - a2 - am);
    }
    a[x1i] += d * am;
}
} // namespace

void rasterizeCoverageRows(const Polyline& poly, FillRule rule, int x0, int w, int rowBegin, int rowEnd, CoverageRowFn fn,
                           void* user, std::vector<f32>& acc) {
    if (w <= 0 || rowEnd <= rowBegin) return;
    // Bands of kBand rows: the accumulation buffer stays small whatever the path's height, and each row only pays
    // for the columns its edges touch (lo..hi), so a thin diagonal line across the window is cheap.
    constexpr int kBand = 16;
    const int stride = w + 2;
    acc.assign(std::size_t(kBand) * std::size_t(stride), 0.f);
    thread_local std::vector<u8> rowBuf;
    rowBuf.resize(std::size_t(w));
    const f32 fw = f32(w), fx0 = f32(x0);
    // vertical extent of every contour: a band skips the contours that do not reach it (stroke outlines are thousands
    // of small polygons)
    thread_local std::vector<Vec2> spans; // (min y, max y)
    spans.resize(poly.contours.size());
    for (std::size_t ci = 0; ci < poly.contours.size(); ++ci) {
        const Polyline::Contour& c = poly.contours[ci];
        f32 lo = 1.f, hi = 0.f; // empty
        for (u32 i = 0; i < c.count && c.count >= 3; ++i) {
            const f32 y = poly.points[c.first + i].y;
            if (i == 0) lo = hi = y;
            lo = std::min(lo, y);
            hi = std::max(hi, y);
        }
        spans[ci] = {lo, hi};
    }
    for (int band = rowBegin; band < rowEnd; band += kBand) {
        const int bandEnd = std::min(rowEnd, band + kBand);
        int lo[kBand], hi[kBand];
        for (int k = 0; k < kBand; ++k) {
            lo[k] = stride;
            hi[k] = -1;
        }
        for (std::size_t ci = 0; ci < poly.contours.size(); ++ci) {
            const Polyline::Contour& c = poly.contours[ci];
            if (c.count < 3 || !(spans[ci].y > f32(band)) || !(spans[ci].x < f32(bandEnd))) continue;
            const Vec2* pts = poly.points.data() + c.first;
            for (u32 i = 0; i < c.count; ++i) {
                Vec2 p0 = pts[i], p1 = pts[i + 1 == c.count ? 0 : i + 1];
                if (p0.y == p1.y) continue;
                f32 dir = 1.f;
                if (p0.y > p1.y) {
                    std::swap(p0, p1);
                    dir = -1.f;
                }
                if (!(p1.y > f32(band)) || !(p0.y < f32(bandEnd))) continue; // (also skips NaN)
                if (!finite(p0) || !finite(p1)) continue;
                const f32 ax = p0.x - fx0, dxdy = (p1.x - p0.x) / (p1.y - p0.y);
                const int r0 = std::max(band, int(std::floor(p0.y))), r1 = std::min(bandEnd, int(std::ceil(p1.y)));
                for (int r = r0; r < r1; ++r) {
                    // the piece of the edge inside row r, from the edge's own end point (no state carried between rows:
                    // a row's coverage does not depend on which rows are rasterised)
                    const f32 yt = std::max(p0.y, f32(r)), yb = std::min(p1.y, f32(r + 1));
                    const f32 dy = yb - yt;
                    if (!(dy > 0.f)) continue;
                    const f32 xt = ax + (yt - p0.y) * dxdy, xb = ax + (yb - p0.y) * dxdy;
                    f32 d = dir * dy, xl = std::min(xt, xb), xr = std::max(xt, xb);
                    const int k = r - band;
                    f32* row = acc.data() + std::size_t(k) * std::size_t(stride);
                    if (xr <= 0.f) { // left of the window: the whole row to its right is covered
                        row[0] += d;
                        lo[k] = 0;
                        hi[k] = std::max(hi[k], 0);
                        continue;
                    }
                    if (xl >= fw) continue;
                    if (xl < 0.f) {
                        const f32 f = -xl / (xr - xl);
                        row[0] += d * f;
                        d *= 1.f - f;
                        xl = 0.f;
                    }
                    if (xr > fw) {
                        d *= 1.f - (xr - fw) / (xr - xl);
                        xr = fw;
                    }
                    lo[k] = std::min(lo[k], int(xl));
                    hi[k] = std::max(hi[k], int(std::ceil(xr)));
                    coverSpan(row, xl, xr, d);
                }
            }
        }
        auto cover = [rule](f32 sum) {
            f32 cov = std::abs(sum);
            if (rule == FillRule::EvenOdd) {
                cov -= 2.f * std::floor(cov * 0.5f);
                if (cov > 1.f) cov = 2.f - cov;
            } else if (cov > 1.f) {
                cov = 1.f;
            }
            return u8(cov * 255.f + 0.5f);
        };
        for (int r = band; r < bandEnd; ++r) {
            const int k = r - band;
            if (hi[k] < lo[k]) {
                fn(user, r, x0, x0, rowBuf.data());
                continue;
            }
            f32* row = acc.data() + std::size_t(k) * std::size_t(stride);
            const int xb = lo[k];
            int xe = std::min(hi[k] + 1, w);
            f32 sum = 0.f;
            for (int x = xb; x < xe; ++x) {
                sum += row[x];
                rowBuf[std::size_t(x)] = cover(sum);
            }
            if (xe < w) { // past the last edge: constant (non-zero when the shape continues beyond the window)
                const u8 tail = cover(sum);
                if (tail) {
                    std::fill(rowBuf.begin() + xe, rowBuf.end(), tail);
                    xe = w;
                }
            }
            for (int x = xb; x <= std::min(hi[k], stride - 1); ++x) row[x] = 0.f; // the buffer is all zero again
            fn(user, r, x0 + xb, x0 + xe, rowBuf.data() + xb);
        }
    }
}

void rasterizeCoverage(const Polyline& poly, FillRule rule, int x0, int w, int rowBegin, int rowEnd, u8* out,
                       std::vector<f32>& acc) {
    if (w <= 0 || rowEnd <= rowBegin) return;
    struct Ctx {
        u8* out;
        int x0, w, rowBegin;
    } ctx{out, x0, w, rowBegin};
    std::fill(out, out + std::size_t(w) * std::size_t(rowEnd - rowBegin), u8(0));
    rasterizeCoverageRows(
        poly, rule, x0, w, rowBegin, rowEnd,
        [](void* u, int row, int xb, int xe, const u8* cov) {
            const Ctx& c = *static_cast<const Ctx*>(u);
            if (xe > xb) std::copy(cov, cov + (xe - xb), c.out + std::size_t(row - c.rowBegin) * std::size_t(c.w) + std::size_t(xb - c.x0));
        },
        &ctx, acc);
}

} // namespace fuse::ui
