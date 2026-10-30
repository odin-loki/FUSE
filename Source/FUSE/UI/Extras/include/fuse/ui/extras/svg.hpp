// SVG documents (Qt SVG parity: QSvgRenderer / QSvgWidget, SVG Tiny 1.2 static subset). Parses <svg> (width, height,
// viewBox, preserveAspectRatio meet/none), <g>, <rect rx ry>, <circle>, <ellipse>, <line>, <polyline>, <polygon>,
// <path d> (full path grammar incl. arcs), <use href="#id">, <defs> with <linearGradient> / <radialGradient> stops;
// presentation attributes and style="" (fill, stroke, stroke-width, fill-rule, opacity, fill-opacity,
// stroke-opacity, display, visibility), inherited through groups, and transform lists (matrix, translate, scale,
// rotate, skewX, skewY). Colours go through the CSS colour parser. Rendering emits DrawList paths, so it rasterises
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
    /// Bounds of the element with `id` in document space (QSvgRenderer::boundsOnElement); empty when unknown.
    Rect boundsOf(std::string_view id) const;
    /// Draws the document fitted into `target` (aspect kept and centred unless preserveAspectRatio="none").
    void render(DrawList& dl, Rect target, f32 opacity = 1.f) const;

private:
    std::vector<Shape> m_shapes;
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
