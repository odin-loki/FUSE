// fuse::ui::Path: vector paths flattened to edges for DrawOp::Path (CPU rasteriser + ui.draw_list kernel).
#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/from_chars.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace fuse::ui {

void Path::unclose() const {
    if (m_implicitClose) m_edges.pop_back();
    m_implicitClose = false;
}
void Path::edge(Vec2 a, Vec2 b) {
    unclose();
    if (a.x != b.x || a.y != b.y) m_edges.push_back({a.x, a.y, b.x, b.y, 0.f});
}
Path& Path::moveTo(Vec2 p) {
    (void)edges(); // closes the previous contour (fill-only)
    m_implicitClose = false;
    m_start = m_last = p;
    m_inContour = true;
    m_hasCtrl = false;
    return *this;
}
Path& Path::lineTo(Vec2 p) {
    if (!m_inContour) moveTo(m_last);
    edge(m_last, p);
    m_last = p;
    m_hasCtrl = false;
    return *this;
}
namespace {
int segments(f32 len) { return std::clamp(int(std::ceil(std::sqrt(std::max(len, 0.f)) * 1.5f)), 2, 64); }
f32 dist(Vec2 a, Vec2 b) { return std::hypot(b.x - a.x, b.y - a.y); }
} // namespace
Path& Path::quadTo(Vec2 c, Vec2 p) {
    if (!m_inContour) moveTo(m_last);
    const Vec2 a = m_last;
    const int n = segments(dist(a, c) + dist(c, p));
    Vec2 prev = a;
    for (int i = 1; i <= n; ++i) {
        const f32 t = f32(i) / f32(n), u = 1.f - t;
        const Vec2 q{u * u * a.x + 2 * u * t * c.x + t * t * p.x, u * u * a.y + 2 * u * t * c.y + t * t * p.y};
        edge(prev, i == n ? p : q);
        prev = i == n ? p : q;
    }
    m_last = p;
    m_ctrl = c;
    m_hasCtrl = true;
    return *this;
}
Path& Path::cubicTo(Vec2 c1, Vec2 c2, Vec2 p) {
    if (!m_inContour) moveTo(m_last);
    const Vec2 a = m_last;
    const int n = segments(dist(a, c1) + dist(c1, c2) + dist(c2, p));
    Vec2 prev = a;
    for (int i = 1; i <= n; ++i) {
        const f32 t = f32(i) / f32(n), u = 1.f - t;
        const f32 b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
        const Vec2 q{b0 * a.x + b1 * c1.x + b2 * c2.x + b3 * p.x, b0 * a.y + b1 * c1.y + b2 * c2.y + b3 * p.y};
        edge(prev, i == n ? p : q);
        prev = i == n ? p : q;
    }
    m_last = p;
    m_ctrl = c2;
    m_hasCtrl = true;
    return *this;
}
Path& Path::arcTo(Vec2 radii, f32 rotationDeg, bool largeArc, bool sweep, Vec2 p) {
    // SVG 1.1 F.6.5/F.6.6: endpoint -> centre parameterisation, out-of-range radii scaled up, zero radius = line
    if (!m_inContour) moveTo(m_last);
    const Vec2 a = m_last;
    f32 rx = std::abs(radii.x), ry = std::abs(radii.y);
    if ((a.x == p.x && a.y == p.y)) return *this;
    if (rx == 0.f || ry == 0.f) return lineTo(p);
    const f32 phi = rotationDeg * 3.14159265358979f / 180.f, cs = std::cos(phi), sn = std::sin(phi);
    const f32 dx = (a.x - p.x) * 0.5f, dy = (a.y - p.y) * 0.5f;
    const f32 x1 = cs * dx + sn * dy, y1 = -sn * dx + cs * dy;
    const f32 lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1.f) {
        const f32 k = std::sqrt(lambda);
        rx *= k;
        ry *= k;
    }
    const f32 num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const f32 den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    f32 co = den > 0.f ? std::sqrt(std::max(0.f, num / den)) : 0.f;
    if (largeArc == sweep) co = -co;
    const f32 cx1 = co * rx * y1 / ry, cy1 = -co * ry * x1 / rx;
    const f32 cx = cs * cx1 - sn * cy1 + (a.x + p.x) * 0.5f, cy = sn * cx1 + cs * cy1 + (a.y + p.y) * 0.5f;
    auto angle = [](f32 ux, f32 uy, f32 vx, f32 vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
    const f32 ux = (x1 - cx1) / rx, uy = (y1 - cy1) / ry, vx = (-x1 - cx1) / rx, vy = (-y1 - cy1) / ry;
    const f32 t1 = angle(1.f, 0.f, ux, uy);
    f32 dt = angle(ux, uy, vx, vy);
    constexpr f32 kTau = 6.28318530717959f;
    if (!sweep && dt > 0.f) dt -= kTau;
    if (sweep && dt < 0.f) dt += kTau;
    // chord count for a sagitta error <= 0.05 px on the larger radius
    const f32 rm = std::max(rx, ry), step = 2.f * std::acos(std::clamp(1.f - 0.05f / std::max(rm, 0.05f), -1.f, 1.f));
    const int n = std::clamp(int(std::ceil(std::abs(dt) / std::max(step, 1e-3f))), 2, 256);
    Vec2 prev = a;
    for (int i = 1; i <= n; ++i) {
        const f32 t = t1 + dt * f32(i) / f32(n);
        const f32 ex = rx * std::cos(t), ey = ry * std::sin(t);
        const Vec2 q = i == n ? p : Vec2{cs * ex - sn * ey + cx, sn * ex + cs * ey + cy};
        edge(prev, q);
        prev = q;
    }
    m_last = p;
    m_hasCtrl = false;
    return *this;
}
Path& Path::close() {
    if (m_inContour) edge(m_last, m_start);
    m_last = m_start;
    m_inContour = false;
    m_hasCtrl = false;
    return *this;
}
Path& Path::rect(const Rect& r) {
    return moveTo({r.x, r.y}).lineTo({r.right(), r.y}).lineTo({r.right(), r.bottom()}).lineTo({r.x, r.bottom()}).close();
}
Path& Path::circle(Vec2 c, f32 r) {
    constexpr f32 k = 0.5522847498f; // cubic quarter-circle
    const f32 o = r * k;
    moveTo({c.x + r, c.y});
    cubicTo({c.x + r, c.y + o}, {c.x + o, c.y + r}, {c.x, c.y + r});
    cubicTo({c.x - o, c.y + r}, {c.x - r, c.y + o}, {c.x - r, c.y});
    cubicTo({c.x - r, c.y - o}, {c.x - o, c.y - r}, {c.x, c.y - r});
    cubicTo({c.x + o, c.y - r}, {c.x + r, c.y - o}, {c.x + r, c.y});
    return close();
}
const std::vector<PathEdge>& Path::edges() const {
    if (m_inContour && !m_implicitClose && (m_last.x != m_start.x || m_last.y != m_start.y)) {
        m_edges.push_back({m_last.x, m_last.y, m_start.x, m_start.y, 1.f});
        m_implicitClose = true;
    }
    return m_edges;
}
Path& Path::append(const Path& o) {
    (void)edges();
    const auto& e = o.edges();
    m_edges.insert(m_edges.end(), e.begin(), e.end());
    m_implicitClose = false;
    m_inContour = false;
    m_hasCtrl = false;
    m_start = m_last = o.m_last;
    return *this;
}
Rect Path::bounds() const {
    const auto& e = edges();
    if (e.empty()) return {};
    f32 x0 = e[0].x0, y0 = e[0].y0, x1 = x0, y1 = y0;
    for (const PathEdge& k : e) {
        x0 = std::min({x0, k.x0, k.x1});
        y0 = std::min({y0, k.y0, k.y1});
        x1 = std::max({x1, k.x0, k.x1});
        y1 = std::max({y1, k.y0, k.y1});
    }
    return {x0, y0, x1 - x0, y1 - y0};
}
Path& Path::transform(Vec2 sc, Vec2 off) {
    (void)edges();
    for (PathEdge& e : m_edges) {
        e.x0 = e.x0 * sc.x + off.x;
        e.y0 = e.y0 * sc.y + off.y;
        e.x1 = e.x1 * sc.x + off.x;
        e.y1 = e.y1 * sc.y + off.y;
    }
    auto map = [&](Vec2& p) { p = {p.x * sc.x + off.x, p.y * sc.y + off.y}; };
    map(m_last);
    map(m_start);
    map(m_ctrl);
    return *this;
}

Path& Path::transform(const std::array<f32, 6>& m) {
    (void)edges();
    auto map = [&](f32& x, f32& y) { const f32 nx = m[0] * x + m[2] * y + m[4]; y = m[1] * x + m[3] * y + m[5]; x = nx; };
    for (PathEdge& e : m_edges) { map(e.x0, e.y0); map(e.x1, e.y1); }
    map(m_last.x, m_last.y);
    map(m_start.x, m_start.y);
    map(m_ctrl.x, m_ctrl.y);
    return *this;
}

Path& Path::ellipse(Vec2 c, f32 rx, f32 ry) {
    constexpr f32 k = 0.5522847498f; // 4/3 (sqrt2 - 1)
    moveTo({c.x + rx, c.y});
    cubicTo({c.x + rx, c.y + ry * k}, {c.x + rx * k, c.y + ry}, {c.x, c.y + ry});
    cubicTo({c.x - rx * k, c.y + ry}, {c.x - rx, c.y + ry * k}, {c.x - rx, c.y});
    cubicTo({c.x - rx, c.y - ry * k}, {c.x - rx * k, c.y - ry}, {c.x, c.y - ry});
    cubicTo({c.x + rx * k, c.y - ry}, {c.x + rx, c.y - ry * k}, {c.x + rx, c.y});
    return close();
}

bool Path::appendSvg(std::string_view d) {
    std::size_t i = 0;
    auto skip = [&] {
        while (i < d.size() && (d[i] == ' ' || d[i] == ',' || d[i] == '\t' || d[i] == '\n' || d[i] == '\r')) ++i;
    };
    auto number = [&](f32& v) -> bool {
        skip();
        if (i >= d.size()) return false;
        const char* b = d.data() + i;
        const char* e = d.data() + d.size();
        if (*b == '+') ++b;
        const auto r = fromChars(b, e, v);
        if (r.ec != std::errc{}) return false;
        i = std::size_t(r.ptr - d.data());
        return true;
    };
    auto flag = [&](f32& v) -> bool { // arc flags may be written without separators ("a1 1 0 01 1 1")
        skip();
        if (i < d.size() && (d[i] == '0' || d[i] == '1')) { v = f32(d[i++] - '0'); return true; }
        return false;
    };
    char cmd = 0;
    while (true) {
        skip();
        if (i >= d.size()) return true;
        if (std::isalpha(static_cast<unsigned char>(d[i]))) cmd = d[i++];
        else if (!cmd) return false;
        const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
        const Vec2 o = rel ? m_last : Vec2{0.f, 0.f};
        f32 a[7];
        auto read = [&](int n) {
            for (int k = 0; k < n; ++k)
                if (!number(a[k])) return false;
            return true;
        };
        switch (std::tolower(static_cast<unsigned char>(cmd))) {
        case 'm':
            if (!read(2)) return false;
            moveTo({o.x + a[0], o.y + a[1]});
            cmd = rel ? 'l' : 'L'; // further pairs are line-tos
            break;
        case 'l':
            if (!read(2)) return false;
            lineTo({o.x + a[0], o.y + a[1]});
            break;
        case 'h':
            if (!read(1)) return false;
            lineTo({o.x + a[0], m_last.y});
            break;
        case 'v':
            if (!read(1)) return false;
            lineTo({m_last.x, o.y + a[0]});
            break;
        case 'c':
            if (!read(6)) return false;
            cubicTo({o.x + a[0], o.y + a[1]}, {o.x + a[2], o.y + a[3]}, {o.x + a[4], o.y + a[5]});
            break;
        case 's': {
            if (!read(4)) return false;
            const Vec2 c1 = m_hasCtrl ? Vec2{2 * m_last.x - m_ctrl.x, 2 * m_last.y - m_ctrl.y} : m_last;
            cubicTo(c1, {o.x + a[0], o.y + a[1]}, {o.x + a[2], o.y + a[3]});
            break;
        }
        case 'q':
            if (!read(4)) return false;
            quadTo({o.x + a[0], o.y + a[1]}, {o.x + a[2], o.y + a[3]});
            break;
        case 't': {
            if (!read(2)) return false;
            const Vec2 c = m_hasCtrl ? Vec2{2 * m_last.x - m_ctrl.x, 2 * m_last.y - m_ctrl.y} : m_last;
            quadTo(c, {o.x + a[0], o.y + a[1]});
            break;
        }
        case 'a':
            if (!read(3) || !flag(a[3]) || !flag(a[4]) || !number(a[5]) || !number(a[6])) return false;
            arcTo({a[0], a[1]}, a[2], a[3] != 0.f, a[4] != 0.f, {o.x + a[5], o.y + a[6]});
            break;
        case 'z': close(); cmd = 0; break;
        default: return false;
        }
    }
}

// ---- DrawList ----
void DrawList::path(const Path& p, Color fill, bool evenOdd, Color stroke, f32 strokeWidth) {
    const auto& e = p.edges();
    if (e.empty()) return;
    DrawCmd c;
    c.op = DrawOp::Path;
    const f32 m = (strokeWidth > 0.f ? strokeWidth * 0.5f : 0.f) + 1.f;
    const Rect b = p.bounds();
    c.rect = {b.x - m, b.y - m, b.w + 2 * m, b.h + 2 * m};
    c.fill = fill;
    c.border = stroke;
    c.borderWidth = strokeWidth > 0.f && stroke.a > 0.f ? strokeWidth : 0.f;
    c.v = evenOdd ? 1 : 0;
    c.image = u32(m_edges.size());
    c.tw = u16(e.size() & 0xFFFFu);
    c.th = u16(e.size() >> 16);
    m_edges.insert(m_edges.end(), e.begin(), e.end());
    m_cmds.push_back(c);
}
void DrawList::pushClipLayers(std::span<const ClipLayer> layers) {
    // edge stream: per layer a header {value0, evenOdd, edgeCount, alpha0, -1 - gradient}, for gradients two
    // parameter edges {toUnit[0..3], 0} {toUnit[4], toUnit[5], value1, alpha1, 0}, then the layer's edges
    DrawCmd c;
    c.op = DrawOp::ClipPath;
    c.image = u32(m_edges.size());
    bool any = false;
    Rect b{};
    for (const ClipLayer& l : layers) {
        const auto& e = l.path.edges();
        if (e.empty()) continue;
        const Rect lb = l.path.bounds();
        if (!any) b = lb, any = true;
        else {
            const f32 x0 = std::min(b.x, lb.x), y0 = std::min(b.y, lb.y);
            b = {x0, y0, std::max(b.right(), lb.right()) - x0, std::max(b.bottom(), lb.bottom()) - y0};
        }
        const u8 g = l.gradient <= 2 ? l.gradient : 0;
        m_edges.push_back({l.value0, l.evenOdd ? 1.f : 0.f, f32(e.size()), l.alpha0, -1.f - f32(g)});
        if (g) {
            m_edges.push_back({l.toUnit[0], l.toUnit[1], l.toUnit[2], l.toUnit[3], 0.f});
            m_edges.push_back({l.toUnit[4], l.toUnit[5], l.value1, l.alpha1, 0.f});
        }
        m_edges.insert(m_edges.end(), e.begin(), e.end());
    }
    const std::size_t n = m_edges.size() - c.image;
    c.rect = b;
    c.tw = u16(n & 0xFFFFu);
    c.th = u16(n >> 16);
    m_cmds.push_back(c); // no layers: n == 0, which clips everything (like an empty clip path)
}
void DrawList::pushClipPath(const Path& p, bool evenOdd) {
    const auto& e = p.edges();
    DrawCmd c;
    c.op = DrawOp::ClipPath;
    c.rect = p.bounds();
    c.v = evenOdd ? 1 : 0;
    c.image = u32(m_edges.size());
    c.tw = u16(e.size() & 0xFFFFu);
    c.th = u16(e.size() >> 16);
    m_edges.insert(m_edges.end(), e.begin(), e.end());
    m_cmds.push_back(c);
}
void DrawList::gradientPath(const Path& p, Color from, Color to, GradientKind kind, bool evenOdd) {
    const std::size_t n = m_cmds.size();
    path(p, from, evenOdd);
    if (m_cmds.size() == n) return;
    m_cmds.back().gradient = kind;
    m_cmds.back().fill2 = to;
}
// ---- multi-stop gradients ------------------------------------------------------------------------------------------
namespace {
std::array<f32, 6> composeAffine(const std::array<f32, 6>& A, const std::array<f32, 6>& B) { // A(B(p))
    return {A[0] * B[0] + A[2] * B[1], A[1] * B[0] + A[3] * B[1], A[0] * B[2] + A[2] * B[3], A[1] * B[2] + A[3] * B[3],
            A[0] * B[4] + A[2] * B[5] + A[4], A[1] * B[4] + A[3] * B[5] + A[5]};
}
bool invertAffine(const std::array<f32, 6>& m, std::array<f32, 6>& out) {
    const f32 det = m[0] * m[3] - m[1] * m[2];
    if (std::abs(det) < 1e-12f) return false;
    const f32 id = 1.f / det;
    out = {m[3] * id, -m[1] * id, -m[2] * id, m[0] * id, (m[2] * m[5] - m[3] * m[4]) * id, (m[1] * m[4] - m[0] * m[5]) * id};
    return true;
}
} // namespace

Gradient Gradient::linear(Vec2 from, Vec2 to, std::vector<GradientStop> stops, GradientSpread spread) {
    Gradient g;
    g.spread = spread;
    g.stops = std::move(stops);
    const f32 dx = to.x - from.x, dy = to.y - from.y, l2 = std::max(dx * dx + dy * dy, 1e-12f);
    g.toUnit = {dx / l2, 0.f, dy / l2, 0.f, -(from.x * dx + from.y * dy) / l2, 0.f};
    return g;
}
Gradient Gradient::linearAngle(const Rect& box, f32 degrees, std::vector<GradientStop> stops) {
    const f32 a = degrees * 3.14159265358979f / 180.f, sx = std::sin(a), sy = -std::cos(a);
    const f32 half = (std::abs(box.w * sx) + std::abs(box.h * sy)) * 0.5f;
    const Vec2 c{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    return linear({c.x - sx * half, c.y - sy * half}, {c.x + sx * half, c.y + sy * half}, std::move(stops));
}
Gradient Gradient::radialFocal(Vec2 centre, f32 rx, f32 ry, Vec2 focus, std::vector<GradientStop> stops, GradientSpread spread) {
    Gradient g = radialAt(centre, rx, ry, std::move(stops), spread);
    g.focal = {(focus.x - centre.x) / std::max(rx, 1e-6f), (focus.y - centre.y) / std::max(ry, 1e-6f)};
    return g;
}
Gradient Gradient::radialAt(Vec2 centre, f32 rx, f32 ry, std::vector<GradientStop> stops, GradientSpread spread) {
    Gradient g;
    g.radial = true;
    g.spread = spread;
    g.stops = std::move(stops);
    rx = std::max(rx, 1e-6f);
    ry = std::max(ry, 1e-6f);
    g.toUnit = {1.f / rx, 0.f, 0.f, 1.f / ry, -centre.x / rx, -centre.y / ry};
    return g;
}
Gradient& Gradient::transform(const std::array<f32, 6>& m) {
    std::array<f32, 6> inv;
    if (invertAffine(m, inv)) toUnit = composeAffine(toUnit, inv);
    return *this;
}
namespace {
inline f32 spreadT(u8 spread, f32 t) {
    if (spread == u8(GradientSpread::Repeat)) return t - std::floor(t);
    if (spread == u8(GradientSpread::Reflect)) {
        const f32 m = t - 2.f * std::floor(t * 0.5f);
        return m > 1.f ? 2.f - m : m;
    }
    return std::clamp(t, 0.f, 1.f);
}
inline Color stopColor(const GradientStop* s, u32 n, f32 t) {
    if (n == 0) return {};
    if (t <= s[0].offset) return s[0].color;
    for (u32 i = 1; i < n; ++i)
        if (t <= s[i].offset) {
            const f32 span = s[i].offset - s[i - 1].offset;
            const f32 f = span > 1e-6f ? (t - s[i - 1].offset) / span : 1.f;
            const Color a = s[i - 1].color, b = s[i].color;
            return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, a.a + (b.a - a.a) * f};
        }
    return s[n - 1].color;
}
} // namespace
Color Gradient::at(f32 t) const { return stopColor(stops.data(), u32(stops.size()), spreadT(u8(spread), t)); }

Color gradientStopsColor(const DrawCmd& c, const GradientStop* stops, f32 px, f32 py) {
    const auto& m = c.gradXf;
    const f32 u = m[0] * px + m[2] * py + m[4];
    const f32 v = m[1] * px + m[3] * py + m[5];
    f32 t = (c.gradient & 15u) == kGradientLinearStops ? u : std::sqrt(u * u + v * v);
    if ((c.gradient & 15u) == kGradientRadialFocal) {
        const f32 fx = c.focal[0], fy = c.focal[1], dx = u - fx, dy = v - fy;
        const f32 dF = dx * fx + dy * fy, k = 1.f - (fx * fx + fy * fy);
        t = (dF + std::sqrt(std::max(dF * dF + k * (dx * dx + dy * dy), 0.f))) / k;
    }
    return stopColor(stops, c.stopCount, spreadT(u8(c.gradient >> 4), t));
}

void DrawList::applyGradient(DrawCmd& c, const Gradient& g) {
    Vec2 f = g.focal;
    const bool focal = g.radial && (f.x != 0.f || f.y != 0.f);
    if (const f32 len = std::sqrt(f.x * f.x + f.y * f.y); len > 0.99f) f = {f.x * 0.99f / len, f.y * 0.99f / len};
    c.gradient = u8((focal ? kGradientRadialFocal : g.radial ? kGradientRadialStops : kGradientLinearStops) | (u8(g.spread) << 4));
    c.focal = focal ? std::array<f32, 2>{f.x, f.y} : std::array<f32, 2>{};
    c.gradXf = g.toUnit;
    c.stops = u32(m_stops.size());
    const std::size_t n = std::min<std::size_t>(g.stops.size(), 64);
    f32 last = 0.f;
    for (std::size_t i = 0; i < n; ++i) { // SVG: offsets clamp to [0, 1] and to the previous offset
        GradientStop st = g.stops[i];
        st.offset = std::max(last, std::clamp(st.offset, 0.f, 1.f));
        last = st.offset;
        m_stops.push_back(st);
    }
    c.stopCount = u16(n);
    // 2-colour approximation for consumers without stop support (PDF export, legacy paths)
    if (n) { c.fill = g.stops.front().color; c.fill2 = g.stops[n - 1].color; }
}
void DrawList::gradientRect(const Rect& r, const Gradient& g, const std::array<f32, 4>& radii) {
    DrawCmd c;
    c.op = DrawOp::Rect;
    c.rect = r;
    c.radius = radii;
    applyGradient(c, g);
    if (c.stopCount) m_cmds.push_back(c);
}
void DrawList::gradientPath(const Path& p, const Gradient& g, bool evenOdd, Color stroke, f32 strokeWidth) {
    const std::size_t n = m_cmds.size();
    path(p, g.stops.empty() ? Color{} : g.stops.front().color, evenOdd, stroke, strokeWidth);
    if (m_cmds.size() == n || g.stops.empty()) return;
    applyGradient(m_cmds.back(), g);
}

void DrawList::pushFrom(const DrawList& src, DrawCmd c) {
    if ((c.gradient & 15u) >= kGradientLinearStops || (c.op == DrawOp::Backdrop && c.stopCount)) {
        const u32 at = u32(m_stops.size());
        if (std::size_t(c.stops) + c.stopCount <= src.m_stops.size())
            m_stops.insert(m_stops.end(), src.m_stops.begin() + c.stops, src.m_stops.begin() + c.stops + c.stopCount);
        else c.stopCount = 0;
        c.stops = at;
    }
    if (c.op == DrawOp::Path || c.op == DrawOp::ClipPath) {
        const std::size_t n = std::size_t(c.tw) | (std::size_t(c.th) << 16);
        const u32 at = u32(m_edges.size());
        if (std::size_t(c.image) + n <= src.m_edges.size())
            m_edges.insert(m_edges.end(), src.m_edges.begin() + c.image, src.m_edges.begin() + c.image + std::ptrdiff_t(n));
        c.image = at;
    }
    m_cmds.push_back(c);
}

} // namespace fuse::ui
