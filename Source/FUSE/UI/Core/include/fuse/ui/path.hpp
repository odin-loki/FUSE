// FUSE UI vector paths (plan §4.9): Affine transforms, Path (move / line / quad / cubic / arc / close, shape helpers,
// SVG path data), flattening, stroking (caps, joins, miter limit, dashes) and exact-area coverage rasterisation.
//
// These are the building blocks of DrawOp::Path (draw_list.hpp), Painter (painter.hpp) and SvgImage (svg.hpp). The
// free functions work on caller-owned buffers that keep their capacity, so a warmed-up caller does not allocate.
#pragma once

#include "fuse/ui/types.hpp"

#include <array>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace fuse::ui {

/// 2x3 affine transform: x' = a x + c y + tx, y' = b x + d y + ty (the SVG / canvas `matrix(a b c d tx ty)`).
struct Affine {
    f32 a = 1.f, b = 0.f, c = 0.f, d = 1.f, tx = 0.f, ty = 0.f;
    static constexpr Affine translation(f32 x, f32 y) { return {1.f, 0.f, 0.f, 1.f, x, y}; }
    static constexpr Affine scaling(f32 sx, f32 sy) { return {sx, 0.f, 0.f, sy, 0.f, 0.f}; }
    /// Rotation by `radians`; positive turns +x towards +y (clockwise on a y-down screen), as in SVG and QPainter.
    static Affine rotation(f32 radians) {
        const f32 s = std::sin(radians), co = std::cos(radians);
        return {co, s, -s, co, 0.f, 0.f};
    }
    Vec2 apply(Vec2 p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    Vec2 applyVector(Vec2 v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }
    /// (*this * o) applies `o` first, then *this.
    Affine operator*(const Affine& o) const {
        return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d,
                a * o.tx + c * o.ty + tx, b * o.tx + d * o.ty + ty};
    }
    f32 det() const { return a * d - b * c; }
    /// False (and `out` untouched) when singular.
    bool invert(Affine& out) const;
    /// sqrt(|det|): the factor lengths (stroke widths) scale by under a similarity.
    f32 meanScale() const { return std::sqrt(std::abs(det())); }
    /// Largest stretch of any direction (the larger singular value).
    f32 maxScale() const;
    bool isIdentity() const { return a == 1.f && b == 0.f && c == 0.f && d == 1.f && tx == 0.f && ty == 0.f; }
    /// No rotation / skew: axis-aligned rects stay axis-aligned rects.
    bool isScaleTranslate() const { return b == 0.f && c == 0.f; }
    /// Rotation + uniform scale (+ mirror) + translation: circles stay circles, a stroke keeps a constant width.
    bool isSimilarity() const;
    friend bool operator==(const Affine&, const Affine&) = default;
};

enum class PathVerb : u8 { Move, Line, Quad, Cubic, Close }; ///< points consumed: 1, 1, 2, 3, 0
enum class FillRule : u8 { NonZero, EvenOdd };
enum class LineCap : u8 { Butt, Round, Square };
enum class LineJoin : u8 { Miter, Round, Bevel };

/// Stroke geometry. A miter join longer than `miterLimit` x width / 2 falls back to a bevel (SVG / Qt rule).
/// `dashes` are on / off lengths (an odd count repeats, as in SVG); none = solid.
struct StrokeStyle {
    static constexpr int kMaxDashes = 8;
    f32 width = 1.f;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    f32 miterLimit = 4.f;
    f32 dashOffset = 0.f;
    u8 dashCount = 0;
    std::array<f32, kMaxDashes> dashes{};
    /// Sets the dash pattern (at most kMaxDashes entries are kept).
    StrokeStyle& setDashes(std::span<const f32> pattern, f32 offset = 0.f);
    friend bool operator==(const StrokeStyle&, const StrokeStyle&) = default;
};

struct PathParseError {
    u32 offset = 0; ///< byte offset in the path data where parsing stopped
};

/// A vector path: sub-paths of lines, quadratic and cubic Béziers. Value type; clear() keeps the capacity.
class Path {
public:
    void clear() {
        m_verbs.clear();
        m_points.clear();
    }
    bool empty() const { return m_verbs.empty(); }
    Path& moveTo(Vec2 p);
    Path& lineTo(Vec2 p);
    Path& quadTo(Vec2 c, Vec2 p);
    Path& cubicTo(Vec2 c1, Vec2 c2, Vec2 p);
    Path& close();
    /// SVG elliptical arc from the current point to `to` (`xRotation` in radians).
    Path& arcTo(f32 rx, f32 ry, f32 xRotation, bool largeArc, bool sweep, Vec2 to);
    /// Arc of the ellipse (centre, rx, ry) from angle `start` over `sweep` radians (positive = clockwise on screen).
    /// Starts a new sub-path, or continues the current one with a line to the arc's start when `connect`.
    Path& addArc(Vec2 centre, f32 rx, f32 ry, f32 start, f32 sweep, bool connect = false);
    Path& addRect(const Rect& r);
    Path& addRoundedRect(const Rect& r, const std::array<f32, 4>& radii); ///< tl, tr, br, bl
    Path& addRoundedRect(const Rect& r, f32 radius) { return addRoundedRect(r, {radius, radius, radius, radius}); }
    Path& addEllipse(Vec2 centre, f32 rx, f32 ry);
    Path& addCircle(Vec2 centre, f32 r) { return addEllipse(centre, r, r); }
    Path& addPolyline(std::span<const Vec2> pts, bool closed);
    Path& addPath(const Path& other, const Affine* xf = nullptr);
    void transform(const Affine& xf);
    /// Parses SVG path data (`M L H V C S Q T A Z`, absolute and relative, implicit repeats). Fails on a syntax error.
    static std::expected<Path, PathParseError> fromSvg(std::string_view d);
    /// Bounds of the control points (contains the curve).
    Rect bounds() const;
    Vec2 currentPoint() const { return m_points.empty() ? Vec2{} : m_points.back(); }
    std::span<const PathVerb> verbs() const { return m_verbs; }
    std::span<const Vec2> points() const { return m_points; }

private:
    void ensureStart();
    std::vector<PathVerb> m_verbs;
    std::vector<Vec2> m_points;
    Vec2 m_start{};
    bool m_open = false; ///< a sub-path is open (a Move was emitted since the last Close)
};

/// Flattened contours.
struct Polyline {
    struct Contour {
        u32 first = 0, count = 0;
        bool closed = false;
    };
    std::vector<Vec2> points;
    std::vector<Contour> contours;
    void clear() {
        points.clear();
        contours.clear();
    }
    std::size_t segmentCount() const;
};

/// Flattens to line segments with a maximum deviation of `tolerance` (in the units of the output: when `xf` is given
/// the points are transformed first). Appends to `out`.
void flattenPath(std::span<const PathVerb> verbs, std::span<const Vec2> points, f32 tolerance, Polyline& out,
                 const Affine* xf = nullptr);
inline void flattenPath(const Path& p, f32 tolerance, Polyline& out, const Affine* xf = nullptr) {
    flattenPath(p.verbs(), p.points(), tolerance, out, xf);
}
/// Cuts `in` into the "on" intervals of a dash pattern (appends open contours to `out`). An empty / degenerate
/// pattern copies `in`.
void dashPolyline(const Polyline& in, std::span<const f32> dashes, f32 offset, Polyline& out);
/// The outline of the stroke of `in` as closed polygons that all wind the same way: fill it with FillRule::NonZero.
/// Dashes in `style` are applied. Round caps / joins are flattened to `tolerance`. Appends to `out`.
void strokePolyline(const Polyline& in, const StrokeStyle& style, f32 tolerance, Polyline& out);

/// Exact-area antialiased coverage of the closed contours of `poly` (open ones are closed implicitly) for the pixel
/// rows [rowBegin, rowEnd) and columns [x0, x0 + w): out[(row - rowBegin) * w + column] = 0..255. Geometry left of x0
/// still counts (the fill continues into the window); a row's result does not depend on which other rows are asked
/// for. Overlapping same-direction contours saturate (non-zero); `acc` is scratch.
void rasterizeCoverage(const Polyline& poly, FillRule rule, int x0, int w, int rowBegin, int rowEnd, u8* out,
                       std::vector<f32>& acc);
/// The same coverage, row by row and without a mask: `fn` is called once for every row of [rowBegin, rowEnd), in
/// order, with the columns [xBegin, xEnd) that can be covered (an empty range for an untouched row) and their
/// coverage (`coverage[0]` is column xBegin; valid during the call). Memory and time follow the covered spans, not the
/// bounding box - what large paths (a line across the window) are rendered with.
using CoverageRowFn = void (*)(void* user, int row, int xBegin, int xEnd, const u8* coverage);
void rasterizeCoverageRows(const Polyline& poly, FillRule rule, int x0, int w, int rowBegin, int rowEnd, CoverageRowFn fn,
                           void* user, std::vector<f32>& acc);

} // namespace fuse::ui
