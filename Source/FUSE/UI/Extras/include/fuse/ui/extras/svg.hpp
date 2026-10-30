// SVG documents (Qt SVG parity: QSvgRenderer / QSvgWidget, SVG Tiny 1.2 static subset). Parses <svg> (width, height,
// viewBox, preserveAspectRatio meet/none), <g>, <rect rx ry>, <circle>, <ellipse>, <line>, <polyline>, <polygon>,
// <path d> (full path grammar incl. arcs), <use href="#id">, <defs> with <linearGradient> / <radialGradient> stops;
// presentation attributes and style="" (fill, stroke, stroke-width, fill-rule, opacity, fill-opacity,
// stroke-opacity, display, visibility), inherited through groups, and transform lists (matrix, translate, scale,
// rotate, skewX, skewY). Gradients are multi-stop at any angle (x1..y2 / cx cy r, gradientUnits, gradientTransform,
// spreadMethod, href inheritance; the radial focal point is not modelled). <text> / <tspan> (x y dx dy, font-size,
// text-anchor, letter-spacing) become glyph-outline paths of FUSE's bundled font, so they fill, stroke, clip and
// transform like any shape. clip-path="url(#c)" clips through DrawList clip paths (nested clips intersect; a
// clipPath's children are unioned). mask="url(#m)" is approximated: the mask content's geometry clips and its mean
// luminance x alpha scales the opacity (no per-pixel luminance). fill="url(#pattern)" tiles the pattern content
// (patternUnits / patternContentUnits / viewBox / patternTransform) clipped to the shape. Colours go through the CSS colour parser. Rendering emits DrawList paths, so it rasterises
// with the CPU path rasteriser or the GPU draw-list kernel like any other vector content.
#pragma once

#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/ui.hpp"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::svg {

using Affine = std::array<f32, 6>; ///< [a b c d e f]: x' = a x + c y + e, y' = b x + d y + f

struct Shape {
    Path path;             ///< in document (viewBox) user space, transforms applied
    Color fill, stroke;    ///< .a == 0: none (opacity folded in)
    Color fill2;           ///< gradient end colour when gradient != kGradientNone
    GradientKind gradient = kGradientNone;
    f32 strokeWidth = 0.f; ///< user-space, scaled by the transform
    bool evenOdd = false;
    std::string id; ///< the element's own id (groups: see Document::boundsOf)
    Gradient paint; ///< multi-stop fill in document space; used instead of fill / fill2 when paint.stops is non-empty
    int clip = -1;    ///< Document::clips() index of the innermost clip region (chained through ClipRegion::parent)
    int pattern = -1; ///< Document::patterns() index: the fill is that pattern's tiles (fill.a = its opacity)
};

/// A clip region in document space (clipPath geometry, or a mask's geometry).
struct ClipRegion {
    Path path; ///< all geometry merged (bounds, hit tests)
    bool evenOdd = false;
    /// When non-empty, the exact region drawn: one layer per clipPath child (a union, each with its own clip-rule)
    /// or per mask child (per-pixel luminance x alpha, gradients as a ramp between their end stops).
    std::vector<ClipLayer> layers;
    int parent = -1; ///< outer region also clipping (intersection), -1 none
};

/// A pattern fill: tile rect in pattern space, pattern space -> document, tile-local content -> pattern space.
struct PatternFill {
    Rect tile{};
    Affine toDoc{1, 0, 0, 1, 0, 0};
    Affine content{1, 0, 0, 1, 0, 0};
    std::vector<Shape> shapes;
};

class Document {
public:
    /// Parse SVG text; false (with error()) when there is no <svg> root or the markup is malformed.
    bool load(std::string_view svgText);
    bool loadFile(const std::string& path);
    bool valid() const { return m_valid; }
    const std::string& error() const { return m_error; }
    /// viewBox (or 0 0 width height).
    Rect viewBox() const { return m_viewBox; }
    /// Intrinsic size (width / height attributes, else the viewBox size).
    Size size() const { return m_size; }
    const std::vector<Shape>& shapes() const { return m_shapes; }
    const std::vector<ClipRegion>& clips() const { return m_clips; }
    const std::vector<PatternFill>& patterns() const { return m_patterns; }
    /// Bounds of the element with `id` in document space (QSvgRenderer::boundsOnElement); empty when unknown.
    Rect boundsOf(std::string_view id) const;
    /// Draws the document fitted into `target` (aspect kept and centred unless preserveAspectRatio="none").
    void render(DrawList& dl, Rect target, f32 opacity = 1.f) const;

private:
    std::vector<Shape> m_shapes;
    std::vector<ClipRegion> m_clips;
    std::vector<PatternFill> m_patterns;
    struct IdRange { std::string id; std::size_t first, last; }; ///< element id -> shapes it produced
    std::vector<IdRange> m_ids;
    Rect m_viewBox{};
    Size m_size{};
    bool m_valid = false, m_stretch = false;
    std::string m_error;
};

/// Parses an SVG transform list into an affine matrix (identity on empty / bad input).
Affine parseTransform(std::string_view t);

} // namespace fuse::ui::svg

namespace fuse::ui::widgets {
/// QSvgWidget: paints an SVG document scaled into its content box (intrinsic size = the document size).
NodeHandle svgWidget(UiContext& ui, NodeHandle parent, std::string_view svgText, std::string_view id = {});
bool svgWidgetLoad(UiContext& ui, NodeHandle w, std::string_view svgText);
const svg::Document* svgWidgetDocument(const UiContext& ui, NodeHandle w);
} // namespace fuse::ui::widgets
