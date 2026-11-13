// FUSE UI draw list (CPU data, no Vulkan types) and the CPU reference rasteriser. Plan §4.9.
//
// The CPU rasteriser is the specification of every primitive: fuse_ui_vk (UI4) is gated against it with FLIP.
// Output is premultiplied RGBA8 (R in the low byte) in display space, matching frame.ui_composite.
#pragma once

#include "fuse/ui/path.hpp"
#include "fuse/ui/types.hpp"

#include <array>
#include <span>
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
    // ---- vector ops (appended: the values above are the GPU kernel's) ----
    Path,         ///< filled or stroked path: side record paths()[data]; solid `fill` or a gradient; `rect` = bounds
    NinePatch,    ///< sub-rect / 9-slice of image `image` into `rect`: side record patches()[data]; `fill.a` = opacity
    PushClipPath, ///< group: the commands up to the matching PopClip are composited through the path's coverage
    PushLayer,    ///< group: the commands up to the matching PopClip are composited with opacity `fill.a` (`rect` = bounds)
    // ---- effect groups (appended; CPU reference, GPU through the pre-rasterised path like the groups above) ----
    PushTransform, ///< group: its commands live in a local space mapped to the list by transforms()[data] (`rect` = mapped bounds)
    PushFilter,    ///< group: rendered to a layer, then filters()[data .. data + u) are applied in order (`rect` = content bounds)
    PushMask,      ///< group: composited through the alpha (gradient = 0) or luminance (1) of the gradient paints()[data]
};
/// True for the group ops that are rendered to a layer and end at their matching PopClip (not the plain PushClip).
constexpr bool isLayerGroup(DrawOp op) {
    return op == DrawOp::PushClipPath || op == DrawOp::PushLayer || op == DrawOp::PushTransform || op == DrawOp::PushFilter ||
           op == DrawOp::PushMask;
}

/// CSS filter functions (Filter Effects 1). Colour functions work on straight sRGB-encoded values, as CSS does.
enum class FilterKind : u8 { Blur, Brightness, Contrast, Grayscale, Sepia, Saturate, HueRotate, Invert, Opacity, DropShadow };
/// One function of a filter chain. `amount`: Blur / DropShadow = Gaussian standard deviation in list units (the CSS
/// length); HueRotate = radians; the others = the CSS number (1 = 100%).
struct FilterOp {
    FilterKind kind = FilterKind::Blur;
    f32 amount = 0.f;
    Vec2 offset; ///< DropShadow
    Color color; ///< DropShadow
    friend bool operator==(const FilterOp&, const FilterOp&) = default;
};
enum class MaskMode : u8 { Alpha, Luminance };
/// DrawOp::PushTransform side record: `xf` maps the group's local space to the list; `src` bounds its content there.
struct DrawTransform {
    Affine xf;
    Rect src;
};
/// How far (in the units of `ops`) a filter chain can spread a pixel: what a filtered group's bounds grow by.
f32 filterReach(std::span<const FilterOp> ops);

enum class GradientKind : u8 { Linear, Radial, Conic };
enum class GradientSpread : u8 { Pad, Repeat, Reflect };
struct GradientStop {
    f32 offset = 0.f; ///< 0..1
    Color color;
    friend bool operator==(const GradientStop&, const GradientStop&) = default;
};
/// An N-stop gradient. Colours are interpolated premultiplied (CSS / Qt), so a transparent stop does not darken.
///   Linear  t runs 0..1 from `p0` to `p1`
///   Radial  t = distance from the centre `p0` / radius `p1.x` (no focal point)
///   Conic   t runs 0..1 once around the centre `p0`, clockwise on screen, starting at angle `p1.x` (radians, 0 = +x)
/// `stops` is a non-owning view (copied when a command is recorded); a single stop paints its colour. `transform`
/// maps the gradient's geometry into the space it is used in (SVG gradientTransform / objectBoundingBox units): a
/// non-uniform one turns a radial gradient into an elliptical one.
struct Gradient {
    GradientKind kind = GradientKind::Linear;
    GradientSpread spread = GradientSpread::Pad;
    Vec2 p0, p1;
    std::span<const GradientStop> stops;
    Affine transform;
    static Gradient linear(Vec2 from, Vec2 to, std::span<const GradientStop> stops, GradientSpread spread = GradientSpread::Pad) {
        return {GradientKind::Linear, spread, from, to, stops, {}};
    }
    static Gradient radial(Vec2 centre, f32 radius, std::span<const GradientStop> stops, GradientSpread spread = GradientSpread::Pad) {
        return {GradientKind::Radial, spread, centre, {radius, 0.f}, stops, {}};
    }
    static Gradient conic(Vec2 centre, f32 startAngle, std::span<const GradientStop> stops) {
        return {GradientKind::Conic, GradientSpread::Repeat, centre, {startAngle, 0.f}, stops, {}};
    }
};
/// Colour of a stop list at `t` (clamped to the first / last stop; straight-alpha interpolation between stops): what
/// a colour map looks up. For painting use a Gradient, which interpolates premultiplied.
Color gradientStopsAt(std::span<const GradientStop> stops, f32 t);
/// What a path is painted with: a solid colour or a gradient (the gradient is copied; its stops stay a view).
struct Brush {
    Color color;
    Gradient gradient;
    bool hasGradient = false;
    f32 opacity = 1.f; ///< multiplies a gradient's stops (a solid colour carries it in its alpha)
    Brush() = default;
    Brush(Color c) : color(c) {}
    Brush(const Gradient& g) : gradient(g), hasGradient(true) {}
};

/// DrawOp::Path / PushClipPath side record: ranges into DrawList::pathVerbs() / pathPoints() (list coordinates).
struct DrawPath {
    u32 firstVerb = 0, verbCount = 0, firstPoint = 0, pointCount = 0;
    i32 paint = -1; ///< paints()[paint], -1 = the command's solid `fill`
    FillRule rule = FillRule::NonZero;
    bool stroked = false;
    StrokeStyle stroke; ///< when `stroked` (list units)
};
/// A recorded gradient: `toUnit` maps list coordinates to gradient space (linear: t = x; radial: t = |(x, y)|;
/// conic: t = angle of (x, y) / 2 pi). `opacity` multiplies the stops.
struct DrawPaint {
    GradientKind kind = GradientKind::Linear;
    GradientSpread spread = GradientSpread::Pad;
    u32 firstStop = 0, stopCount = 0;
    f32 opacity = 1.f;
    Affine toUnit;
};
/// DrawOp::NinePatch side record. `src` is in image texels (empty = the whole image); the insets cut the source and
/// the destination into 3x3 cells: corners keep their size, edges stretch along one axis, the centre along both.
/// Zero insets draw `src` scaled to the rect.
struct DrawPatch {
    Rect src;
    Edges srcInsets; ///< image texels
    Edges dstInsets; ///< list units
    bool smooth = false; ///< bilinear instead of nearest
};

struct DrawCmd {
    DrawOp op = DrawOp::Rect;
    u8 gradient = 0; ///< 0 none, 1 vertical, 2 horizontal
    /// Glyph: `rect` is already in device pixels (UiContext rasterises glyphs at fontSize x renderScale while it
    /// records the rest of the list in logical units). DrawList::scale leaves such a command alone and clears the flag,
    /// so a finished list never carries it.
    u8 devicePx = 0;
    u16 u = 0, v = 0, tw = 0, th = 0; ///< Glyph: atlas texels
    u32 image = 0;
    u32 data = 0; ///< Path / PushClipPath: index into paths(); NinePatch: index into patches()
    u32 node = 0xFFFFFFFFu; ///< source node index (inspector, damage)
    Rect rect;
    Color fill, fill2, border;
    f32 borderWidth = 0.f;
    std::array<f32, 4> radius{};
    f32 blur = 0.f;
};

class DrawList {
public:
    void clear() {
        m_cmds.clear();
        if (m_vector) clearSide();
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
    void pushClip(const Rect& r, f32 radius = 0.f);
    void popClip(); ///< ends pushClip / pushClipRounded / pushClipPath / pushLayer

    // ---- vector ops: paths, N-stop gradients, 9-slice images, path clips, opacity layers ----
    // Paths are stored unflattened in list coordinates (`xf` is applied while recording) and flattened by the
    // rasteriser in device pixels, so they stay smooth at any render scale. These ops composite as premultiplied
    // RGBA8 texels with the Image blend equation (see CpuRasterizer), which is what lets the GPU packer pre-rasterise
    // them. Nothing here allocates once the list's buffers are warm.
    /// Fills `path` (transformed by `xf`) with a colour or gradient; a gradient's geometry is in the path's space.
    void fillPath(const Path& path, const Brush& brush, FillRule rule = FillRule::NonZero, const Affine& xf = {});
    /// Strokes `path`. Under `xf` the stroke width and dashes scale by xf.meanScale(): exact for similarities
    /// (Painter outlines the stroke in user space first when the transform is not one).
    void strokePath(const Path& path, const StrokeStyle& stroke, const Brush& brush, const Affine& xf = {});
    /// A shape with an outline in one call: fills `path` (non-zero, or even-odd) when `fill` is visible, then strokes
    /// it `strokeWidth` wide, centred on the outline with round joins and caps, when `stroke` is.
    void path(const Path& path, Color fill, bool evenOdd = false, Color stroke = {}, f32 strokeWidth = 0.f);
    /// Rounded rect filled with an N-stop linear / radial / conic gradient (gradient geometry in list coordinates).
    void gradientRect(const Rect& r, const Gradient& g, const std::array<f32, 4>& radii = {});
    /// Image `image` cut 3x3 by `srcInsets` (texels) into `dst`; `dstInsets` default to the source insets. `src`
    /// (texels) selects a sub-image (empty = all). `opacity` multiplies the image.
    void ninePatch(u32 image, const Rect& dst, const Edges& srcInsets, const Rect& src = {}, const Edges* dstInsets = nullptr,
                   f32 opacity = 1.f, bool smooth = false);
    /// Sub-rect `src` (texels) of image `image` scaled into `dst`.
    void imageRect(u32 image, const Rect& dst, const Rect& src, f32 opacity = 1.f, bool smooth = false) {
        ninePatch(image, dst, {}, src, nullptr, opacity, smooth);
    }
    /// Clip to a rounded rect with per-corner radii (tl, tr, br, bl). Equal radii record a plain PushClip; different
    /// ones a path clip group.
    void pushClipRounded(const Rect& r, const std::array<f32, 4>& radii);
    /// Clip group: the commands up to popClip() are rendered to a layer and composited through the path's coverage.
    void pushClipPath(const Path& path, FillRule rule = FillRule::NonZero, const Affine& xf = {});
    /// Opacity group: the commands up to popClip() are rendered to a layer over `bounds` (empty = the current clip)
    /// and composited once with `opacity`, so overlapping children do not show through each other.
    void pushLayer(f32 opacity, const Rect& bounds = {});
    // ---- effect groups (CSS transform / filter / mask). Each ends with popClip(). ----
    /// Transform group: the commands up to popClip() are recorded in a local space and drawn mapped by `xf` (local ->
    /// list). `localBounds` bounds what they draw. A pure translation moves the commands by whole device pixels (exact);
    /// anything else renders them to a layer that is resampled bilinearly, so edges are antialiased and glyphs soften
    /// when scaled up (the layer has the group's resolution). A singular `xf` draws nothing.
    void pushTransform(const Affine& xf, const Rect& localBounds);
    /// Filter group: the commands up to popClip() are rendered to a layer over `bounds` grown by filterReach(), the
    /// functions are applied in order and the result is composited. No functions = a plain group.
    void pushFilter(std::span<const FilterOp> ops, const Rect& bounds);
    /// Mask group: the commands up to popClip() are multiplied by the gradient's alpha (or luminance x alpha).
    void pushMask(const Gradient& g, const Rect& bounds, MaskMode mode = MaskMode::Alpha);
    /// Sets what an already recorded group covers, once its content is known: the bounds of a PushFilter / PushMask /
    /// PushLayer command, the local bounds of a PushTransform (its mapped rect follows).
    void setGroupBounds(std::size_t cmdIndex, const Rect& bounds);
    std::span<const DrawTransform> transforms() const { return m_xforms; }
    std::span<const FilterOp> filters() const { return m_filters; }
    /// The list rect a group command (isLayerGroup) can touch: its `rect`, grown by the filter reach for PushFilter.
    Rect groupBounds(const DrawCmd& c) const;
    std::span<const DrawPath> paths() const { return m_paths; }
    std::span<const PathVerb> pathVerbs() const { return m_verbs; }
    std::span<const Vec2> pathPoints() const { return m_points; }
    std::span<const DrawPaint> paints() const { return m_paints; }
    std::span<const GradientStop> stops() const { return m_stops; }
    std::span<const DrawPatch> patches() const { return m_patches; }
    /// True when the list holds any of the vector ops (hosts / packers skip their handling otherwise).
    bool hasVectorOps() const { return m_vector; }
    const std::vector<DrawCmd>& cmds() const { return m_cmds; }
    std::vector<DrawCmd>& cmds() { return m_cmds; }
    std::size_t size() const { return m_cmds.size(); }
    /// FNV-64 over the commands (golden / parity hashes).
    u64 hash() const;
    /// Moves every command by `d` (Line end points, path points and gradients included): re-bases a subtree's list to
    /// a popup window's origin.
    void translate(Vec2 d);
    /// Logical units -> device pixels (render scale / HiDPI): multiplies geometry, radii, border and line widths and
    /// blur radii by `s` about the origin. Glyph commands flagged `devicePx` are kept (flag cleared); other glyph
    /// commands only move (atlas texels are not resampled). With a fractional `s` the edges of Rect / Image / Backdrop /
    /// PushClip commands are rounded to device pixels (a non-empty rect stays at least 1 px) and border widths to whole
    /// pixels, so fills and hairlines stay sharp; integer scales map pixel edges onto pixel edges and are not rounded.
    /// `s` == 1 changes nothing. Path points, stroke widths, dashes, gradients and 9-slice destination insets scale
    /// too (paths are not snapped: they are re-flattened in device pixels).
    void scale(f32 s);

private:
    void clearSide();
    void translateSide(Vec2 d);
    void scaleSide(f32 s, bool snap);
    u32 addPath(const Path& path, const Affine& xf, Rect& bounds);
    i32 addPaint(const Brush& brush, const Affine& xf);
    std::vector<DrawCmd> m_cmds;
    std::vector<DrawPath> m_paths;
    std::vector<PathVerb> m_verbs;
    std::vector<Vec2> m_points;
    std::vector<DrawPaint> m_paints;
    std::vector<GradientStop> m_stops;
    std::vector<DrawPatch> m_patches;
    std::vector<DrawTransform> m_xforms;
    std::vector<FilterOp> m_filters;
    bool m_vector = false; ///< any Path / NinePatch / PushClipPath / PushLayer command was recorded
};

/// A premultiplied RGBA8 image.
struct Image {
    int width = 0, height = 0;
    std::vector<u32> pixels;
    /// >= 0: on the GPU (fuse_ui_vk) path the image is the external texture bound to this slot
    /// (UiVkRenderer::setExternalImage) and `pixels` are only the CPU rasteriser's fallback / screenshot copy.
    int externalSlot = -1;
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
    /// The registered images, by index (what packDrawList needs for the GPU path).
    const Image* const* images() const { return m_images.data(); }
    u32 imageCount() const { return u32(m_images.size()); }

private:
    const u8* m_atlas = nullptr;
    int m_atlasSize = 0;
    std::vector<const Image*> m_images;
};

// ---- Vector ops: contract shared by the CPU rasteriser and the GPU packer -------------------------------------------
// Path, NinePatch and the groups (PushClipPath, PushLayer, PushTransform, PushFilter, PushMask) are specified as "layer ops": each produces premultiplied RGBA8 source
// texels (paint x coverage quantised to 8 bits; for a group, its children rendered on a transparent layer, times the
// clip path's coverage) which are then composited exactly like an Image texel (times the rounded clip coverage and the
// group opacity). fuse_ui_render therefore draws them with the kernel's existing Image op from texels produced by
// rasterizeCommands(), and both paths agree by construction.

/// What the rasteriser reads besides the list: the glyph atlas and the registered images (non-owning).
struct RasterSources {
    const u8* atlas = nullptr;
    int atlasSize = 0;
    const Image* const* images = nullptr;
    u32 imageCount = 0;
};
/// Rasterises commands [first, first + count) of `list` into `out`, which is resized to the integer device rect
/// `area` and cleared to transparent: the layer those commands produce on their own. `rootWidth` is the width of the
/// final target (path coverage is computed in its coordinates, so a layer and a direct render agree bit for bit).
/// With `groupOpacityOne` a PushLayer at `first` is composited with opacity 1 (its opacity is left to whoever
/// composites `out`).
void rasterizeCommands(const DrawList& list, std::size_t first, std::size_t count, const Rect& area, int rootWidth,
                       const RasterSources& src, Image& out, bool groupOpacityOne = false);
/// Index of the PopClip matching the push command at `i` (PushClip or any isLayerGroup op); size() when unbalanced.
std::size_t matchingPop(const DrawList& list, std::size_t i);

/// Statistics of the path coverage cache (per thread): the coverage masks of small paths are kept by shape, so an
/// icon drawn again at any whole-pixel position is a lookup instead of a rasterisation.
struct PathCacheStats {
    u64 hits = 0, misses = 0, uncached = 0;
    u32 entries = 0;
    std::size_t bytes = 0;
};
PathCacheStats pathCacheStats();
void clearPathCache();

} // namespace fuse::ui
