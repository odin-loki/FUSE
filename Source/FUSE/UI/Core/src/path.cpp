// fuse::ui::Path: vector paths flattened to edges for DrawOp::Path (CPU rasteriser + ui.draw_list kernel).
#include "fuse/ui/draw_list.hpp"

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
        const auto r = std::from_chars(b, e, v);
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
            lineTo({o.x + a[5], o.y + a[6]});
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
void DrawList::gradientPath(const Path& p, Color from, Color to, GradientKind kind, bool evenOdd) {
    const std::size_t n = m_cmds.size();
    path(p, from, evenOdd);
    if (m_cmds.size() == n) return;
    m_cmds.back().gradient = kind;
    m_cmds.back().fill2 = to;
}
void DrawList::pushFrom(const DrawList& src, DrawCmd c) {
    if (c.op == DrawOp::Path) {
        const std::size_t n = std::size_t(c.tw) | (std::size_t(c.th) << 16);
        const u32 at = u32(m_edges.size());
        if (std::size_t(c.image) + n <= src.m_edges.size())
            m_edges.insert(m_edges.end(), src.m_edges.begin() + c.image, src.m_edges.begin() + c.image + std::ptrdiff_t(n));
        c.image = at;
    }
    m_cmds.push_back(c);
}

} // namespace fuse::ui
