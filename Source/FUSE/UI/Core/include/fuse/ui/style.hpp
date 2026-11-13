// FUSE UI style engine: property registry, computed styles, CSS-subset style sheets, cascade. Plan §4.6, decision D4.
//
// Style sheet subset (`.fusestyle`):
//   selectors   type (widget class name), `.class`, `#id`, `*`, pseudo-classes :hover :pressed :focus :focus-visible
//               :disabled :checked :selected, descendant (` `) and child (`>`) combinators, selector lists (`,`)
//   values      px / % / fr / plain numbers, #rgb #rrggbb #rrggbbaa, rgb()/rgba(), keywords, var(--token[, fallback])
//   tokens      custom properties (`--accent: #3af`) declared in `:root` (or any rule matching the node) — theme tokens
//   cascade     specificity (ids, classes + pseudo, types), then source order; inline (node API) values win;
//               inherited properties: color, font-size, text-align, font-weight, font-style, font-family,
//               letter-spacing, line-height, direction
//   text        font-weight (normal | bold | 1..1000), font-style (normal | italic | oblique), font-family (a comma
//               list of names; "sans-serif" / "system-ui" = the context's font, "serif" / "monospace" = a matching
//               installed family), text-decoration (none | underline | line-through, space separated),
//               letter-spacing (px), line-height (normal | <number> x font size | px | %)
//   motion      `transition: <property|all> <duration> [easing] [delay], ...` (animatable properties fade on a state
//               change), `enter-animation` / `exit-animation: none|fade|slide-up|slide-down` + `enter-duration` /
//               `exit-duration` (played when a node first shows / is hidden); all off with UiContext::setReducedMotion
//   direction   `direction: ltr|rtl` (inherited): mirrors Box rows and Anchor left / right, flips text-align start / end;
//               `padding-start` / `padding-end` resolve to the left / right padding by direction; :drag-over state
//   selectors+  attribute selectors `[name]` `[name=v]` `~=` `|=` `^=` `$=` `*=` (` i` flag; UiContext::setAttribute, and
//               `id`), :first-child :last-child :only-child :nth-child(an+b | odd | even) :nth-last-child(), :empty,
//               :not() :is() :where() :has() (relative selectors `:has(> a)`, `:has(+ a)`), :scope, the sibling
//               combinators `+` and `~`, CSS nesting (`&`, implicit descendant)
//   at-rules    @media (width / height ranges, min- / max-, orientation, aspect-ratio, prefers-color-scheme,
//               prefers-contrast, prefers-reduced-motion, hover, pointer, resolution; not / and / or / lists; re-evaluated by
//               StyleEngine::setMediaEnv), @container [name] (width / height / inline-size / block-size / aspect-ratio /
//               orientation of the nearest `container-type` ancestor), @layer (statement, block, anonymous, nested
//               `a.b`; unlayered rules win), @scope (root) [to (limit)], @supports (declarations, selector(), not /
//               and / or), @keyframes
//   animation   `animation: <name> <duration> [easing] [delay] [count | infinite] [direction] [fill-mode] [play-state],
//               ...` and the animation-* longhands run @keyframes on the node (see UiContext, "CSS animations")
//   position    `position: static | relative | absolute | fixed | sticky` with left / top / right / bottom, `z-index`,
//               `aspect-ratio`; `grid-template-columns: subgrid`
//   effects     `transform` (2D functions, `transform-origin`), `filter` (blur, brightness, contrast, grayscale, sepia,
//               saturate, hue-rotate, invert, opacity, drop-shadow), `clip-path` (inset, circle, ellipse, polygon),
//               `mask-image` (linear-gradient / radial-gradient) + `mask-mode`, `background-image: linear-gradient(...)`
// Not supported: !important, cubic-bezier() / steps() easings, 3D transforms, url() masks, scroll / view timelines.
#pragma once

#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/types.hpp"

#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui {

enum class Unit : u8 { Auto, Px, Percent, Fr };
struct Length {
    f32 value = 0.f;
    Unit unit = Unit::Auto;
    static constexpr Length px(f32 v) { return {v, Unit::Px}; }
    static constexpr Length pct(f32 v) { return {v, Unit::Percent}; }
    static constexpr Length fr(f32 v) { return {v, Unit::Fr}; }
    static constexpr Length autoLen() { return {}; }
    bool isAuto() const { return unit == Unit::Auto; }
    /// Resolves against `base` (percent); Auto and Fr resolve to `fallback`.
    f32 resolve(f32 base, f32 fallback) const {
        switch (unit) {
        case Unit::Px: return value;
        case Unit::Percent: return base * value / 100.f;
        default: return fallback;
        }
    }
    friend bool operator==(const Length&, const Length&) = default;
};

enum class LayoutKind : u8 { Box, Grid, Stack, Anchor, Absolute, Flow };
enum class Direction : u8 { Row, Column };
enum class Align : u8 { Auto, Start, Center, End, Stretch };
enum class Justify : u8 { Start, Center, End, SpaceBetween, SpaceAround };
enum class Overflow : u8 { Visible, Clip, Scroll };
enum class GradientDir : u8 { None, Vertical, Horizontal };
enum class TextAlignment : u8 { Start, Center, End };
/// Mouse cursor shape a node asks for (CSS `cursor:`; see UiContext::cursor). Auto = whatever the ancestors ask for.
enum class Cursor : u8 {
    Auto, Arrow, IBeam, PointingHand, ResizeH, ResizeV, ResizeNWSE, ResizeNESW, Move, Crosshair, NotAllowed, Wait, Hidden
};
inline constexpr int kCursorCount = int(Cursor::Hidden) + 1;
/// CSS keyword of a shape ("default", "text", "pointer", "ew-resize", ...).
std::string_view cursorCssName(Cursor c);
/// Easing curves shared by UiContext::animate, style transitions and property animations.
enum class Easing : u8 { Linear, InOutCubic, OutBack, In, Out };
f32 applyEasing(Easing e, f32 k);
/// CSS `position`. Relative: laid out in flow, then moved by its insets. Absolute: out of flow, placed against the
/// padding box of the nearest positioned ancestor (else the viewport). Fixed: out of flow, placed against the viewport,
/// painted over everything and not clipped or scrolled by ancestors. Sticky: in flow, kept inside the scrollport of
/// the nearest scrolling ancestor by its insets while its parent's content box allows.
enum class PositionKind : u8 { Static, Relative, Absolute, Fixed, Sticky };
inline constexpr i32 kZAuto = -2147483647 - 1; ///< z-index: auto
/// CSS `transform` as a 2D matrix (m = a b c d tx ty; x' = a x + c y + tx). Percentages of translate() are kept apart:
/// tx += pct[0] * width + pct[1] * height, ty += pct[2] * width + pct[3] * height (border box of the node).
struct Transform2D {
    std::array<f32, 6> m{1.f, 0.f, 0.f, 1.f, 0.f, 0.f};
    std::array<f32, 4> pct{};
    bool identity() const { return *this == Transform2D{}; }
    friend bool operator==(const Transform2D&, const Transform2D&) = default;
};
inline constexpr int kMaxFilters = 4;
/// CSS `filter`: up to kMaxFilters functions in order; at most one of them a drop-shadow (its offset and colour are
/// `shadowOffset` / `shadowColor`, its blur the function's amount).
struct FilterList {
    struct Fn {
        FilterKind kind = FilterKind::Blur;
        f32 amount = 0.f;
        friend bool operator==(const Fn&, const Fn&) = default;
    };
    u8 count = 0;
    std::array<Fn, kMaxFilters> fn{};
    Vec2 shadowOffset;
    Color shadowColor;
    friend bool operator==(const FilterList&, const FilterList&) = default;
};
enum class ClipShapeKind : u8 { None, Inset, Circle, Ellipse, Polygon };
inline constexpr int kMaxClipPoints = 8;
/// CSS `clip-path` basic shape, relative to the border box. v: Inset = top right bottom left radius; Circle = r cx cy
/// (r < 0: closest-side); Ellipse = rx ry cx cy; Polygon = x0 y0 x1 y1 ... (`count` points). Bit k of `pct` set: v[k]
/// is a percentage (of the width for x values, the height for y values, the box diagonal / sqrt 2 for a circle radius).
struct ClipShape {
    ClipShapeKind kind = ClipShapeKind::None;
    u8 count = 0;
    bool evenOdd = false;
    u16 pct = 0;
    std::array<f32, kMaxClipPoints * 2> v{};
    friend bool operator==(const ClipShape&, const ClipShape&) = default;
};
enum class GradientSpecKind : u8 { None, Linear, Radial };
inline constexpr int kMaxGradientStops = 4;
/// A CSS gradient image (mask-image, background-image): linear at `angle` (radians, CSS: 0 = to top, clockwise) or
/// radial (ellipse, farthest-corner, centred), up to kMaxGradientStops colour stops with offsets 0..1.
struct GradientSpec {
    GradientSpecKind kind = GradientSpecKind::None;
    u8 stopCount = 0;
    f32 angle = 3.14159265f;
    std::array<f32, kMaxGradientStops> offset{};
    std::array<Color, kMaxGradientStops> color{};
    friend bool operator==(const GradientSpec&, const GradientSpec&) = default;
};
/// One entry of the `animation` list of a computed style.
enum class AnimDirection : u8 { Normal, Reverse, Alternate, AlternateReverse };
enum class AnimFill : u8 { None, Forwards, Backwards, Both };
inline constexpr int kMaxAnimations = 4;
struct AnimationSpec {
    u32 name = 0; ///< animationNameId of the @keyframes rule; 0 = none
    f32 duration = 0.f, delay = 0.f;
    f32 iterations = 1.f; ///< infinity = infinite
    Easing ease = Easing::InOutCubic; ///< between each pair of keyframes (a keyframe may override it)
    AnimDirection direction = AnimDirection::Normal;
    AnimFill fill = AnimFill::None;
    bool paused = false;
    friend bool operator==(const AnimationSpec&, const AnimationSpec&) = default;
};
/// Id of an @keyframes / container / layer name as it is stored in values (24 bits: exact in a float; never 0).
constexpr u32 animationNameId(std::string_view name) {
    const u32 h = hashId(name) & 0xFFFFFFu;
    return h ? h : 1u;
}
/// enter-animation / exit-animation kinds.
enum class MotionKind : u8 { None, Fade, SlideUp, SlideDown };

inline constexpr int kMaxGridTracks = 8;
inline constexpr int kMaxFontFamilies = 4; ///< names kept from a font-family list
inline constexpr int kMaxFontVariations = 4; ///< font-variation-settings entries kept
inline constexpr int kMaxFontFeatures = 8;   ///< font-feature-settings entries kept
enum TextDecorationBits : u8 { kTextUnderline = 1, kTextLineThrough = 2 };

enum class Prop : u8;
/// One `transition` entry of a computed style (prop kTransitionAll: every animatable property).
struct Transition {
    Prop prop{};
    Easing ease = Easing::InOutCubic;
    f32 duration = 0.f, delay = 0.f; ///< seconds
    friend bool operator==(const Transition&, const Transition&) = default;
};
inline constexpr int kMaxTransitions = 8;

/// The fully resolved style of a node. Trivially copyable; deduplicated in StyleTable.
struct ComputedStyle {
    // display / layout
    bool visible = true;
    LayoutKind layout = LayoutKind::Box;
    Direction direction = Direction::Column;
    Length width, height, minWidth, minHeight, maxWidth, maxHeight, basis;
    f32 grow = 0.f, shrink = 1.f;
    Edges padding, margin;
    f32 gap = 0.f;
    Align alignItems = Align::Stretch;
    Align alignSelf = Align::Auto;
    Justify justify = Justify::Start;
    bool wrap = false;
    Overflow overflow = Overflow::Visible;
    std::array<Length, kMaxGridTracks> gridColumns{};
    u8 gridColumnCount = 0;
    u8 gridColumn = 0, gridColumnSpan = 1, gridRow = 0, gridRowSpan = 1; ///< 0 = auto placement
    Length left, top, right, bottom; ///< Anchor / Absolute placement
    bool centerX = false, centerY = false;
    // paint
    Color background, background2, borderColor, color = Color::rgb(0xFFFFFF), shadowColor;
    GradientDir gradient = GradientDir::None;
    f32 borderWidth = 0.f;
    std::array<f32, 4> radius{}; ///< tl, tr, br, bl
    f32 fontSize = 14.f;
    TextAlignment textAlign = TextAlignment::Start;
    f32 opacity = 1.f;
    f32 shadowBlur = 0.f, shadowX = 0.f, shadowY = 0.f;
    f32 backdropBlur = 0.f; ///< backdrop-filter: blur(<len>) / backdrop-blur: <len> (frosted glass under the background)
    bool pointerEvents = true;
    // text (inherited, except textDecoration)
    u16 fontWeight = 400;    ///< CSS weight 1..1000 (400 normal, 700 bold)
    bool fontItalic = false;
    u8 textDecoration = 0;   ///< kTextUnderline | kTextLineThrough
    std::array<u32, kMaxFontFamilies> fontFamily{}; ///< fuse::text::familyHash of each listed family; 0 ends the list
    f32 letterSpacing = 0.f; ///< extra pixels after each glyph
    Length lineHeight;       ///< Auto: the font's line height; Px; Percent: of the font size ("1.5" parses as 150%)
    /// Line box height in pixels for the layout (0 = natural).
    f32 lineHeightPx() const { return lineHeight.unit == Unit::Auto ? 0.f : lineHeight.resolve(fontSize, 0.f); }
    Cursor cursor = Cursor::Auto; ///< cursor: auto | default | text | pointer | ew-resize | ... (not inherited; see UiContext::cursor)
    // variable fonts and OpenType features (inherited). They act on a variable face once the application called
    // fuse::text::enableVariableFonts(): font-weight drives wght, font-stretch wdth, font-size opsz, then
    // font-variation-settings sets axes directly. font-feature-settings needs the FULL text profile (HarfBuzz).
    f32 fontStretch = 100.f;   ///< font-stretch percentage (50..200; normal = 100)
    u8 fontVariationCount = 0; ///< font-variation-settings ("wght" 650, "opsz" 12): (tag, value)
    std::array<u32, kMaxFontVariations> fontVariationTags{};
    std::array<f32, kMaxFontVariations> fontVariationValues{};
    u8 fontFeatureCount = 0;   ///< font-feature-settings ("tnum", "liga" 0): (tag, value); 0 off, 1 on, n alternate n
    std::array<u32, kMaxFontFeatures> fontFeatureTags{};
    std::array<u32, kMaxFontFeatures> fontFeatureValues{};
    // icon (not inherited): drawn left of the text by buttons / tool buttons (fuse/ui/icon_set.hpp)
    u32 icon = 0;       ///< `icon: <name>`: hashId of an icon of the context's IconSet or a built-in icon (0 = none)
    f32 iconSize = 0.f; ///< `icon-size`: cell size in px (0 = auto: 16 px, or the size given to widgets::setIcon)
    // direction (inherited) and logical padding (< 0 = not set; resolved into `padding` by the cascade)
    bool rtl = false;
    f32 paddingStart = -1.f, paddingEnd = -1.f;
    // motion
    std::array<Transition, kMaxTransitions> transitions{}; ///< the first `transitionCount` are set
    u8 transitionCount = 0;
    MotionKind enterAnim = MotionKind::None, exitAnim = MotionKind::None;
    f32 enterDuration = 0.15f, exitDuration = 0.12f; ///< seconds
    // positioning
    PositionKind position = PositionKind::Static;
    i32 zIndex = kZAuto;       ///< paint / hit order among siblings (kZAuto = 0, in tree order)
    f32 aspectRatio = 0.f;     ///< width / height; 0 = auto
    bool subgrid = false;      ///< grid-template-columns: subgrid (a Grid child of a Grid uses the parent's column tracks)
    // effects (none inherited)
    Transform2D transform;
    Length originX = Length::pct(50.f), originY = Length::pct(50.f); ///< transform-origin, relative to the border box
    FilterList filter;
    ClipShape clipPath;
    GradientSpec mask;         ///< mask-image
    bool maskLuminance = false;
    GradientSpec backgroundImage; ///< background-image: linear-gradient / radial-gradient (drawn over `background`)
    // CSS animations
    std::array<AnimationSpec, kMaxAnimations> animations{};
    u8 animationCount = 0;
    // container queries
    u8 containerType = 0;      ///< 0 normal, 1 inline-size, 2 size
    u32 containerName = 0;     ///< animationNameId, 0 = none
    /// True when painting this node needs a group of its own (transform, filter, clip-path, mask).
    bool hasEffects() const {
        return !transform.identity() || filter.count || clipPath.kind != ClipShapeKind::None || mask.kind != GradientSpecKind::None;
    }
    friend bool operator==(const ComputedStyle&, const ComputedStyle&) = default;
};

// ---- Property registry ---------------------------------------------------------------------------------------------
enum class Prop : u8 {
    Display, Layout, Direction, Width, Height, MinWidth, MinHeight, MaxWidth, MaxHeight, Basis, Grow, Shrink,
    PaddingLeft, PaddingTop, PaddingRight, PaddingBottom, MarginLeft, MarginTop, MarginRight, MarginBottom, Gap,
    AlignItems, AlignSelf, Justify, Wrap, Overflow, GridColumns, GridColumn, GridColumnSpan, GridRow, GridRowSpan,
    Left, Top, Right, Bottom, CenterX, CenterY, Background, Background2, Gradient, BorderColor, BorderWidth,
    RadiusTL, RadiusTR, RadiusBR, RadiusBL, Color, FontSize, TextAlign, Opacity, ShadowColor, ShadowBlur, ShadowX,
    ShadowY, PointerEvents, BackdropBlur,
    FontWeight, FontStyle, FontFamily, TextDecoration, LetterSpacing, LineHeight, Cursor,
    TextDirection, PaddingStart, PaddingEnd, Transition, EnterAnimation, EnterDuration, ExitAnimation, ExitDuration,
    // style engine port (appended)
    Position, ZIndex, AspectRatio, Transform, TransformOriginX, TransformOriginY, Filter, ClipPath, MaskImage, MaskMode,
    BackgroundImage, AnimationName, AnimationDuration, AnimationTimingFunction, AnimationDelay, AnimationIterationCount,
    AnimationDirection, AnimationFillMode, AnimationPlayState, ContainerType, ContainerName,
    // text / icon port (appended)
    FontStretch, FontVariationSettings, FontFeatureSettings, Icon, IconSize,
    Count
};
inline constexpr int kPropCount = int(Prop::Count);
/// Transition::prop value of `transition: all ...`; PropValue::e of a Prop::Transition value that clears the list.
inline constexpr Prop kTransitionAll = Prop(0xFE);
inline constexpr u8 kTransitionNone = 0xFD;
/// The transition that applies to `p` in `s` (an exact entry wins over `all`); null when none.
const Transition* findTransition(const ComputedStyle& s, Prop p);
/// Writes into `out` the value of `p` interpolated between `a` and `b` (k 0..1, may overshoot). Colours, numbers and
/// px lengths blend; lengths with different units (auto / % / px) and non-animatable properties switch to `b`.
void lerpStyleProp(ComputedStyle& out, const ComputedStyle& a, const ComputedStyle& b, Prop p, f32 k);
/// True when `a` and `b` have the same value for `p`.
bool sameStyleProp(const ComputedStyle& a, const ComputedStyle& b, Prop p);

/// Raw: PropValue::f (property specific). FontSettings: `trackCount` (tag, value) pairs in PropValue::tags / tracks
/// (px) - font-variation-settings, font-feature-settings. Ident: a name stored as its hashId in PropValue::tags[0]
/// (trackCount 1; 0 = none) - icon.
enum class PropType : u8 { Bool, Enum, Length, Number, Color, Tracks, Raw, FontSettings, Ident };
struct PropInfo {
    Prop id;
    std::string_view cssName;
    PropType type;
    bool inherited;
    bool animatable;
    bool affectsLayout;
};
/// The single registry shared by style sheets, inline setters, animation, the inspector and Lua bindings.
const PropInfo& propInfo(Prop p);
/// Looks up a CSS name (longhands only; shorthands are expanded by the parser). False when unknown.
bool findProp(std::string_view cssName, Prop& out);

/// A typed property value.
struct PropValue {
    PropType type = PropType::Number;
    bool b = false;
    u8 e = 0;
    Length len;
    f32 num = 0.f;
    fuse::ui::Color color;
    std::array<Length, kMaxGridTracks> tracks{};
    u8 trackCount = 0;
    /// Raw values (transform: the matrix + pct; filter: kind / amount pairs; clip-path / gradients: their numbers;
    /// animation-* lists: one entry per animation). `fPct`: bit k set = f[k] is a percentage.
    std::array<f32, 16> f{};
    u8 fCount = 0;
    u16 fPct = 0;
    std::array<u32, kMaxGridTracks> tags{}; ///< FontSettings: the OpenType tag of each entry; Ident: tags[0] = hashId
    /// An Ident value (0 = none).
    static PropValue ofIdent(u32 hash) { PropValue p; p.type = PropType::Ident; p.trackCount = 1; p.tags[0] = hash; return p; }
    u32 ident() const { return trackCount ? tags[0] : 0u; }
    static PropValue ofBool(bool v) { PropValue p; p.type = PropType::Bool; p.b = v; return p; }
    static PropValue ofEnum(u8 v) { PropValue p; p.type = PropType::Enum; p.e = v; return p; }
    static PropValue ofLength(Length v) { PropValue p; p.type = PropType::Length; p.len = v; return p; }
    static PropValue ofNumber(f32 v) { PropValue p; p.type = PropType::Number; p.num = v; return p; }
    static PropValue ofColor(fuse::ui::Color v) { PropValue p; p.type = PropType::Color; p.color = v; return p; }
    /// A Prop::Transition value: `target` animates over `seconds` (kTransitionAll for every animatable property).
    static PropValue ofTransition(Prop target, f32 seconds, Easing ease = Easing::InOutCubic, f32 delay = 0.f) {
        PropValue p; p.type = PropType::Number; p.e = u8(target); p.num = seconds; p.len = Length::px(delay); p.trackCount = u8(ease); return p;
    }
};
/// A Prop::Transform value.
PropValue transformValue(const Transform2D& t);
/// The node's transform about its transform-origin for the border box `box` (list / layout coordinates).
Affine styleTransform(const ComputedStyle& s, const Rect& box);
/// The clip-path shape of `s` for the border box `box` as a path (appended); false when there is none.
bool clipShapePath(const ClipShape& shape, const Rect& box, Path& out);
/// True when `p` is inside the clip-path shape (true when there is none).
bool clipShapeContains(const ClipShape& shape, const Rect& box, Vec2 p);
/// The gradient of `g` over `box` with its stops written to `stops` (kMaxGradientStops entries).
Gradient gradientOf(const GradientSpec& g, const Rect& box, GradientStop* stops);
/// Straight colour of the gradient at `p` (what a mask multiplies by: its alpha, or its luminance x alpha).
Color gradientColorAt(const GradientSpec& g, const Rect& box, Vec2 p);
void applyProp(ComputedStyle& s, Prop p, const PropValue& v);
PropValue readProp(const ComputedStyle& s, Prop p);
/// Copies inherited properties from parent into s.
void inheritFrom(ComputedStyle& s, const ComputedStyle& parent);

// ---- State bits used by pseudo-classes -----------------------------------------------------------------------------
enum StateBits : u32 {
    kStateHover = 1u << 0,
    kStatePressed = 1u << 1,
    kStateFocus = 1u << 2,
    kStateFocusVisible = 1u << 3,
    kStateDisabled = 1u << 4,
    kStateChecked = 1u << 5,
    kStateSelected = 1u << 6,
    kStateDragOver = 1u << 7, ///< :drag-over - the current drop target of a drag (UiContext::beginDrag / dropExternal)
};

// ---- Style sheets --------------------------------------------------------------------------------------------------
enum class StyleError : u8 { Syntax, UnknownProperty, BadValue };
struct StyleDiagnostic {
    StyleError error;
    u32 line;
    std::string message;
};

/// A compound selector. `firstPart` / `partCount`: its attribute / structural / functional conditions in
/// StyleRule::parts.
struct SimpleSelector {
    u32 typeHash = 0; ///< 0 = any
    u32 idHash = 0;
    std::array<u32, 4> classes{};
    u8 classCount = 0;
    u32 states = 0;
    enum class Combinator : u8 { None, Descendant, Child, NextSibling, LaterSibling } combinator = Combinator::None; ///< to the previous (left)
    u16 firstPart = 0, partCount = 0;
};
enum class AttrOp : u8 { Exists, Equals, Includes, DashMatch, Prefix, Suffix, Substring };
/// A condition of a compound beyond type / id / class / state.
struct SelectorPart {
    enum class Kind : u8 { Attr, NthChild, NthLastChild, Empty, Not, Is, Where, Has, Scope, Root };
    Kind kind = Kind::Attr;
    AttrOp attrOp = AttrOp::Exists;
    bool noCase = false;
    i32 a = 0, b = 0;                 ///< Nth*: matches index a n + b (1-based)
    u32 name = 0;                     ///< Attr: hashId of the attribute name
    u32 text = 0, textLen = 0;        ///< Attr: the value, a range of StyleRule::strings
    u32 firstList = 0, listCount = 0; ///< Not / Is / Where / Has: the argument selectors, a range of StyleRule::lists
};
/// A complex selector stored out of line: compounds [first, first + count) of StyleRule::sub. `lead` is the
/// combinator in front of a relative selector (`:has(> a)`); Descendant when none is written.
struct SelectorRange {
    u32 first = 0, count = 0;
    SimpleSelector::Combinator lead = SimpleSelector::Combinator::Descendant;
};

/// One term of a media / container condition in postfix order.
struct QueryTerm {
    enum class Op : u8 { Feature, And, Or, Not, True, False };
    enum class Feature : u8 { Width, Height, AspectRatio, Orientation, PrefersDark, PrefersReducedMotion, Hover, Pointer, Resolution, PrefersContrast };
    enum class Cmp : u8 { Truthy, Eq, Lt, Le, Gt, Ge };
    Op op = Op::True;
    Feature feature = Feature::Width;
    Cmp cmp = Cmp::Truthy;
    f32 value = 0.f; ///< px; a ratio; dppx; Orientation: 0 portrait, 1 landscape; Pointer: 0 none, 1 coarse, 2 fine
};
/// An @media or @container block; `parent` chains to the block it is nested in (-1 = none).
struct StyleCondition {
    enum class Kind : u8 { Media, Container };
    Kind kind = Kind::Media;
    i32 parent = -1;
    u32 containerName = 0; ///< Container: animationNameId, 0 = the nearest container
    bool needsHeight = false; ///< Container: the query reads the block size
    std::vector<QueryTerm> program;
};
/// What @media queries are evaluated against (StyleEngine::setMediaEnv; UiContext keeps it up to date).
struct MediaEnv {
    f32 width = 800.f, height = 600.f; ///< viewport, logical px
    bool dark = false;                 ///< prefers-color-scheme: dark
    bool reducedMotion = false;        ///< prefers-reduced-motion: reduce
    bool hover = true;                 ///< the primary pointer can hover
    u8 pointer = 2;                    ///< 0 none, 1 coarse, 2 fine
    f32 resolution = 1.f;              ///< dppx (render scale)
    bool moreContrast = false;         ///< prefers-contrast: more (a high-contrast theme is in use)
    friend bool operator==(const MediaEnv&, const MediaEnv&) = default;
};
/// True when `program` holds for the values in `env` (container queries: width / height = the container's content box).
bool evalQuery(const std::vector<QueryTerm>& program, const MediaEnv& env);

struct Declaration {
    Prop prop = Prop::Count;   ///< Count => custom property definition
    u32 customHash = 0;        ///< custom property name (definition) or var() reference
    PropValue value;
    std::string raw;           ///< unresolved text when it references var(); custom property text
};

struct StyleRule {
    std::vector<SimpleSelector> chain; ///< left to right
    u32 specificity = 0;
    u32 order = 0;
    std::vector<Declaration> decls;
    bool root = false; ///< :root
    // extended selectors (empty for plain rules)
    std::vector<SelectorPart> parts;
    std::vector<SimpleSelector> sub;  ///< compounds of the argument selectors
    std::vector<SelectorRange> lists; ///< argument selectors
    std::string strings;              ///< attribute values
    i32 cond = -1;                    ///< StyleSheet::conditions() index of the innermost @media / @container, -1 = none
    u16 layer = 0;                    ///< StyleSheet::layers() index + 1; 0 = not in a layer
    bool scoped = false;              ///< inside @scope: lists [scopeFirst, +scopeCount) = roots, [limitFirst, +limitCount) = limits
    u32 scopeFirst = 0, scopeCount = 0, limitFirst = 0, limitCount = 0;
};

/// One keyframe of an @keyframes rule.
struct KeyframeBlock {
    f32 offset = 0.f; ///< 0..1
    bool hasEase = false;
    Easing ease = Easing::Linear; ///< animation-timing-function in the block: the curve towards the next keyframe
    std::vector<Declaration> decls;
};
struct KeyframesRule {
    u32 name = 0; ///< animationNameId
    std::vector<KeyframeBlock> frames; ///< ascending offsets
};

/// What the matcher needs to know about a node and its ancestors.
struct StyleSubject {
    virtual ~StyleSubject() = default;
    virtual u32 typeHash(u32 node) const = 0;
    virtual u32 idHash(u32 node) const = 0;
    virtual bool hasClass(u32 node, u32 classHash) const = 0;
    virtual u32 states(u32 node) const = 0;
    virtual u32 parent(u32 node) const = 0; ///< 0xFFFFFFFF at the root
    // tree order, content and attributes (structural selectors, sibling combinators, :has, attribute selectors)
    virtual u32 prevSibling(u32) const { return 0xFFFFFFFFu; }
    virtual u32 nextSibling(u32) const { return 0xFFFFFFFFu; }
    virtual u32 firstChild(u32) const { return 0xFFFFFFFFu; }
    virtual bool empty(u32 node) const { return firstChild(node) == 0xFFFFFFFFu; } ///< no children and no text
    virtual bool attribute(u32 /*node*/, u32 /*nameHash*/, std::string_view& /*out*/) const { return false; }
    /// Content-box size of the nearest ancestor that is a query container (named `name` when non-zero; one that
    /// answers the block size when `needHeight`). False when there is none.
    virtual bool containerSize(u32 /*node*/, u32 /*name*/, bool /*needHeight*/, Size& /*out*/) const { return false; }
};

/// What else has to be restyled when a class / state / attribute of a node changes (StyleEngine::invalidation).
struct StyleInvalidation {
    u8 descend = 0;        ///< levels of descendants a selector can reach from the changed node (255 = any depth)
    bool siblings = false; ///< later siblings (and `descend` levels below them)
    bool has = false;      ///< :has() anchors: ancestors and the earlier siblings of the node and of its ancestors
};
enum StyleFeature : u32 {
    kStyleSibling = 1u << 0,     ///< a `+` / `~` combinator
    kStyleStructural = 1u << 1,  ///< :nth-child family / :empty
    kStyleHas = 1u << 2,         ///< :has()
    kStyleHasDeep = 1u << 3,     ///< :has() on a compound that is not the subject (a change restyles the whole tree)
    kStyleStructDeep = 1u << 4,  ///< a structural pseudo-class / sibling combinator left of the subject
    kStyleDescend = 1u << 5,     ///< a selector with more than one compound
    kStyleContainer = 1u << 6,   ///< an @container rule
    kStyleEmptyText = 1u << 7,   ///< :empty
};

class StyleSheet {
public:
    /// Parses CSS-subset text. Unknown properties and bad values are skipped and reported in `diagnostics`;
    /// unrecoverable syntax errors fail.
    static std::expected<StyleSheet, StyleDiagnostic> parse(std::string_view css,
                                                            std::vector<StyleDiagnostic>* diagnostics = nullptr);
    const std::vector<StyleRule>& rules() const { return m_rules; }
    /// Theme tokens (from :root): the last definition wins. With `condOk` (per condition of this sheet: its @media
    /// chain holds; StyleEngine keeps it) a token defined inside an @media block that does not hold is skipped, so
    /// `@media (prefers-color-scheme: light) { :root { --accent: ... } }` re-colours only while it applies.
    bool token(u32 nameHash, std::string_view& out, const std::vector<u8>* condOk = nullptr) const;
    const std::vector<std::pair<u32, std::string>>& tokens() const { return m_tokens; }
    /// Per token: the condition (index into conditions()) of the block it was defined in, -1 for none. May be shorter
    /// than tokens() (the rest: -1).
    const std::vector<i32>& tokenConds() const { return m_tokenConds; }
    /// @media / @container blocks (StyleRule::cond), @layer names in order of first appearance ("a", "a.b"; anonymous
    /// layers get a private name), @keyframes rules.
    const std::vector<StyleCondition>& conditions() const { return m_conditions; }
    const std::vector<std::string>& layers() const { return m_layers; }
    const std::vector<KeyframesRule>& keyframes() const { return m_keyframes; }
    /// Rebuilds a parsed sheet from its parts (cooked `.fusestyle` loading; see fuse/ui/doc/cook.hpp).
    static StyleSheet fromParts(std::vector<StyleRule> rules, std::vector<std::pair<u32, std::string>> tokens,
                                std::vector<StyleCondition> conditions = {}, std::vector<std::string> layers = {},
                                std::vector<KeyframesRule> keyframes = {}, std::vector<i32> tokenConds = {}) {
        StyleSheet s;
        s.m_tokenConds = std::move(tokenConds);
        s.m_rules = std::move(rules);
        s.m_tokens = std::move(tokens);
        s.m_conditions = std::move(conditions);
        s.m_layers = std::move(layers);
        s.m_keyframes = std::move(keyframes);
        return s;
    }

private:
    friend class StyleEngine;
    friend struct StyleParser;
    std::vector<StyleRule> m_rules;
    std::vector<std::pair<u32, std::string>> m_tokens;
    std::vector<i32> m_tokenConds;
    u32 m_handle = 0; ///< StyleEngine::addSheetTracked identity (0: added with addSheet)
    std::vector<StyleCondition> m_conditions;
    std::vector<std::string> m_layers;
    std::vector<KeyframesRule> m_keyframes;
};
/// Parses the body of an @keyframes rule (`from { ... } 50% { ... } to { ... }`): what UiContext::animateCss takes.
bool parseKeyframes(std::string_view body, KeyframesRule& out, std::vector<StyleDiagnostic>* diagnostics = nullptr);

/// Parses one value for `p` (also used by inline setters and markup). False on a bad value.
bool parsePropValue(Prop p, std::string_view text, PropValue& out);
bool parseColor(std::string_view text, Color& out);

/// Ordered sheets + deduplicated computed-style table + matcher.
class StyleEngine {
public:
    StyleEngine();
    void clearSheets();
    void addSheet(StyleSheet sheet);
    u32 sheetVersion() const { return m_version; }
    /// @media environment. A change that flips the result of any @media block re-sorts the rules and bumps
    /// sheetVersion() (every node is restyled, without transitions); other changes cost nothing.
    void setMediaEnv(const MediaEnv& env);
    const MediaEnv& mediaEnv() const { return m_media; }
    /// kStyle* bits of the loaded sheets: which tree changes need more than restyling the changed node.
    u32 features() const { return m_features; }
    bool usesContainerQueries() const { return (m_features & kStyleContainer) != 0; }
    /// What a change of class `key` (hashId), state bits `key`, or attribute `key` (hashId of its name) reaches.
    StyleInvalidation classInvalidation(u32 classHash) const { return lookupInvalidation(0, classHash); }
    StyleInvalidation stateInvalidation(u32 stateBits) const;
    StyleInvalidation attributeInvalidation(u32 nameHash) const { return lookupInvalidation(2, nameHash); }
    /// The @keyframes rule called `name` (animationNameId); the last sheet that defines it wins. Null when unknown.
    const KeyframesRule* findKeyframes(u32 name) const;
    /// Resolves the value of a keyframe / rule declaration (var() against the sheets' :root tokens). False when bad.
    bool resolveDeclaration(const Declaration& d, PropValue& out);
    /// Computes the style of `node`: defaults → inherited from `parentStyle` → matched rules → `inlineValues`.
    /// Returns an index into the table (allocates only for a never-seen combination).
    u32 compute(const StyleSubject& subject, u32 node, const ComputedStyle* parentStyle,
                const ComputedStyle* defaults, const std::vector<std::pair<Prop, PropValue>>& inlineValues);
    const ComputedStyle& style(u32 index) const { return m_table[index]; }
    /// Scratch (non-interned, mutable) table entries for nodes whose style is being animated: the node points at its
    /// own slot while a transition / property animation runs, so intermediate values never grow the interned table.
    /// acquireScratch may reallocate the table (invalidates style() references).
    u32 acquireScratch(const ComputedStyle& init);
    void releaseScratch(u32 index);
    ComputedStyle& scratch(u32 index) { return m_table[index]; }
    u32 tableSize() const { return u32(m_table.size()); }
    bool matches(const StyleSubject& subject, u32 node, const StyleRule& rule) const;

    // ---- sheets by handle + theme token colours (Core/src/style_sheets.cpp) ----------------------------------------
    /// addSheet that returns a handle (never 0) for replaceSheet / removeSheet. Handles die with clearSheets.
    u32 addSheetTracked(StyleSheet sheet);
    /// Swaps the sheet `handle` names for `sheet`, keeping its place in the cascade order. False: unknown handle.
    bool replaceSheet(u32 handle, StyleSheet sheet);
    /// Drops the sheet `handle` names. False: unknown handle.
    bool removeSheet(u32 handle);
    bool hasSheet(u32 handle) const;
    u32 sheetCount() const { return u32(m_sheets.size()); }
    const StyleSheet& sheet(u32 index) const { return m_sheets[index]; }
    /// The `:root` token `nameHash` (hashId("--name")) as a colour: the last sheet that defines it wins, `var(--other)`
    /// chains are followed. False when no sheet defines it or it is not a colour. Cached per sheetVersion() (no
    /// allocation), so widget paint code may call it every frame.
    bool tokenColor(u32 nameHash, Color& out) const;
    /// True when tokenColor(nameHash) is answered from the cache (it was asked for since the last sheet change and
    /// found a slot). For gates: a theme whose tokens do not all fit would resolve the rest on every paint.
    bool tokenColorCached(u32 nameHash) const;

private:
    struct TokenColorSlot {
        u32 hash = 0;
        u8 state = 0; ///< 0 empty, 1 colour, 2 not a colour / undefined
        Color color;
    };
    /// Open addressing, at most 8 probes: a token that finds no slot is resolved on every call. Sized for the widget
    /// theme's token table (90 names; with 128 slots four of them never found a slot) plus an application's own.
    mutable std::array<TokenColorSlot, 512> m_tokenColors{};
    mutable u32 m_tokenColorVersion = 0xFFFFFFFFu;
    u32 m_nextSheetHandle = 0;
    struct Ordered {
        const StyleRule* rule;
        const StyleSheet* sheet;
    };
    struct InvalKey {
        u32 key;
        u8 kind; ///< 0 class, 1 state bit, 2 attribute
        StyleInvalidation inv;
    };
    struct MatchCtx;
    bool matchSimple(const StyleSubject& s, u32 node, const SimpleSelector& sel) const;
    bool matchCompound(const MatchCtx& c, u32 node, const SimpleSelector& sel) const;
    bool matchChain(const MatchCtx& c, u32 node, const SimpleSelector* chain, int k, u32 anchor, SimpleSelector::Combinator lead) const;
    bool matchList(const MatchCtx& c, u32 node, u32 firstList, u32 listCount) const;
    bool matchHas(const MatchCtx& c, u32 anchor, const SelectorRange& range) const;
    bool conditionHolds(const StyleSubject& s, u32 node, const Ordered& o) const;
    void rebuild();
    void collectKeys(const StyleRule& r, const SimpleSelector& sel, StyleInvalidation inv, bool inHas);
    void addKey(u8 kind, u32 key, StyleInvalidation inv);
    StyleInvalidation lookupInvalidation(u8 kind, u32 key) const;
    MediaEnv m_media;
    u32 m_features = 0;
    std::vector<InvalKey> m_keys;
    std::vector<std::vector<u8>> m_condOk; ///< per sheet, per condition: its @media chain holds
    bool resolveVar(std::string_view raw, Prop p, PropValue& out, int depth) const;
    /// The token `h` from the sheets: the last sheet that defines it under conditions that hold now.
    bool sheetToken(u32 h, std::string_view& out) const;
    u32 intern(const ComputedStyle& cs);
    void rehash(std::size_t slots);
    std::vector<u8> m_isScratch;   ///< parallel to m_table
    std::vector<u32> m_freeScratch;
    u32 m_interned = 0;
    std::vector<StyleSheet> m_sheets;
    std::vector<Ordered> m_ordered; ///< rules whose @media chain holds, sorted by (layer, specificity, sheet, order)
    std::vector<const StyleRule*> m_scratch;
    std::vector<std::pair<u32, std::string_view>> m_localTokens;
    std::vector<ComputedStyle> m_table;
    std::vector<u32> m_hashIndex; ///< open addressing into m_table (+1)
    u32 m_version = 0;
};

} // namespace fuse::ui
