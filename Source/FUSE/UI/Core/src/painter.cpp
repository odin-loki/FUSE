#include "fuse/ui/painter.hpp"

#include "fuse/ui/ui.hpp"

namespace fuse::ui {

namespace {
// Scratch geometry, reused across calls (keeps its capacity: a warmed-up Painter does not allocate).
struct Scratch {
    Path path;
    Polyline flat, outline;
};
Scratch& scratch() {
    thread_local Scratch s;
    return s;
}
Rect mapRect(const Affine& m, const Rect& r) { // scale + translate only
    const f32 x0 = m.a * r.x + m.tx, x1 = m.a * r.right() + m.tx, y0 = m.d * r.y + m.ty, y1 = m.d * r.bottom() + m.ty;
    return {std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0), std::abs(y1 - y0)};
}
} // namespace

Painter::Painter(DrawList& dl, UiContext* ui, u32 node) : m_dl(dl), m_ui(ui), m_node(node), m_first(dl.size()) {
    if (ui) m_deviceScale = std::max(ui->renderScale(), 1e-3f);
}

Painter::Painter(const PaintContext& pc, DrawList& dl) : Painter(dl, &pc.ui, pc.node.index) { m_stack[0].opacity = pc.opacity; }

Painter::~Painter() {
    for (int d = m_depth; d >= 0; --d)
        for (u32 i = 0; i < m_stack[std::size_t(d)].groups; ++i) m_dl.popClip();
    if (m_node != 0xFFFFFFFFu) // everything recorded through this painter belongs to its node
        for (std::size_t k = std::min(m_first, m_dl.size()); k < m_dl.size(); ++k)
            if (m_dl.cmds()[k].node == 0xFFFFFFFFu) m_dl.cmds()[k].node = m_node;
}

void Painter::save() {
    if (m_depth + 1 >= kMaxDepth) {
        ++m_overflow;
        return;
    }
    m_stack[std::size_t(m_depth) + 1] = m_stack[std::size_t(m_depth)];
    ++m_depth;
    cur().groups = 0;
}

void Painter::restore() {
    if (m_overflow > 0) {
        --m_overflow;
        return;
    }
    if (m_depth == 0) return;
    for (u32 i = 0; i < cur().groups; ++i) m_dl.popClip();
    --m_depth;
}

void Painter::setBrush(Color c) {
    BrushState& b = cur().brush;
    b.on = true;
    b.gradient = false;
    b.color = c;
}

void Painter::setBrush(const Gradient& g) {
    BrushState& b = cur().brush;
    b.on = true;
    b.gradient = true;
    b.kind = g.kind;
    b.spread = g.spread;
    b.p0 = g.p0;
    b.p1 = g.p1;
    b.transform = g.transform;
    b.stopCount = u8(std::min<std::size_t>(g.stops.size(), kMaxBrushStops));
    for (u8 i = 0; i < b.stopCount; ++i) b.stops[i] = g.stops[i];
}

void Painter::setPen(Color c, f32 width) {
    cur().penOn = true;
    cur().penColor = c;
    cur().pen = StrokeStyle{};
    cur().pen.width = width;
}

void Painter::setPen(Color c, const StrokeStyle& style) {
    cur().penOn = true;
    cur().penColor = c;
    cur().pen = style;
}

// ---- clips / layers ------------------------------------------------------------------------------------------------
void Painter::clipRect(const Rect& r) {
    const Affine& m = cur().xf;
    if (m.isScaleTranslate()) {
        m_dl.pushClip(mapRect(m, r));
    } else {
        Path& p = scratch().path;
        p.clear();
        p.addRect(r);
        m_dl.pushClipPath(p, FillRule::NonZero, m);
    }
    ++cur().groups;
}

void Painter::clipRoundedRect(const Rect& r, const std::array<f32, 4>& radii) {
    const Affine& m = cur().xf;
    if (m.isScaleTranslate() && std::abs(m.a) == std::abs(m.d) && m.a > 0.f && m.d > 0.f) {
        m_dl.pushClipRounded(mapRect(m, r), {radii[0] * m.a, radii[1] * m.a, radii[2] * m.a, radii[3] * m.a});
    } else {
        Path& p = scratch().path;
        p.clear();
        p.addRoundedRect(r, radii);
        m_dl.pushClipPath(p, FillRule::NonZero, m);
    }
    ++cur().groups;
}

void Painter::clipPath(const Path& path, FillRule rule) {
    m_dl.pushClipPath(path, rule, cur().xf);
    ++cur().groups;
}

void Painter::beginLayer(f32 opacity, const Rect& bounds) {
    Rect b{};
    if (!bounds.empty()) { // device bounds of the transformed rect
        const Affine& m = cur().xf;
        const Vec2 c[4]{m.apply({bounds.x, bounds.y}), m.apply({bounds.right(), bounds.y}), m.apply({bounds.right(), bounds.bottom()}),
                        m.apply({bounds.x, bounds.bottom()})};
        f32 x0 = c[0].x, y0 = c[0].y, x1 = x0, y1 = y0;
        for (const Vec2& p : c) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
        b = {x0, y0, x1 - x0, y1 - y0};
        if (b.empty()) b = {x0, y0, 1e-3f, 1e-3f}; // degenerate but not "whole clip"
    }
    m_dl.pushLayer(opacity * cur().opacity, b);
    ++cur().groups;
}

void Painter::endLayer() {
    if (cur().groups == 0) return;
    m_dl.popClip();
    --cur().groups;
}

// ---- paint ---------------------------------------------------------------------------------------------------------
void Painter::fillPath(const Path& path, const Brush& brush, FillRule rule) {
    Brush b = brush;
    if (b.hasGradient) b.opacity *= cur().opacity;
    else b.color.a *= cur().opacity;
    m_dl.fillPath(path, b, rule, cur().xf);
}

void Painter::strokePath(const Path& path, const StrokeStyle& style, const Brush& brush) {
    Brush b = brush;
    if (b.hasGradient) b.opacity *= cur().opacity;
    else b.color.a *= cur().opacity;
    const Affine& m = cur().xf;
    if (m.isSimilarity()) {
        m_dl.strokePath(path, style, b, m);
        return;
    }
    // non-uniform scale / skew: the stroke is not a constant-width band in device space, so outline it here, in user
    // space, and fill the transformed outline
    const f32 k = m.maxScale() * m_deviceScale;
    if (!(k > 0.f) || !std::isfinite(k) || !(std::abs(m.det()) > 0.f)) return;
    const f32 tol = 0.05f / k;
    Scratch& s = scratch();
    s.flat.clear();
    s.outline.clear();
    flattenPath(path, tol, s.flat);
    strokePolyline(s.flat, style, tol, s.outline);
    // `path` may be the scratch path (drawRect & co.): it has been consumed by flattenPath above
    s.path.clear();
    for (const Polyline::Contour& c : s.outline.contours) s.path.addPolyline({s.outline.points.data() + c.first, c.count}, true);
    m_dl.fillPath(s.path, b, FillRule::NonZero, m);
}

void Painter::fillCurrent(const Path& path) {
    const BrushState& bs = cur().brush;
    if (!bs.on) return;
    if (bs.gradient) {
        const Gradient g{bs.kind, bs.spread, bs.p0, bs.p1, {bs.stops.data(), bs.stopCount}, bs.transform};
        fillPath(path, Brush(g), cur().rule);
    } else {
        fillPath(path, Brush(bs.color), cur().rule);
    }
}

void Painter::strokeCurrent(const Path& path) {
    if (cur().penOn) strokePath(path, cur().pen, Brush(cur().penColor));
}

void Painter::drawPath(const Path& path) {
    fillCurrent(path);
    strokeCurrent(path);
}

void Painter::fillRect(const Rect& r, const Brush& brush) {
    const Affine& m = cur().xf;
    if (!brush.hasGradient && m.isScaleTranslate()) { // the native rect op
        Color c = brush.color;
        c.a *= cur().opacity;
        if (c.a > 0.f) m_dl.rect(mapRect(m, r), c);
        return;
    }
    Path& p = scratch().path;
    p.clear();
    p.addRect(r);
    fillPath(p, brush, FillRule::NonZero);
}

void Painter::drawRect(const Rect& r) {
    const State& s = cur();
    if (s.brush.on && !s.brush.gradient) fillRect(r, Brush(s.brush.color));
    const bool fillByPath = s.brush.on && s.brush.gradient;
    if (!fillByPath && !s.penOn) return;
    Path& p = scratch().path;
    p.clear();
    p.addRect(r);
    if (fillByPath) fillCurrent(p);
    strokeCurrent(p);
}

void Painter::drawRoundedRect(const Rect& r, const std::array<f32, 4>& radii) {
    const State& s = cur();
    const Affine& m = s.xf;
    bool filled = !s.brush.on;
    if (s.brush.on && !s.brush.gradient && m.isScaleTranslate() && m.a > 0.f && m.a == m.d) { // the native op
        Color c = s.brush.color;
        c.a *= s.opacity;
        if (c.a > 0.f) m_dl.roundedRect(mapRect(m, r), c, {radii[0] * m.a, radii[1] * m.a, radii[2] * m.a, radii[3] * m.a});
        filled = true;
    }
    if (filled && !s.penOn) return;
    Path& p = scratch().path;
    p.clear();
    p.addRoundedRect(r, radii);
    if (!filled) fillCurrent(p);
    strokeCurrent(p);
}

void Painter::drawEllipse(Vec2 centre, f32 rx, f32 ry) {
    Path& p = scratch().path;
    p.clear();
    p.addEllipse(centre, rx, ry);
    drawPath(p);
}

void Painter::drawLine(Vec2 a, Vec2 b) {
    const State& s = cur();
    if (!s.penOn) return;
    if (s.pen.cap == LineCap::Round && s.pen.dashCount == 0 && s.xf.isSimilarity()) { // the native capsule op
        Color c = s.penColor;
        c.a *= s.opacity;
        if (c.a > 0.f) m_dl.line(s.xf.apply(a), s.xf.apply(b), c, s.pen.width * s.xf.meanScale());
        return;
    }
    Path& p = scratch().path;
    p.clear();
    p.moveTo(a).lineTo(b);
    strokeCurrent(p);
}

void Painter::drawPolyline(std::span<const Vec2> pts) {
    if (pts.size() < 2 || !cur().penOn) return;
    Path& p = scratch().path;
    p.clear();
    p.addPolyline(pts, false);
    strokeCurrent(p);
}

void Painter::drawPolygon(std::span<const Vec2> pts) {
    if (pts.size() < 2) return;
    Path& p = scratch().path;
    p.clear();
    p.addPolyline(pts, true);
    drawPath(p);
}

void Painter::drawArc(Vec2 centre, f32 rx, f32 ry, f32 start, f32 sweep) {
    if (!cur().penOn) return;
    Path& p = scratch().path;
    p.clear();
    p.addArc(centre, rx, ry, start, sweep);
    strokeCurrent(p);
}

// ---- text / images -------------------------------------------------------------------------------------------------
void Painter::drawText(std::string_view text, const Rect& box, Color color, f32 pixelSize, TextAlignment align) {
    if (!m_ui || text.empty()) return;
    const Affine& m = cur().xf;
    color.a *= cur().opacity;
    if (color.a <= 0.f) return;
    Rect b;
    f32 k = 1.f;
    if (m.isScaleTranslate() && m.a > 0.f && m.d > 0.f) {
        b = mapRect(m, box);
        k = std::sqrt(m.a * m.d);
    } else { // rotation / skew / mirror: upright text at the transformed origin (see the header)
        k = m.meanScale();
        const Vec2 o = m.apply({box.x, box.y});
        b = {o.x, o.y, box.w * k, box.h * k};
    }
    if (!(k > 0.f) || !std::isfinite(k)) return;
    m_ui->paintString(m_dl, text, b, color, pixelSize * k, align, m_node);
}

bool Painter::mapImageRect(const Rect& dst, Rect& out) const {
    const Affine& m = cur().xf;
    if (!m.isScaleTranslate() || !(m.a > 0.f) || !(m.d > 0.f)) return false;
    out = mapRect(m, dst);
    return !out.empty();
}

bool Painter::drawImage(u32 image, const Rect& dst) {
    Rect r;
    if (!mapImageRect(dst, r)) return false;
    DrawCmd c;
    c.op = DrawOp::Image;
    c.image = image;
    c.rect = r;
    c.fill = Color{1.f, 1.f, 1.f, cur().opacity};
    if (cur().opacity > 0.f) m_dl.push(c);
    return true;
}

bool Painter::drawImage(u32 image, const Rect& dst, const Rect& src, bool smooth) {
    Rect r;
    if (!mapImageRect(dst, r)) return false;
    m_dl.imageRect(image, r, src, cur().opacity, smooth);
    return true;
}

bool Painter::drawNinePatch(u32 image, const Rect& dst, const Edges& insets, const Rect& src, bool smooth) {
    Rect r;
    if (!mapImageRect(dst, r)) return false;
    const Affine& m = cur().xf;
    const Edges di{insets.l * m.a, insets.t * m.d, insets.r * m.a, insets.b * m.d};
    m_dl.ninePatch(image, r, insets, src, &di, cur().opacity, smooth);
    return true;
}

} // namespace fuse::ui
