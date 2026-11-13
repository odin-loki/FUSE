// FUSE UI icon packs (.fuseicons): named vector icons cooked from SVG files by fuse_ui_icon_cook
// (fuse/ui/icons/icon_cook.hpp) and drawn through the same SvgImage / Painter path as the built-in icons
// (fuse/ui/icons.hpp) and the `svgimage` widget. A pack holds every icon already parsed: paths, transforms, paints,
// stroke styles and gradients in binary form plus a name index, so loading one never runs the XML scanner and an
// icon is as sharp as the render scale at any size.
//
// Use: load a pack (IconSet::fromFile), attach it to the context (UiContext::setIcons) and name icons with the `icon`
// / `icon-size` style properties (`icon: save; icon-size: 24px`), widgets::setIcon(ui, node, "save") or the "icon" key
// of a .fuseui node. A name that the attached pack does not have falls back to the built-in icon of that name
// (fuse/ui/icons.hpp: "save", "folder", "close", ...). Paints written as currentColor take the widget's text colour
// (theme, hover, disabled); an icon whose paints are all currentColor or black (the SVG default fill) is cooked fully
// tintable; any other colour keeps its colour (multi-colour icons), gradients stay gradients.
//
// The pack format is this module's own (the merged `magical-bassi` branch cooked coverage atlases, versions 1 and 2;
// those packs are not loadable: recook the SVG sources).
//   "FICN" u32 version (3) | u32 iconCount | icons sorted by name:
//     u32 nameHash (hashId) | u16 nameLen | name | u8 flags (1 = multi-colour) | f32 width, height | f32 viewBox[4]
//     u32 gradientCount x { u8 kind, spread, bboxUnits | f32 xf[6] | f32 x1 y1 x2 y2 cx cy r | u16 stops x { f32 offset, rgba } }
//     u32 shapeCount x { f32 xf[6] | paint fill, stroke | f32 width | u8 cap, join | f32 miterLimit, dashOffset |
//                        u8 dashCount x f32 | u8 fillRule | u32 verbCount x u8 | u32 pointCount x { f32 x, y } }
//     paint = u8 kind (0 none, 1 solid, 2 currentColor, 3 gradient) | f32 rgba[4] | u32 gradient | f32 opacity
// Little endian. Loading validates every count, index and number: a bad pack is an error message, never a crash.
#pragma once

#include "fuse/ui/svg.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui {

inline constexpr char kIconPackMagic[4] = {'F', 'I', 'C', 'N'};
inline constexpr u32 kIconPackVersion = 3;

class IconSet {
public:
    /// Validates and loads a pack (copies what it needs).
    static std::expected<IconSet, std::string> fromBytes(std::span<const u8> bytes);
    /// `path` is UTF-8.
    static std::expected<IconSet, std::string> fromFile(const std::string& path);

    u32 count() const { return u32(m_names.size()); }
    /// Icons are sorted by name.
    std::string_view name(u32 icon) const { return m_names[icon]; }
    u32 nameHash(u32 icon) const { return m_hashes[icon]; }
    /// Icon index, -1 when unknown.
    i32 find(std::string_view name) const;
    i32 findHash(u32 nameHash) const;
    /// The icon as a retained vector image (stable address for the life of the set).
    const SvgImage& image(u32 icon) const { return m_images[icon]; }
    /// True when some paint has a colour of its own (not currentColor): the icon is not fully tinted by the text colour.
    bool multiColor(u32 icon) const { return m_multiColor[icon] != 0; }
    /// Draws the icon fitted into `box` (aspect kept, centred); currentColor paints take `color`.
    void draw(Painter& p, u32 icon, const Rect& box, Color color) const;

private:
    std::vector<std::string> m_names;
    std::vector<u32> m_hashes;
    std::vector<u32> m_byHash; ///< icon indices sorted by hash
    std::vector<u8> m_multiColor;
    std::vector<SvgImage> m_images;
};

/// One icon for encodeIconPack: `name` is its lookup name ([a-z0-9_.-], unique).
struct IconPackSource {
    std::string name;
    SvgImage image;
};
struct IconPackStats {
    u32 icons = 0;
    u32 multiColor = 0; ///< icons with paints of their own colour
    u32 shapes = 0;
    std::size_t bytes = 0;
};
/// Encodes parsed icons into a .fuseicons pack (the cooker's back end). Icons are sorted by name; an icon whose paints
/// are all currentColor or opaque black is made fully currentColor. Fails on a bad or duplicate name and on an image
/// without shapes. The output is a pure function of the input.
std::expected<std::vector<u8>, std::string> encodeIconPack(std::vector<IconPackSource> icons, IconPackStats* stats = nullptr);

} // namespace fuse::ui
