// FUSE UI SVG subset (icons and simple illustrations): parsed once into a retained SvgImage that renders through a
// Painter at any size, so it is as sharp as the render scale. No external dependency (own XML subset scanner).
//
// Supported
//   structure   <svg> (viewBox, width, height), <g>, <defs>; nested transforms (matrix / translate / scale / rotate /
//               skewX / skewY); comments, <?xml?>, <!DOCTYPE>, CDATA and text are skipped
//   shapes      <path> (full path data), <rect> (rx / ry), <circle>, <ellipse>, <line>, <polyline>, <polygon>
//   paint       fill, stroke (colour names, #rgb / #rgba / #rrggbb / #rrggbbaa, rgb() / rgba(), none, currentColor,
//               url(#gradient)), fill-rule, fill-opacity, stroke-opacity, opacity, stroke-width, stroke-linecap,
//               stroke-linejoin, stroke-miterlimit, stroke-dasharray, stroke-dashoffset, color, display="none";
//               as attributes or in style="..."; inherited through groups
//   gradients   <linearGradient> / <radialGradient> with <stop>s (offset, stop-color, stop-opacity), gradientUnits,
//               gradientTransform, spreadMethod, href / xlink:href (stops only)
// Not supported (counted in unsupportedElements(), never an error): <text>, <use>, <image>, <clipPath>, <mask>,
// <filter>, <pattern>, <marker>, <symbol>, <style> sheets, animation. Approximations: `opacity` multiplies the fill
// and stroke alphas (no group compositing), a radial gradient's focal point (fx, fy) is ignored, lengths are user
// units (unit suffixes are ignored, percentages only in gradients).
#pragma once

#include "fuse/ui/painter.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui {

enum class SvgErrorKind : u8 {
    Syntax,    ///< malformed XML (unterminated tag / attribute, mismatched end tag, ...)
    NoSvgRoot, ///< no <svg> element
    BadValue,  ///< a required value does not parse (path data, a shape's number)
    Limit,     ///< too deeply nested or too many elements
};
struct SvgError {
    SvgErrorKind kind = SvgErrorKind::Syntax;
    u32 offset = 0; ///< byte offset in the document
    std::string message;
};

class SvgImage {
public:
    /// Parses a document. Fails cleanly on malformed input (never crashes, bounded work).
    static std::expected<SvgImage, SvgError> parse(std::string_view xml);

    /// Intrinsic size: width / height, else the viewBox size, else 0.
    Size size() const { return m_size; }
    /// The viewBox (width / height when there is none).
    Rect viewBox() const { return m_viewBox; }
    bool empty() const { return m_shapes.empty(); }
    u32 shapeCount() const { return u32(m_shapes.size()); }
    u32 gradientCount() const { return u32(m_gradients.size()); }
    u32 unsupportedElements() const { return m_unsupported; }
    /// Path segments of all shapes (verbs), a size measure.
    u32 segmentCount() const;

    /// Draws the image scaled into `dst` (local coordinates of `p`): the viewBox is fitted keeping its aspect ratio
    /// and centred (preserveAspectRatio xMidYMid meet), or stretched to `dst` when `stretch`. `currentColor` replaces
    /// fill / stroke="currentColor" (icon tinting). Allocation-free once the draw list is warm.
    void render(Painter& p, const Rect& dst, Color currentColor = Color::rgb(0xFFFFFF), bool stretch = false) const;

private:
    struct Paint {
        enum class Kind : u8 { None, Solid, Current, Gradient } kind = Kind::None;
        Color color;
        u32 gradient = 0;
        f32 opacity = 1.f;
    };
    struct Shape {
        Path path;
        Affine xf;
        Paint fill, stroke;
        StrokeStyle strokeStyle;
        FillRule rule = FillRule::NonZero;
    };
    struct GradientDef {
        GradientKind kind = GradientKind::Linear;
        GradientSpread spread = GradientSpread::Pad;
        bool bboxUnits = true;
        Affine xf;
        f32 x1 = 0.f, y1 = 0.f, x2 = 1.f, y2 = 0.f; ///< linear
        f32 cx = 0.5f, cy = 0.5f, r = 0.5f;         ///< radial
        std::vector<GradientStop> stops;
    };
    friend struct SvgParser;
    Brush brushFor(const Paint& paint, const Shape& s, Color currentColor, Gradient& storage) const;

    Size m_size;
    Rect m_viewBox;
    std::vector<Shape> m_shapes;
    std::vector<GradientDef> m_gradients;
    u32 m_unsupported = 0;
};

} // namespace fuse::ui
