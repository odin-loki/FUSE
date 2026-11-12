// FUSE UI Painter (plan §4.9, the QPainter equivalent): an immediate-style drawing API that records into a DrawList.
//
//   void MyWidget::paint(const PaintContext& pc, DrawList& dl) {
//       Painter p(pc, dl);                       // inherits the node's opacity and the context's render scale
//       p.translate(pc.content.x, pc.content.y);
//       p.setBrush(Color::rgb(0x3D8BFD));
//       p.setPen(Color::rgb(0xFFFFFF), 1.5f);
//       p.drawEllipse({40, 40}, 30, 20);
//       p.save();
//       p.rotate(0.3f);
//       p.clipPath(star);
//       p.fillRect({0, 0, 80, 80}, Gradient::linear({0, 0}, {80, 0}, stops));
//       p.restore();                             // pops the clip and the rotation
//   }
//
// State (transform, opacity, brush, pen, fill rule, open clips / layers) lives in a fixed stack of kMaxDepth levels:
// a Painter never allocates, and what it records is resolution independent (paths are flattened by the rasteriser in
// device pixels). Coordinates are logical units in the list's space, transformed by the current matrix.
//
// Limits (by design, stated rather than faked):
//   * text is drawn from the glyph atlas: it follows translation and scale (uniform scale -> the font size), but is
//     NOT rotated or skewed - under such a transform it is drawn upright at the transformed position
//   * images follow translation and positive scale only: drawImage / drawNinePatch return false and draw nothing
//     under rotation, skew or mirroring
//   * a brush set with setBrush(Gradient) keeps at most kMaxBrushStops stops (pass a Gradient to fillPath / fillRect
//     for more)
#pragma once

#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/style.hpp"

#include <string_view>

namespace fuse::ui {

class UiContext;
struct PaintContext;

class Painter {
public:
    static constexpr int kMaxDepth = 16;
    static constexpr int kMaxBrushStops = 16;

    /// Records into `dl`. `ui` is needed for drawText only; `node` tags the recorded commands (inspector, damage)
    /// when the painter is destroyed.
    explicit Painter(DrawList& dl, UiContext* ui = nullptr, u32 node = 0xFFFFFFFFu);
    /// For a widget's paint callback: the node's accumulated opacity is the painter's base opacity.
    Painter(const PaintContext& pc, DrawList& dl);
    /// Closes the clips / layers that are still open.
    ~Painter();
    Painter(const Painter&) = delete;
    Painter& operator=(const Painter&) = delete;

    // ---- state stack ----
    /// Pushes the state. Beyond kMaxDepth nested saves the state is not copied (the matching restore is a no-op).
    void save();
    /// Pops the state: transform, opacity, brush, pen and fill rule return to their saved values, and every clip /
    /// layer opened since the matching save() is closed.
    void restore();
    int depth() const { return m_depth; }

    // ---- transform (each call post-multiplies: it acts in the current local coordinates, like QPainter / canvas) ----
    void translate(f32 x, f32 y) { transform(Affine::translation(x, y)); }
    void translate(Vec2 d) { translate(d.x, d.y); }
    void scale(f32 sx, f32 sy) { transform(Affine::scaling(sx, sy)); }
    void scale(f32 s) { scale(s, s); }
    void rotate(f32 radians) { transform(Affine::rotation(radians)); }
    void transform(const Affine& m) { cur().xf = cur().xf * m; }
    void setTransform(const Affine& m) { cur().xf = m; }
    const Affine& worldTransform() const { return m_stack[std::size_t(m_depth)].xf; }

    // ---- paint state ----
    /// Multiplies every colour drawn afterwards (per primitive, as QPainter::setOpacity; beginLayer() for a group).
    void setOpacity(f32 o) { cur().opacity = std::clamp(o, 0.f, 1.f); }
    f32 opacity() const { return m_stack[std::size_t(m_depth)].opacity; }
    void setBrush(Color c);
    void setBrush(const Gradient& g); ///< geometry in local coordinates at draw time; stops are copied
    void setNoBrush() { cur().brush.on = false; }
    void setPen(Color c, f32 width = 1.f); ///< a solid pen: butt caps, miter joins, no dashes
    void setPen(Color c, const StrokeStyle& style);
    void setNoPen() { cur().penOn = false; }
    void setFillRule(FillRule r) { cur().rule = r; }

    // ---- clips and layers (closed by the restore() of the enclosing save(), or by the destructor) ----
    void clipRect(const Rect& r);
    void clipRoundedRect(const Rect& r, const std::array<f32, 4>& radii);
    void clipPath(const Path& path, FillRule rule = FillRule::NonZero);
    /// Group opacity: what is drawn until endLayer() / restore() is composited once with `opacity`. `bounds` (local
    /// coordinates; empty = the current clip) limits the layer.
    void beginLayer(f32 opacity, const Rect& bounds = {});
    void endLayer();

    // ---- drawing with the current brush (fill) and pen (stroke) ----
    void drawPath(const Path& path);
    void drawRect(const Rect& r);
    void drawRoundedRect(const Rect& r, const std::array<f32, 4>& radii);
    void drawRoundedRect(const Rect& r, f32 radius) { drawRoundedRect(r, {radius, radius, radius, radius}); }
    void drawEllipse(Vec2 centre, f32 rx, f32 ry);
    void drawCircle(Vec2 centre, f32 r) { drawEllipse(centre, r, r); }
    void drawLine(Vec2 a, Vec2 b);                 ///< pen only
    void drawPolyline(std::span<const Vec2> pts);  ///< pen only (open)
    void drawPolygon(std::span<const Vec2> pts);   ///< closed: brush and pen
    void drawArc(Vec2 centre, f32 rx, f32 ry, f32 start, f32 sweep); ///< pen only; radians, positive = clockwise

    // ---- drawing with explicit paint (the brush / pen state is not used) ----
    void fillPath(const Path& path, const Brush& brush, FillRule rule = FillRule::NonZero);
    void strokePath(const Path& path, const StrokeStyle& style, const Brush& brush);
    void fillRect(const Rect& r, const Brush& brush);

    // ---- text and images ----
    /// One line of text in `box` (vertically centred, elided), through UiContext::paintString. No-op without a context.
    void drawText(std::string_view text, const Rect& box, Color color, f32 pixelSize, TextAlignment align = TextAlignment::Start);
    /// Image `image` (a rasteriser image index) scaled into `dst`. False (nothing drawn) under rotation / skew / mirror.
    bool drawImage(u32 image, const Rect& dst);
    /// Sub-rect `src` (texels) of the image scaled into `dst`.
    bool drawImage(u32 image, const Rect& dst, const Rect& src, bool smooth = false);
    /// 9-slice: `insets` (texels) cut the image (or `src`) 3x3; corners keep their size, edges and centre stretch.
    bool drawNinePatch(u32 image, const Rect& dst, const Edges& insets, const Rect& src = {}, bool smooth = false);

    DrawList& drawList() { return m_dl; }

private:
    struct BrushState {
        bool on = false, gradient = false;
        Color color;
        GradientKind kind = GradientKind::Linear;
        GradientSpread spread = GradientSpread::Pad;
        Vec2 p0, p1;
        Affine transform;
        u8 stopCount = 0;
        std::array<GradientStop, kMaxBrushStops> stops{};
    };
    struct State {
        Affine xf;
        f32 opacity = 1.f;
        BrushState brush;
        bool penOn = false;
        Color penColor;
        StrokeStyle pen;
        FillRule rule = FillRule::NonZero;
        u32 groups = 0; ///< clips / layers opened at this level
    };
    State& cur() { return m_stack[std::size_t(m_depth)]; }
    const State& cur() const { return m_stack[std::size_t(m_depth)]; }
    void fillCurrent(const Path& path);
    void strokeCurrent(const Path& path);
    bool mapImageRect(const Rect& dst, Rect& out) const;

    DrawList& m_dl;
    UiContext* m_ui = nullptr;
    u32 m_node = 0xFFFFFFFFu;
    std::size_t m_first = 0; ///< list size at construction: commands from here on are tagged with m_node at the end
    f32 m_deviceScale = 1.f; ///< render scale hint (tolerance of strokes outlined in user space)
    int m_depth = 0;
    int m_overflow = 0; ///< saves beyond kMaxDepth
    std::array<State, kMaxDepth> m_stack{};
};

} // namespace fuse::ui
