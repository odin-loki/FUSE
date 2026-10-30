// FUSE UI draw list (CPU data, no Vulkan types) and the CPU reference rasteriser. Plan §4.9.
//
// The CPU rasteriser is the specification of every primitive: fuse_ui_vk (UI4) is gated against it with FLIP.
// Output is premultiplied RGBA8 (R in the low byte) in display space, matching frame.ui_composite.
#pragma once

#include "fuse/ui/types.hpp"

#include <array>
#include <functional>
#include <span>
#include <string>
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
    PushFilter, ///< CSS `filter`: commands up to the matching PopFilter draw into an isolated layer over `rect` (the
                ///< pixel box, set by DrawList::popFilter from the layer's ink plus the filter's reach); see pushFilter
    PopFilter,  ///< ends the layer: blur, colour matrix, drop shadow, then composited over what was under it
};

/// Gradient kinds (DrawCmd::gradient, low 4 bits) for Rect and Path. 1..3: from `fill` to `fill2` across `rect`.
/// 4 / 5: multi-stop linear / radial (DrawCmd::gradXf maps a pixel to gradient space; t = x' (linear) or |(x', y')|
/// (radial); colour from the stop table DrawList::gradientStops()[stops .. stops + stopCount)); bits 4-5: spread.
enum GradientKind : u8 {
    kGradientNone = 0, kGradientVertical = 1, kGradientHorizontal = 2, kGradientRadial = 3,
    kGradientLinearStops = 4, kGradientRadialStops = 5,
    /// 6: radial with a focal point (SVG fx / fy): DrawCmd::focal holds the focal point in gradient space
    /// (|f| < 1); t solves |(x', y') - f (1 - t)| = t, i.e. circles interpolating from the focal point to the unit circle.
    kGradientRadialFocal = 6,
};
enum class GradientSpread : u8 { Pad = 0, Repeat = 1, Reflect = 2 };

struct GradientStop {
    f32 offset = 0.f; ///< 0..1, non-decreasing (DrawList clamps)
    Color color;      ///< straight alpha; interpolated straight (like the 2-stop kinds)
};

/// CSS filter functions as one affine colour matrix on straight-alpha sRGB values in 0..1 (feColorMatrix layout:
/// row k = out channel k (r, g, b, a), columns r, g, b, a, offset). Functions compose in CSS order with then():
/// `ColorMatrix::brightness(1.2f).then(ColorMatrix::saturate(0.5f))` is `brightness(1.2) saturate(0.5)`. Composing
/// skips the per-step clamp of a filter chain, which only matters when an intermediate step leaves 0..1.
struct ColorMatrix {
    std::array<f32, 20> m{1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
    static ColorMatrix brightness(f32 a);
    static ColorMatrix contrast(f32 a);
    static ColorMatrix saturate(f32 s);
    static ColorMatrix grayscale(f32 a);  ///< 0..1
    static ColorMatrix sepia(f32 a);      ///< 0..1
    static ColorMatrix hueRotate(f32 deg);
    static ColorMatrix invert(f32 a);     ///< 0..1
    static ColorMatrix opacity(f32 a);    ///< 0..1
    /// this filter, then `next`
    ColorMatrix then(const ColorMatrix& next) const;
    bool identity() const { return *this == ColorMatrix{}; }
    /// out = M * (r, g, b, a, 1), clamped to 0..1 (straight alpha in and out)
    void apply(const f32 in[4], f32 out[4]) const;
    friend bool operator==(const ColorMatrix&, const ColorMatrix&) = default;
};

/// CSS drop-shadow(<dx> <dy> <blur> <color>): the alpha of the filtered image, blurred (box radius round(blur / 6),
/// three passes), offset by whole pixels (rounded), tinted and drawn under it.
struct DropShadow {
    Vec2 offset{};
    f32 blur = 0.f;
    Color color{};
    bool visible() const { return color.a > 0.f; }
};

struct Image;
/// An SVG <filter> as a program over float planes of the filter region (premultiplied 0..255 RGBA per pixel).
/// Plane 0 = SourceGraphic (and the result: the last op writes it), plane 1 = blur scratch, 2.. = results.
/// Ops run in order; CPU (runFilterProgram) and GPU (ui_backdrop.glsl modes 7..9) share the arithmetic. Each op's
/// output is cleared outside its primitive subregion (pixel centres). Lengths are absolute pixels once resolved.
enum class FeOp : u8 {
    Blur,      ///< p0 / p1 = stdDeviation x / y (box radius max(1, round(sigma)), three passes); in1 -> out
    Matrix,    ///< p0..19 = 4 rows of (r g b a offset) on unpremultiplied 0..1 colour (as ColorMatrix)
    Offset,    ///< p0 / p1 = dx / dy (rounded to whole pixels): out(x, y) = in1(x - dx, y - dy), 0 outside
    Flood,     ///< p0..3 = straight r g b a: every pixel
    Blend,     ///< p0 = mode (0 normal, 1 multiply, 2 screen, 3 darken, 4 lighten): in1 over in2
    Composite, ///< p0 = operator (0 over, 1 in, 2 out, 3 atop, 4 xor, 5 arithmetic k1..k4 = p1..p4): in1 op in2
    Alpha,     ///< out = (0, 0, 0, a of in1) (SourceAlpha)
    Copy,      ///< out = in1
    Turbulence, ///< SVG reference Perlin noise: p0/p1 base frequency, p2 octaves, p3 type (0 turbulence, 1 fractal),
                ///< p4 stitch, p5/p6 user-space origin, p7..p10 stitch tile; data = lattice + gradients (seed)
    Lighting,  ///< p0 0 diffuse / 1 specular, p1 surfaceScale, p2 kd|ks, p3 specularExponent, p4..6 light colour,
               ///< p7 light 0 distant / 1 point / 2 spot, p8..10 direction|position, p11..13 spot axis, p14 spot
               ///< exponent, p15 cos(cone) (-2: none); normals from in1's alpha (Sobel, SVG edge factors)
    Morphology, ///< p0 0 erode / 1 dilate, p1 / p2 radius x / y: per-channel min / max (transparent outside)
    Image,     ///< p0..3 destination rect, p4 / p5 image size; data = premultiplied RGBA (nearest sampled)
    Tile,      ///< p0..3 the input tile rect: in1 repeated over the subregion
    Transfer,  ///< one feFunc: p0 channel, p1 type (0 identity, 1 table, 2 discrete, 3 linear, 4 gamma), p2 slope |
               ///< amplitude, p3 intercept | exponent, p4 offset, p5 table size; data = table values
    Convolve,  ///< p0 / p1 order, p2 1 / divisor, p3 bias, p4 / p5 target, p6 edge (0 duplicate, 1 wrap, 2 none),
               ///< p7 preserveAlpha; data = kernel (row-major, as written)
    Displace,  ///< p0 scale, p1 / p2 x / y channel (0..3): in1 sampled at the in2-displaced position (nearest)
};
struct FilterOpDesc {
    FeOp op = FeOp::Copy;
    u8 in1 = 0, in2 = 0, out = 0;
    std::array<f32, 25> p{};
    std::array<f32, 4> sub{}; ///< primitive subregion x y width height (as written: see subSet / subPct)
    u8 subSet = 0, subPct = 0; ///< bit i: sub[i] given / given as a percentage
    std::vector<f32> data;
};
struct FilterProgram {
    std::vector<FilterOpDesc> ops;
    u8 planes = 2;
    std::array<f32, 4> region{-10.f, -10.f, 120.f, 120.f}; ///< filter region as written (default -10% / 120%)
    u8 regionSet = 0, regionPct = 0xF;
    bool filterUnitsUser = false;  ///< filterUnits="userSpaceOnUse" (default objectBoundingBox)
    bool primitiveUnitsBox = false; ///< primitiveUnits="objectBoundingBox" (default userSpaceOnUse)
    bool resolved = false;
    Rect regionPx{}; ///< resolved: the filter region in pixels
    bool empty() const { return ops.empty(); }
    /// Absolute pixels for the element bounding box `bbox` (user space origin = its top-left): filter region,
    /// primitive subregions, objectBoundingBox lengths, stitch tiles.
    FilterProgram resolve(const Rect& bbox) const;
    /// pixels the result can reach beyond the source ink (blur 3r, offsets, ...), summed along the op list
    int reach() const;
};
/// Resolves an feImage href (a name given to UiContext::defineFilterImage); nullptr: unknown.
using FilterImageResolver = std::function<const Image*(std::string_view)>;
/// Parses an SVG `<filter>` element (feGaussianBlur, feColorMatrix, feOffset, feFlood, feBlend, feComposite, feMerge,
/// feDropShadow, feTurbulence, feDiffuseLighting, feSpecularLighting, feMorphology, feImage, feTile,
/// feComponentTransfer, feConvolveMatrix, feDisplacementMap; in / in2 / result; x / y / width / height on the filter
/// and primitives, filterUnits, primitiveUnits). `text` may hold more markup: the first <filter> (or the one with id
/// `id`) is used. Colour math in sRGB (color-interpolation-filters is not honoured). False on errors.
bool parseSvgFilter(std::string_view text, FilterProgram& out, std::string_view id = {}, const FilterImageResolver& images = {});
/// The ids of every <filter id="..."> in `text`.
std::vector<std::string> svgFilterIds(std::string_view text);
/// Encoded program layout in a stop table (DrawCmd::u == 1): header 2 stops {K, (planes, margin, dataStops, 0)},
/// {0, region}; per op 7 stops {op, (in1, in2, out, dataIndex)}, {0, subregion}, 5 parameter stops; then the data
/// floats, 5 per stop (r g b a offset). Appends to `stops`; false (nothing appended) when it does not fit.
bool encodeFilterProgram(const FilterProgram& resolved, std::vector<GradientStop>& stops, u16& count);
/// Recomputes derived words (box radii, whole-pixel offsets, margin) after a change; scale multiplies lengths.
void scaleFilterProgram(GradientStop* prog, f32 scale);
/// Kind of op `i` of an encoded program.
FeOp filterProgramOp(const GradientStop* prog, u32 i);
/// Runs an encoded program over `plane0` (w x h x 4 floats, the layer at pixel origin x0, y0); result in plane0.
void runFilterProgram(const GradientStop* prog, std::vector<f32>& plane0, int w, int h, int x0, int y0, bool hdr = false);

/// A multi-stop gradient at any angle / ellipse / transform. `toUnit` maps a pixel to gradient space (affine
/// [a b c d e f]: x' = a x + c y + e, y' = b x + d y + f).
struct Gradient {
    bool radial = false;
    GradientSpread spread = GradientSpread::Pad;
    std::array<f32, 6> toUnit{1.f, 0.f, 0.f, 1.f, 0.f, 0.f};
    std::vector<GradientStop> stops;
    Vec2 focal{0.f, 0.f}; ///< radial: focal point in gradient space (0 = the centre); kept inside |f| <= 0.99
    /// t = 0 at `from`, 1 at `to` (SVG linearGradient x1 y1 x2 y2).
    static Gradient linear(Vec2 from, Vec2 to, std::vector<GradientStop> stops, GradientSpread spread = GradientSpread::Pad);
    /// CSS linear-gradient(<angle>deg, ...) over `box`: 0 = to top, 90 = to right; the gradient line spans the box
    /// corners (length |w sin a| + |h cos a|).
    static Gradient linearAngle(const Rect& box, f32 degrees, std::vector<GradientStop> stops);
    /// t = 0 at `centre`, 1 on the ellipse (rx, ry) (SVG radialGradient cx cy r; CSS radial-gradient ellipse).
    static Gradient radialAt(Vec2 centre, f32 rx, f32 ry, std::vector<GradientStop> stops, GradientSpread spread = GradientSpread::Pad);
    /// As radialAt with t = 0 at the focal point `focus` (SVG radialGradient fx fy; focal radius 0).
    static Gradient radialFocal(Vec2 centre, f32 rx, f32 ry, Vec2 focus, std::vector<GradientStop> stops,
                                GradientSpread spread = GradientSpread::Pad);
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
    std::array<f32, 2> focal{}; ///< kind 6: focal point in gradient space
};

class DrawList {
public:
    void clear() {
        m_cmds.clear();
        m_edges.clear();
        m_stops.clear();
        m_filterStack.clear();
        m_pendingPrograms.clear();
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
    /// Backdrop filter (CSS backdrop-filter: blur() then colour functions): blur (0 = none), then `filter` per pixel.
    /// The matrix rides in gradientStops() (4 rows of 5 as stops; DrawCmd::stops / stopCount = 4).
    void backdrop(const Rect& r, f32 blur, const std::array<f32, 4>& radii, const ColorMatrix& filter, Color tint = {});
    /// As above plus a drop shadow of the filtered backdrop (CSS backdrop-filter: drop-shadow()): drawn under the
    /// backdrop's own pixels, so it only shows where the backdrop is translucent. Shadow data: stops 4 / 5 (stopCount 6).
    void backdrop(const Rect& r, f32 blur, const std::array<f32, 4>& radii, const ColorMatrix& filter, const DropShadow& shadow,
                  Color tint = {});
    /// CSS `filter` on an element: what is drawn until popFilter() goes to an isolated (transparent) layer, which is
    /// then blurred (`blur`, as backdrop), passed through `matrix`, given `shadow`, and composited over the pixels
    /// under it (within the clip in force; clip paths and rounded clips multiply). Fixed order: blur, matrix, shadow.
    /// The layer is a backdrop root: a backdrop inside it sees only the layer. Nests.
    /// `chained`: an inner layer of one filter chain (CSS functions in written order become nested layers, the first
    /// innermost): composited without the clip coverage, which the outermost layer applies once.
    void pushFilter(const ColorMatrix& matrix, f32 blur = 0.f, const DropShadow& shadow = {}, bool chained = false);
    /// A filter layer running an SVG filter program (see FilterProgram / encodeFilterProgram; DrawCmd::u = 1). The
    /// program is resolved at popFilter against `bbox` (empty: the ink bounds); the layer box is its filter region.
    void pushFilter(const FilterProgram& program, bool chained = false, const Rect& bbox = {});
    /// Backdrop filter running an SVG program (resolved against `r`), then `tint` over it.
    void backdrop(const Rect& r, const FilterProgram& program, const std::array<f32, 4>& radii, Color tint = {});
    /// Ends the innermost pushFilter: sets both commands' `rect` to the layer's pixel box (ink bounds of the commands
    /// in between, grown by the blur and shadow reach).
    void popFilter();
    /// CSS 2D `transform` of a painted subtree: commands up to popTransform() draw in local (untransformed)
    /// coordinates into an isolated layer, which is then resampled (bilinear, premultiplied) through
    /// m = {a, b, c, d, e, f} (x' = a x + c y + e, y' = b x + d y + f) over the pixels under it. Encoded as a
    /// PushFilter / PopFilter pair with DrawCmd::u = 2 and gradXf = the inverse map (pixel -> local); the layer box
    /// is the union of the local ink and its transformed bounds. A singular m drops the commands in between. Nests
    /// with filters and other transforms. Local ink outside the clip in force (or the target) is not kept.
    void pushTransform(const std::array<f32, 6>& m);
    void popTransform() { popFilter(); }
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
    std::size_t edgeCount() const { return m_edges.size(); }
    /// Maps commands [fromCmd, size()) and path edges [fromEdge, edgeCount()) by p' = p * scale + offset (CSS
    /// transform of a painted subtree). Radii, border widths and blur scale by the smaller factor; glyphs move but
    /// keep their texel size; filter-program lengths are not rescaled.
    void transformRange(std::size_t fromCmd, std::size_t fromEdge, Vec2 scale, Vec2 offset);

private:
    friend void scaleDrawList(const DrawList& in, f32 scale, DrawList& out);
    std::vector<DrawCmd> m_cmds;
    std::vector<PathEdge> m_edges;
    std::vector<GradientStop> m_stops;
    std::vector<u32> m_filterStack; ///< open pushFilter command indices
    struct PendingProgram { u32 cmd; FilterProgram program; Rect bbox; };
    std::vector<PendingProgram> m_pendingPrograms; ///< programs of open pushFilters, encoded at popFilter
    void putFilter(DrawCmd& c, const ColorMatrix& m, const DropShadow& shadow);
    void applyGradient(DrawCmd& c, const Gradient& g);
};

/// PushFilter / PopFilter pairing: partner[i] = the matching command's index, ~0u for other commands and for an
/// unmatched push / pop (both rasterisers ignore those).
void matchFilters(const DrawList& list, std::vector<u32>& partner);
/// A PushFilter / PopFilter / Backdrop carrying a FilterProgram (DrawCmd::u == 1) rather than matrix rows (4 / 6).
bool isFilterProgram(const DrawCmd& c);
/// A PushFilter / PopFilter of DrawList::pushTransform (DrawCmd::u == 2; gradXf = pixel -> local map).
inline bool isTransformLayer(const DrawCmd& c) {
    return (c.op == DrawOp::PushFilter || c.op == DrawOp::PopFilter) && c.u == 2;
}
/// Bilinear sample (zero outside) of a premultiplied float box `buf` (w x h, 4 floats per pixel) at box-local
/// continuous point (fx, fy) (texel centres at i + 0.5); ui_backdrop.glsl mode 6 has the same arithmetic.
void sampleLayerBilinear(const f32* buf, int w, int h, f32 fx, f32 fy, f32 out[4]);

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

/// CPU raster target precision: Rgba8 is Image; the float formats are HdrImage.
enum class PixelFormat : u8 { Rgba8, Rgba16F, Rgba32F };

/// One premultiplied float pixel of an HdrImage (extended-range sRGB-encoded, 1.0 = SDR white; rgb may exceed 1).
struct PixelF {
    f32 r = 0.f, g = 0.f, b = 0.f, a = 0.f;
};

/// Float CPU raster target (the CPU counterpart of the GPU HDR canvas, UiVkRenderer::setTargetHdr): blends in float
/// without 8-bit rounding and keeps colours above SDR white (color::hdr, Image::hdrPixels). Pixels are held as f32;
/// with `format` Rgba16F, CpuRasterizer::render rounds what it drew to binary16 (what an RGBA16F canvas stores).
struct HdrImage {
    int width = 0, height = 0;
    PixelFormat format = PixelFormat::Rgba32F;
    std::vector<PixelF> pixels;
    void resize(int w, int h, PixelF clear = {0.f, 0.f, 0.f, 1.f}) {
        width = w;
        height = h;
        pixels.assign(std::size_t(w) * std::size_t(h), clear);
    }
    const PixelF& at(int x, int y) const { return pixels[std::size_t(y) * std::size_t(width) + std::size_t(x)]; }
    /// Pixels [x0, x1) of row y as RGBA16F (r | g << 16 | b << 32 | a << 48), e.g. for an RGBA16F canvas upload.
    void packHalfRow(int y, int x0, int x1, u64* out) const;
    /// Quantises the box to 8-bit premultiplied (as the 8-bit path stores: clamp to [0, 1], round) into `out`.
    void toRgba8(Image& out) const;
};

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
    /// Float raster (HDR): same coverage, blur, gradients, images and text as the 8-bit render, blended in float like
    /// ui_draw_list_hdr / ui_backdrop_hdr (rgb >= 0, alpha in [0, 1], no per-blend quantisation). Images with
    /// hdrPixels draw those; others their 8-bit pixels.
    void render(const DrawList& list, HdrImage& target, Rect scissor = {}) const;
    /// The registered images, by index (what packDrawList needs for the GPU path).
    const Image* const* images() const { return m_images.data(); }
    u32 imageCount() const { return u32(m_images.size()); }

private:
    const u8* m_atlas = nullptr;
    int m_atlasSize = 0;
    std::vector<const Image*> m_images;
};

} // namespace fuse::ui
