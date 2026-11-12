// FUSE UI icon cooker (fuse_ui_icon_cook_lib, plan §4.9 / UI4): SVG icons -> a .fuseicons pack (format in
// fuse/ui/icons.hpp). Build / tool time only; the runtime (IconSet) never parses SVG.
//
// SVG subset (vendored nanosvg): path d (M L H V C S Q T A Z, relative forms), rect (rx / ry), circle, ellipse, line,
// polyline, polygon, g with transform (matrix / translate / scale / rotate / skew), fill, stroke, stroke-width,
// stroke-linecap / -linejoin / -miterlimit, opacity / fill-opacity / stroke-opacity, fill-rule, viewBox, width /
// height, currentColor, gradients. Text and images are not supported.
//
// Colour: an icon whose paints are all currentColor or black (the SVG default fill) is monochrome — one coverage layer
// the runtime tints with the widget's text colour. Any other colour makes it multi-colour: its paints, in paint order,
// are grouped into layers of consecutive equal colour (a COLR-style layered palette); currentColor layers stay
// tintable, the others keep their colour (black included), gradients become their mean stop colour (the stops' alpha
// stays in the coverage). The runtime draws the layers as tinted coverage glyphs, back to front (edges shared by two
// layers blend like independently anti-aliased shapes).
//
// Each icon is rasterised (nanosvgrast: exact horizontal coverage, 5 vertical subsamples) into a size x size cell per
// cooked size, scaled to fit and centred, trimmed to its covered box and shelf-packed into one atlas. Output is a
// pure function of the inputs (names sorted, fixed packing order).
#pragma once

#include "fuse/ui/types.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::icons {

struct SvgSource {
    std::string name; ///< lookup name (file stem): [a-z0-9_.-], unique
    std::string svg;  ///< file text
};

struct CookOptions {
    std::vector<int> sizes{16, 20, 24, 32, 48}; ///< pixel sizes (1..512), cooked in this order
};

struct CookStats {
    u32 icons = 0;
    int atlasWidth = 0, atlasHeight = 0;
    std::size_t bytes = 0;
    u32 multiColor = 0; ///< icons with fixed-colour layers
};

/// One colour layer of an icon: `color` RGBA8 (R low byte), 0 = currentColor.
struct SvgLayer {
    u32 color = 0;
    std::vector<u8> coverage; ///< size x size
};

/// Rasterises one SVG to size x size coverage (row-major). Fails when the SVG has no size or no painted shapes.
std::expected<std::vector<u8>, std::string> rasteriseSvg(std::string_view svg, int size);
/// The colour layers of one SVG at size x size (one currentColor layer = the whole coverage for a monochrome icon).
std::expected<std::vector<SvgLayer>, std::string> rasteriseSvgLayers(std::string_view svg, int size);
/// Cooks `sources` (any order) to a .fuseicons pack.
std::expected<std::vector<u8>, std::string> cookIcons(std::vector<SvgSource> sources, const CookOptions& options = {},
                                                      CookStats* stats = nullptr);
/// Reads every *.svg in `dir` (not recursive) as name = file stem, sorted by name.
std::expected<std::vector<SvgSource>, std::string> loadSvgDirectory(const std::string& dir);
/// Reads one .svg file (name = file stem).
std::expected<SvgSource, std::string> loadSvgFile(const std::string& path);

} // namespace fuse::ui::icons
