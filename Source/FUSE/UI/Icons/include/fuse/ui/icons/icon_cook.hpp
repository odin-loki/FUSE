// FUSE UI icon cooker (fuse_ui_icon_cook_lib): SVG icons -> a .fuseicons pack (format in fuse/ui/icon_set.hpp).
// Build / tool time: the runtime (IconSet) loads the pack without parsing any SVG.
//
// The SVG files are read with the module's own parser (fuse::ui::SvgImage, fuse/ui/svg.hpp: path data, rect, circle,
// ellipse, line, polyline, polygon, groups and transforms, fill / stroke with currentColor, opacities, fill rules,
// dashes, linear and radial gradients). There is no second SVG stack: the merged branch's cooker rasterised with
// nanosvg into coverage atlases; this one keeps the parsed vector shapes, so cooked icons render through the same
// Painter path as every other vector drawing. An SVG with elements the parser does not support (<text>, <use>,
// <image>, <clipPath>, <mask>, <filter>, ...) is rejected rather than cooked wrong.
//
// Colour: an icon whose paints are all currentColor or black (the SVG default fill) is monochrome: the runtime tints
// it with the widget's text colour. Any other colour makes it multi-colour: currentColor paints stay tintable, the
// others keep their colour (black included), gradients stay gradients.
//
// Output is a pure function of the inputs (icons sorted by name).
#pragma once

#include "fuse/ui/icon_set.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::icons {

struct SvgSource {
    std::string name; ///< lookup name (file stem): [a-z0-9_.-], unique
    std::string svg;  ///< file text
};

struct CookStats {
    u32 icons = 0;
    u32 multiColor = 0; ///< icons with paints of their own colour
    u32 shapes = 0;
    std::size_t bytes = 0;
};

/// Cooks `sources` (any order) to a .fuseicons pack. Fails with a message naming the icon on a malformed SVG, an
/// unsupported element, an SVG without painted shapes or size, and a bad or duplicate name.
std::expected<std::vector<u8>, std::string> cookIcons(std::vector<SvgSource> sources, CookStats* stats = nullptr);
/// Reads every *.svg in `dir` (not recursive) as name = file stem, sorted by name.
std::expected<std::vector<SvgSource>, std::string> loadSvgDirectory(const std::string& dir);
/// Reads one .svg file (name = file stem).
std::expected<SvgSource, std::string> loadSvgFile(const std::string& path);

} // namespace fuse::ui::icons
