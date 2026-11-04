// FUSE UI icon cooker (fuse_ui_icon_cook_lib, plan §4.9 / UI4): SVG icons -> a .fuseicons pack (format in
// fuse/ui/icons.hpp). Build / tool time only; the runtime (IconSet) never parses SVG.
//
// SVG subset (vendored nanosvg): path d (M L H V C S Q T A Z, relative forms), rect (rx / ry), circle, ellipse, line,
// polyline, polygon, g with transform (matrix / translate / scale / rotate / skew), fill, stroke, stroke-width,
// stroke-linecap / -linejoin / -miterlimit, opacity / fill-opacity / stroke-opacity, fill-rule, viewBox, width /
// height, currentColor. Icons are monochrome: every painted shape contributes coverage (its colour is dropped), and the
// runtime tints the coverage with the widget's text colour. Gradients paint as coverage too; text and images are not
// supported.
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
};

/// Rasterises one SVG to size x size coverage (row-major). Fails when the SVG has no size or no painted shapes.
std::expected<std::vector<u8>, std::string> rasteriseSvg(std::string_view svg, int size);
/// Cooks `sources` (any order) to a .fuseicons pack.
std::expected<std::vector<u8>, std::string> cookIcons(std::vector<SvgSource> sources, const CookOptions& options = {},
                                                      CookStats* stats = nullptr);
/// Reads every *.svg in `dir` (not recursive) as name = file stem, sorted by name.
std::expected<std::vector<SvgSource>, std::string> loadSvgDirectory(const std::string& dir);
/// Reads one .svg file (name = file stem).
std::expected<SvgSource, std::string> loadSvgFile(const std::string& path);

} // namespace fuse::ui::icons
