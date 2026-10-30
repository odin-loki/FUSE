#pragma once
// CSS Masking: clip-path (basic shapes, path(), url(), reference boxes) and mask-* layers (images, gradients,
// mask-mode, mask-composite, size / position / repeat / origin / clip). ComputedStyle keeps each property's source
// text interned (animationNameId); these helpers parse it once (cached by id) and build the DrawList clip data.
#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/style.hpp"

#include <functional>
#include <string>
#include <vector>

namespace fuse::ui {

/// Reference boxes (clip-path <geometry-box>, mask-origin, mask-clip).
enum class CssBox : u8 { Border, Padding, Content, Margin, NoClip };
/// `border` is the node's border box; padding = border inset by border-width, content = border inset by padding,
/// margin = border outset by margin. NoClip returns the border box.
Rect cssBox(const ComputedStyle& s, const Rect& border, CssBox box);

struct ClipPathSpec {
    enum class Kind : u8 { None, Inset, Circle, Ellipse, Polygon, Path, Url, Box };
    Kind kind = Kind::None;
    CssBox box = CssBox::Border;
    bool evenOdd = false;
    /// inset: t r b l radius; circle: r cx cy; ellipse: rx ry cx cy; polygon: x0 y0 x1 y1 ... (any count).
    /// Radii: Auto value 0 = closest-side, -1 = farthest-side.
    std::vector<Length> args;
    std::string path; ///< path(): SVG path data in reference-box coordinates
    std::string url;  ///< url(#id): UiContext::defineClipPath id (without '#')
};
/// Parses a clip-path value (validation for the style parser too).
bool parseClipPathSpec(std::string_view text, ClipPathSpec& out);
/// The parsed spec of an interned clip-path id (ComputedStyle::clipPath); cached. id 0: None.
const ClipPathSpec& clipPathSpec(u32 id);
/// The shape of a basic-shape / path() / box spec over reference box `ref` (empty path for None / Url).
Path clipPathShape(const ClipPathSpec& spec, const Rect& ref);
/// Point-in-shape (hit testing) for the same.
bool clipPathContains(const ClipPathSpec& spec, const Rect& ref, Vec2 q);

/// One mask layer (mask-image item with its longhand values; lists repeat cyclically).
struct MaskLayer {
    enum class Kind : u8 { None, Url, Linear, Radial };
    Kind kind = Kind::None;
    bool repeating = false;
    std::string url;          ///< UiContext::defineMaskImage name
    f32 angle = 180.f;        ///< linear: degrees (0 = to top)
    std::vector<Color> stopColors;
    std::vector<Length> stopPos;  ///< Auto: evenly distributed between neighbours
    bool circle = false;
    u8 extent = 3;            ///< radial size keyword: 0 closest-side, 1 closest-corner, 2 farthest-side, 3 farthest-corner
    Length rx{}, ry{};        ///< radial explicit radii (Auto: by extent)
    Length cx = Length::pct(50.f), cy = Length::pct(50.f);
    u8 mode = 0;              ///< 0 match-source (alpha), 1 alpha, 2 luminance
    u8 composite = 0;         ///< 0 add, 1 subtract, 2 intersect, 3 exclude
    u8 sizeKind = 0;          ///< 0 lengths (auto), 1 cover, 2 contain
    Length sw{}, sh{};
    Length px = Length::pct(0.f), py = Length::pct(0.f);
    u8 repX = 0, repY = 0;    ///< 0 repeat, 1 no-repeat, 2 space, 3 round
    CssBox origin = CssBox::Border, clip = CssBox::Border;
};
/// Validators for the style parser (one per longhand; comma-separated lists).
bool validMaskLonghand(Prop p, std::string_view text);
/// The layers of a computed style (empty: no mask). Cached by the longhand ids.
const std::vector<MaskLayer>& maskLayers(const ComputedStyle& s);
using MaskImageLookup = std::function<const Image*(std::string_view)>;
/// Evaluates all layers into one mask texture over the union of the layers' clip boxes: one texel per pixel up to
/// kMaxMaskTexels per side and kMaxMaskTexelArea in all (larger boxes scale down uniformly, sampled bilinearly).
/// out.gradient = 3, out.path = that box. False when the style has no mask.
inline constexpr int kMaxMaskTexels = 1024;
inline constexpr int kMaxMaskTexelArea = 512 * 512;
bool buildMaskClip(const ComputedStyle& s, const Rect& border, const MaskImageLookup& images, ClipLayer& out);
/// The mask value (0..1) at a point (the same arithmetic the texture samples at texel centres).
f32 maskValueAt(const std::vector<MaskLayer>& layers, const ComputedStyle& s, const Rect& border, const MaskImageLookup& images, Vec2 p);

} // namespace fuse::ui
