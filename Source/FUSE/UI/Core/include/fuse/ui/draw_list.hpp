// FUSE UI draw list (CPU data, no Vulkan types) and the CPU reference rasteriser. Plan §4.9.
//
// The CPU rasteriser is the specification of every primitive: fuse_ui_vk (UI4) is gated against it with FLIP.
// Output is premultiplied RGBA8 (R in the low byte) in display space, matching frame.ui_composite.
#pragma once

#include "fuse/ui/types.hpp"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace fuse::ui {

enum class DrawOp : u8 {
    Rect,     ///< rounded rect: per-corner radii, optional border, optional 2-stop gradient; analytic AA
    Shadow,   ///< analytic box shadow (rounded rect blurred by `blur`)
    Glyph,    ///< coverage quad from the glyph atlas tinted by `fill`
    Image,    ///< RGBA8 premultiplied image registered with the rasteriser (`image` index), scaled to `rect`
    Line,     ///< `rect` = (x0, y0, x1, y1), width `borderWidth`
    PushClip, ///< intersect the clip with `rect` (rounded clips: radius[0])
    PopClip,
    Backdrop, ///< frosted glass: blurs what is already drawn under the rounded `rect` (radius `blur`), then `fill` over it
    Path,     ///< filled / stroked vector path: edges DrawList::pathEdges()[image .. image + count) (count = tw | th << 16),
              ///< fill rule `v` (0 non-zero, 1 even-odd), `fill` (+ `gradient` / `fill2` over `rect`), stroke `border` /
              ///< `borderWidth`; `rect` = bounds. Analytic AA from the distance to the nearest edge.
    ClipPath, ///< intersect the clip with a path's fill region (edges as Path, fill rule `v`); PopClip ends it. Coverage
              ///< from the same 4x4 winding sub-samples as Path; nests (all active clip paths multiply).
};

/// Gradient kinds (DrawCmd::gradient, low 4 bits) for Rect and Path. 1..3: from `fill` to `fill2` across `rect`.
/// 4 / 5: multi-stop linear / radial (DrawCmd::gradXf maps a pixel to gradient space; t = x' (linear) or |(x', y')|
/// (radial); colour from the stop table DrawList::gradientStops()[stops .. stops + stopCount)); bits 4-5: spread.
enum GradientKind : u8 {
    kGradientNone = 0, kGradientVertical = 1, kGradientHorizontal = 2, kGradientRadial = 3,
    kGradientLinearStops = 4, kGradientRadialStops = 5,
};
enum class GradientSpread : u8 { Pad = 0, Repeat = 1, Reflect = 2 };

struct GradientStop {
    f32 offset = 0.f; ///< 0..1, non-decreasing (DrawList clamps)
    Color color;      ///< straight alpha; interpolated straight (like the 2-stop kinds)
};

/// A multi-stop gradient at any angle / ellipse / transform. `toUnit` maps a pixel to gradient space (affine
/// [a b c d e f]: x' = a x + c y + e, y' = b x + d y + f).
struct Gradient {
    bool radial = false;
    GradientSpread spread = GradientSpread::Pad;
    std::array<f32, 6> toUnit{1.f, 0.f, 0.f, 1.f, 0.f, 0.f};
    std::vector<GradientStop> stops;
    /// t = 0 at `from`, 1 at `to` (SVG linearGradient x1 y1 x2 y2).
    static Gradient linear(Vec2 from, Vec2 to, std::vector<GradientStop> stops, GradientSpread spread = GradientSpread::Pad);
    /// CSS linear-gradient(<angle>deg, ...) over `box`: 0 = to top, 90 = to right; the gradient line spans the box
    /// corners (length |w sin a| + |h cos a|).
    static Gradient linearAngle(const Rect& box, f32 degrees, std::vector<GradientStop> stops);
    /// t = 0 at `centre`, 1 on the ellipse (rx, ry) (SVG radialGradient cx cy r; CSS radial-gradient ellipse).
    static Gradient radialAt(Vec2 centre, f32 rx, f32 ry, std::vector<GradientStop> stops, GradientSpread spread = GradientSpread::Pad);
    /// Applies a user-space transform to the gradient (SVG gradientTransform / objectBoundingBox units).
    Gradient& transform(const std::array<f32, 6>& m);
    /// Colour at gradient parameter t (spread applied) — the exact arithmetic of the rasterisers.
    Color at(f32 t) const;
};

struct PathEdge;
/// Fill coverage of the path edges `e[0..n)` at pixel centre (px, py): 4x4 winding sub-samples (DrawOp::Path's fill
/// arithmetic; the GPU kernel's clip-path coverage matches it).
f32 pathFillCoverage(const PathEdge* e, std::size_t n, f32 px, f32 py, bool evenOdd);
/// Coverage of one ClipPath entry: a plain path (pathFillCoverage) or layered clip data (DrawList::pushClipLayers),
/// recognised by a header edge with fillOnly < 0. The GPU kernel's clipPathCoverage matches it.
f32 clipEntryCoverage(const PathEdge* e, std::size_t n, f32 px, f32 py, bool evenOdd);

/// One flattened path edge (DrawOp::Path). `fillOnly`: the implicit closing edge of an open contour (it bounds the
/// fill but is not stroked).
struct PathEdge {
    f32 x0, y0, x1, y1;
    f32 fillOnly;
};

/// A vector path (SVG / canvas style): contours of lines, quadratic and cubic Béziers, flattened on build.
class Path {
public:
    Path& moveTo(Vec2 p);
    Path& lineTo(Vec2 p);
    Path& quadTo(Vec2 c, Vec2 p);
    Path& cubicTo(Vec2 c1, Vec2 c2, Vec2 p);
    /// SVG elliptical arc from the current point to p (radii, x-axis rotation in degrees, large-arc and sweep flags).
    Path& arcTo(Vec2 radii, f32 rotationDeg, bool largeArc, bool sweep, Vec2 p);
    Path& close();
    Path& rect(const Rect& r);
    Path& circle(Vec2 c, f32 r);
    Path& ellipse(Vec2 c, f32 rx, f32 ry);
    /// SVG path data (`d` attribute): M L H V C S Q T A Z, absolute and relative (A = true elliptical arcs). Returns false (path left with what parsed) on a syntax error.
    bool appendSvg(std::string_view d);
    static Path fromSvg(std::string_view d) {
        Path p;
        p.appendSvg(d);
        return p;
    }
    /// Maps every point: p * scale + offset (SVG viewBox placement).
    Path& transform(Vec2 scale, Vec2 offset);
    /// Maps every point by the affine matrix [a c e; b d f] (SVG transform order): x' = a x + c y + e, y' = b x + d y + f.
    Path& transform(const std::array<f32, 6>& m);
    /// Adds `o`'s contours (closed as they are) after this path's (QPainterPath::addPath).
    Path& append(const Path& o);
    const std::vector<PathEdge>& edges() const;
    Rect bounds() const;
    bool empty() const { return edges().empty(); }

private:
    void edge(Vec2 a, Vec2 b);
    void unclose() const; ///< drops the implicit closing edge before the contour continues
    mutable std::vector<PathEdge> m_edges;
    Vec2 m_last{}, m_start{}, m_ctrl{};
    bool m_hasCtrl = false, m_inContour = false;
    mutable bool m_implicitClose = false; ///< m_edges ends with the open contour's fill-only closing edge
};

/// One layer of a layered clip (DrawList::pushClipLayers): a filled region with a mask value. Layers composite in
/// order like painted content ("over"), giving a per-pixel value M = v * cov + M * (1 - a * cov): SVG masks (v =
/// luminance x alpha of the layer's paint, a = its alpha) and clip-path unions (v = a = 1: M = union coverage).
/// `gradient` 1 / 2 varies v and a along a linear / radial ramp: t = x' or |(x', y')| with (x', y') = toUnit(pixel),
/// clamped to 0..1, from (value0, alpha0) to (value1, alpha1).
struct ClipLayer {
    Path path;
    bool evenOdd = false;
    f32 value0 = 1.f, alpha0 = 1.f;
    u8 gradient = 0;
    std::array<f32, 6> toUnit{1.f, 0.f, 0.f, 1.f, 0.f, 0.f};
    f32 value1 = 1.f, alpha1 = 1.f;
};

struct DrawCmd {
    DrawOp op = DrawOp::Rect;
    u8 gradient = 0; ///< 0 none, 1 vertical, 2 horizontal
    u16 u = 0, v = 0, tw = 0, th = 0; ///< Glyph: atlas texels
    u32 image = 0;
    u32 node = 0xFFFFFFFFu; ///< source node index (inspector, damage)
    Rect rect;
    Color fill, fill2, border;
    f32 borderWidth = 0.f;
    std::array<f32, 4> radius{};
    f32 blur = 0.f;
    std::array<f32, 6> gradXf{}; ///< kinds 4 / 5: pixel -> gradient space
    u32 stops = 0;               ///< kinds 4 / 5: first stop in DrawList::gradientStops()
    u16 stopCount = 0;
};

class DrawList {
public:
    void clear() {
        m_cmds.clear();
        m_edges.clear();
        m_stops.clear();
    }
    void reserve(std::size_t n) { m_cmds.reserve(n); }
    void push(const DrawCmd& c) { m_cmds.push_back(c); }
    void rect(const Rect& r, Color fill, f32 radius = 0.f);
    void roundedRect(const Rect& r, Color fill, const std::array<f32, 4>& radii, Color border = {}, f32 borderWidth = 0.f);
    void gradientRect(const Rect& r, Color from, Color to, bool vertical, const std::array<f32, 4>& radii = {});
    void shadow(const Rect& r, Color c, f32 blur, f32 radius);
    void line(Vec2 a, Vec2 b, Color c, f32 width);
    /// Backdrop blur (CSS backdrop-filter: blur()): everything drawn before this command under `r` is blurred.
    void backdrop(const Rect& r, f32 blur, const std::array<f32, 4>& radii, Color tint = {});
    /// Fills `p` (fill.a == 0: no fill) and strokes it (`strokeWidth` > 0, centred on the outline).
    void path(const Path& p, Color fill, bool evenOdd = false, Color stroke = {}, f32 strokeWidth = 0.f);
    void gradientPath(const Path& p, Color from, Color to, GradientKind kind, bool evenOdd = false);
    /// Multi-stop gradient fill (any angle, ellipse, transform, spread) of a rounded rect / a path (+ optional stroke).
    void gradientRect(const Rect& r, const Gradient& g, const std::array<f32, 4>& radii = {});
    void gradientPath(const Path& p, const Gradient& g, bool evenOdd = false, Color stroke = {}, f32 strokeWidth = 0.f);
    const std::vector<GradientStop>& gradientStops() const { return m_stops; }
    const std::vector<PathEdge>& pathEdges() const { return m_edges; }
    /// Copies `c` from `src` (its path edges too, for DrawOp::Path).
    void pushFrom(const DrawList& src, DrawCmd c);
    void pushClip(const Rect& r, f32 radius = 0.f);
    void popClip();
    /// Clips everything until the matching popClip() to `p`'s fill region (Qt: QPainter::setClipPath; SVG clipPath).
    void pushClipPath(const Path& p, bool evenOdd = false);
    /// Clips (multiplies coverage) by the per-pixel value of `layers` until popClip(); see ClipLayer.
    void pushClipLayers(std::span<const ClipLayer> layers);
    const std::vector<DrawCmd>& cmds() const { return m_cmds; }
    std::vector<DrawCmd>& cmds() { return m_cmds; }
    std::size_t size() const { return m_cmds.size(); }
    /// FNV-64 over the commands (golden / parity hashes).
    u64 hash() const;
    /// Moves every command by `d` (Line end points included): re-bases a subtree's list to a popup window's origin.
    void translate(Vec2 d);

private:
    friend void scaleDrawList(const DrawList& in, f32 scale, DrawList& out);
    std::vector<DrawCmd> m_cmds;
    std::vector<PathEdge> m_edges;
    std::vector<GradientStop> m_stops;
    void applyGradient(DrawCmd& c, const Gradient& g);
};

/// Multi-stop gradient colour of command `c` at pixel centre (px, py); the GPU kernel has the same arithmetic.
Color gradientStopsColor(const DrawCmd& c, const GradientStop* stops, f32 px, f32 py);

/// A premultiplied RGBA8 image.
struct Image {
    int width = 0, height = 0;
    std::vector<u32> pixels;
    /// >= 0: on the GPU (fuse_ui_vk) path the image is the external texture bound to this slot
    /// (UiVkRenderer::setExternalImage) and `pixels` are only the CPU rasteriser's fallback / screenshot copy.
    int externalSlot = -1;
    /// Optional HDR pixels (width * height, premultiplied RGBA16F: r | g << 16 | b << 32 | a << 48, 1.0 = SDR white).
    /// When present the GPU path draws them instead of `pixels`, keeping values above 1 on an HDR target
    /// (UiVkRenderer::setTargetHdr); `pixels` stay the 8-bit copy the CPU rasteriser and SDR screenshots use.
    std::vector<u64> hdrPixels;
    bool hasHdr() const { return !hdrPixels.empty() && hdrPixels.size() == std::size_t(width) * std::size_t(height); }
    void resize(int w, int h, u32 clear = 0) {
        width = w;
        height = h;
        pixels.assign(std::size_t(w) * std::size_t(h), clear);
    }
    u32 at(int x, int y) const { return pixels[std::size_t(y) * std::size_t(width) + std::size_t(x)]; }
    u64 hash() const;
};

/// Packs a straight colour to premultiplied RGBA8.
u32 packPremultiplied(Color c);

/// Logical units -> physical pixels (HiDPI / fractional scaling): every rect (Line endpoints included), radius,
/// border width and blur multiplied by `scale`. Glyphs are snapped to whole pixels and keep their texel size, so
/// text stays crisp when UiContext::setPixelScale(scale) rasterised it at the physical size.
void scaleDrawList(const DrawList& in, f32 scale, DrawList& out);

class CpuRasterizer {
public:
    /// Coverage atlas for Glyph commands (non-owning; must outlive render calls).
    void setGlyphAtlas(const u8* pixels, int size) {
        m_atlas = pixels;
        m_atlasSize = size;
    }
    /// Registers an image for Image commands (non-owning view). Returns its index.
    u32 addImage(const Image* img);
    /// Rasterises `list` over `target` (blending onto existing pixels) restricted to `scissor` (whole target when empty).
    void render(const DrawList& list, Image& target, Rect scissor = {}) const;
    /// The registered images, by index (what packDrawList needs for the GPU path).
    const Image* const* images() const { return m_images.data(); }
    u32 imageCount() const { return u32(m_images.size()); }

private:
    const u8* m_atlas = nullptr;
    int m_atlasSize = 0;
    std::vector<const Image*> m_images;
};

} // namespace fuse::ui
