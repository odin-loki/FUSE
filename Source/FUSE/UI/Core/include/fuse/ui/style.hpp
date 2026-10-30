// FUSE UI style engine: property registry, computed styles, CSS-subset style sheets, cascade. Plan §4.6, decision D4.
//
// Style sheet subset (`.fusestyle`):
//   selectors   type (widget class name), `.class`, `#id`, `*`, pseudo-classes :hover :pressed :focus :focus-visible
//               :disabled :checked :selected, descendant (` `) and child (`>`) combinators, selector lists (`,`)
//   values      px / % / fr / plain numbers, #rgb #rrggbb #rrggbbaa, rgb()/rgba(), keywords, var(--token[, fallback])
//   tokens      custom properties (`--accent: #3af`) declared in `:root` (or any rule matching the node) — theme tokens
//   cascade     specificity (ids, classes + pseudo, types), then source order; inline (node API) values win;
//               inherited properties: color, font-size, text-align
//               attribute selectors [a] [a=v] [a~=v] [a|=v] [a^=v] [a$=v] [a*=v] (` i` / ` s` flags; UiContext::setAttribute),
//               :first-child :last-child :only-child :nth-child() :nth-last-child() and the -of-type family (An+B,
//               odd, even), sibling combinators `+` and `~`
//   transitions `transition` and transition-property / -duration / -timing-function / -delay (CSS Transitions 1):
//               animatable lengths, numbers and colours tween from the shown value when the computed value changes;
//               reversing shortening; transitionrun / start / end / cancel signals (UiContext)
//   animations  `@keyframes name { from / to / <pct>[, ...] { decls; animation-timing-function } }` and `animation`
//               with all longhands: name, duration, timing-function, delay, iteration-count, direction, fill-mode,
//               play-state (CSS Animations 1); animationstart / iteration / end / cancel signals;
//               animation-composition: replace | add | accumulate (CSS Animations 2)
//   transform   translate() translateX() translateY() scale() scaleX() scaleY() (paint-only, about the box centre);
//               animatable: matching lists per function, otherwise the composed maps
//   discrete    transition-behavior: normal | allow-discrete — discrete properties (display, enums, ...) flip at the
//               midpoint; display keeps the shown value for the whole transition (CSS Transitions 2)
//   logical     :not() :is() :where() (forgiving, zero specificity) :has() (relative: `:has(> a)`, `:has(+ a)`,
//               `:has(~ a)`, `:has(a b)`), :empty; `[class~=x]` and every attribute operator also see the class list
//   scripted    UiContext::animate (Web Animations-style keyframe effects with play / pause / reverse / finish /
//               cancel / currentTime / playbackRate / commitStyles)
#pragma once

#include "fuse/ui/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
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
/// CSS filter functions (backdrop-filter), in list order.
/// DropShadow: the one shadow of the list (the last one wins); Url: url(#id), amount = filterNameId(id) bit-cast,
/// resolved at paint time by UiContext::defineFilter.
/// CSS transform functions (2D and 3D), in list order; arguments: translate px, scale factor, rotate / skew degrees,
/// perspective px (0: none). matrix(a, b, c, d, e, f) is the six consecutive entries MatA..MatF; skew(ax, ay) with
/// ay != 0 is stored as matrix. rotate3d(x, y, z, a) is R3X R3Y R3Z R3A; matrix3d() is the sixteen entries M3_0..M3_15
/// (column-major, as written). Each multi-entry function applies at its last entry.
enum class TransformFn : u8 {
    TranslateX, TranslateY, ScaleX, ScaleY, Rotate, SkewX, SkewY, MatA, MatB, MatC, MatD, MatE, MatF,
    TranslateZ, ScaleZ, RotateX, RotateY, R3X, R3Y, R3Z, R3A, Perspective,
    M3_0, M3_1, M3_2, M3_3, M3_4, M3_5, M3_6, M3_7, M3_8, M3_9, M3_10, M3_11, M3_12, M3_13, M3_14, M3_15
};
inline constexpr int kMaxTransformArgs = 32; ///< entries of a transform list (matrix3d() takes 16)
/// A 2D affine map: x' = a x + c y + e, y' = b x + d y + f (CSS matrix(a, b, c, d, e, f)).
struct Affine2 {
    f32 a = 1.f, b = 0.f, c = 0.f, d = 1.f, e = 0.f, f = 0.f;
    bool identity() const { return a == 1.f && b == 0.f && c == 0.f && d == 1.f && e == 0.f && f == 0.f; }
    bool axisAligned() const { return b == 0.f && c == 0.f; }
    bool translationOnly() const { return a == 1.f && b == 0.f && c == 0.f && d == 1.f; }
    f32 det() const { return a * d - b * c; }
    /// this * n: n applies first.
    Affine2 operator*(const Affine2& n) const {
        return {a * n.a + c * n.b, b * n.a + d * n.b, a * n.c + c * n.d, b * n.c + d * n.d, a * n.e + c * n.f + e, b * n.e + d * n.f + f};
    }
    Vec2 apply(Vec2 p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    /// false when singular (CSS: the element is not rendered and not hit)
    bool inverse(Affine2& out) const {
        const double dt = double(a) * double(d) - double(b) * double(c);
        if (!(std::fabs(dt) > 1e-12)) return false;
        const double i0 = double(d) / dt, i1 = -double(b) / dt, i2 = -double(c) / dt, i3 = double(a) / dt;
        out = {f32(i0), f32(i1), f32(i2), f32(i3), f32(-(i0 * double(e) + i2 * double(f))), f32(-(i1 * double(e) + i3 * double(f)))};
        return true;
    }
    /// Axis-aligned bounds of r under the map.
    Rect mapRect(const Rect& r) const {
        const Vec2 p[4] = {apply({r.x, r.y}), apply({r.right(), r.y}), apply({r.x, r.bottom()}), apply({r.right(), r.bottom()})};
        f32 x0 = p[0].x, y0 = p[0].y, x1 = p[0].x, y1 = p[0].y;
        for (const Vec2& q : p) x0 = std::min(x0, q.x), y0 = std::min(y0, q.y), x1 = std::max(x1, q.x), y1 = std::max(y1, q.y);
        return {x0, y0, x1 - x0, y1 - y0};
    }
    std::array<f32, 6> array() const { return {a, b, c, d, e, f}; }
};
/// The 2D decomposition CSS Transforms 1 interpolates mismatched lists with: M = translate(tx, ty) rotate(angle)
/// skewX(atan(shear)) scale(sx, sy) (angles in degrees).
struct Decomposed2 {
    f32 tx = 0.f, ty = 0.f, angle = 0.f, shear = 0.f, sx = 1.f, sy = 1.f;
};
Decomposed2 decompose(const Affine2& m);
Affine2 recompose(const Decomposed2& d);
/// A 3D homogeneous map (CSS matrix3d order: column-major, m[col * 4 + row]); points are column vectors.
struct Mat4 {
    std::array<f32, 16> m{1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
    f32 at(int row, int col) const { return m[std::size_t(col * 4 + row)]; }
    f32& at(int row, int col) { return m[std::size_t(col * 4 + row)]; }
    bool identity() const { return *this == Mat4{}; }
    /// a 2D affine map (no z, no perspective): CSS "is2D"
    bool is2D() const {
        return at(2, 0) == 0.f && at(2, 1) == 0.f && at(0, 2) == 0.f && at(1, 2) == 0.f && at(2, 2) == 1.f && at(3, 2) == 0.f &&
               at(2, 3) == 0.f && at(3, 0) == 0.f && at(3, 1) == 0.f && at(3, 3) == 1.f;
    }
    static Mat4 from(const Affine2& a) {
        Mat4 r;
        r.at(0, 0) = a.a, r.at(1, 0) = a.b, r.at(0, 1) = a.c, r.at(1, 1) = a.d, r.at(0, 3) = a.e, r.at(1, 3) = a.f;
        return r;
    }
    static Mat4 translate(f32 x, f32 y, f32 z) { Mat4 r; r.at(0, 3) = x, r.at(1, 3) = y, r.at(2, 3) = z; return r; }
    Affine2 affine() const { return {at(0, 0), at(1, 0), at(0, 1), at(1, 1), at(0, 3), at(1, 3)}; }
    /// this * n: n applies first.
    Mat4 operator*(const Mat4& n) const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                f32 v = 0.f;
                for (int k = 0; k < 4; ++k) v += at(i, k) * n.at(k, j);
                r.at(i, j) = v;
            }
        return r;
    }
    /// The plane z = 0 to the screen: rows / columns (x, y, w) of the matrix (row-major 3x3).
    std::array<f32, 9> flatten() const {
        return {at(0, 0), at(0, 1), at(0, 3), at(1, 0), at(1, 1), at(1, 3), at(3, 0), at(3, 1), at(3, 3)};
    }
    bool inverse(Mat4& out) const;
    friend bool operator==(const Mat4&, const Mat4&) = default;
};
/// 3x3 homography helpers (row-major): (X, Y, W) = H (x, y, 1), point = (X / W, Y / W).
bool invert3(const std::array<f32, 9>& h, std::array<f32, 9>& out);
/// false when the point is behind the viewer (W <= 0)
bool mapHomography(const std::array<f32, 9>& h, Vec2 p, Vec2& out);
/// Bounds of r under h (the part in front of the viewer); empty when none is.
Rect mapRectHomography(const std::array<f32, 9>& h, const Rect& r);
/// CSS Transforms 2 3D decomposition (translate, scale, skew xy / xz / yz, perspective, quaternion).
struct Decomposed3 {
    std::array<f32, 3> t{}, s{1.f, 1.f, 1.f}, k{}; ///< k: skew xy, xz, yz
    std::array<f32, 4> p{0.f, 0.f, 0.f, 1.f}, q{0.f, 0.f, 0.f, 1.f};
};
bool decompose3(const Mat4& m, Decomposed3& out);
Mat4 recompose3(const Decomposed3& d);
enum class FilterFn : u8 { Blur, Brightness, Contrast, Saturate, Grayscale, HueRotate, Invert, Sepia, Opacity, DropShadow, Url };
/// drop-shadow(<dx> <dy> [<blur>] [<color>]); color.a == 0: none
struct FilterShadow {
    f32 x = 0.f, y = 0.f, blur = 0.f;
    fuse::ui::Color color{};
    friend bool operator==(const FilterShadow&, const FilterShadow&) = default;
};
/// Interned url(#id) names of filter lists (stable for the process).
u32 filterNameId(std::string_view id);
std::string_view filterName(u32 id);
/// CSS background-clip: the box the background (and backdrop blur) is painted in; the border stays on the border box.
enum class BackgroundClip : u8 { BorderBox, PaddingBox, ContentBox, Text }; ///< Text: inside the node's glyph outlines
enum class GradientDir : u8 { None, Vertical, Horizontal, Angle }; ///< Angle: CSS multi-stop linear-gradient
enum class TextAlignment : u8 { Start, Center, End };

inline constexpr int kMaxGridTracks = 8;
inline constexpr int kMaxFilterShadows = 4; ///< drop-shadow() entries per filter list
inline constexpr int kMaxTransitions = 8;   ///< entries of a transition-* / animation-* list

/// A CSS easing function (cubic-bezier() or steps()); default `ease`.
struct Easing {
    enum class Kind : u8 { Bezier, Steps };
    enum class StepPos : u8 { JumpStart, JumpEnd, JumpNone, JumpBoth };
    Kind kind = Kind::Bezier;
    f32 x1 = 0.25f, y1 = 0.1f, x2 = 0.25f, y2 = 1.f;
    u8 steps = 1;
    StepPos stepPos = StepPos::JumpEnd;
    friend bool operator==(const Easing&, const Easing&) = default;
};
/// The eased output progress at input progress t in [0, 1] (CSS Easing 1).
f32 easeValue(const Easing& e, f32 t);
bool parseEasing(std::string_view text, Easing& out);
/// A set of properties (bit per Prop).
struct PropMask {
    std::array<u64, 2> w{};
    bool has(u32 p) const { return (w[p >> 6] >> (p & 63)) & 1u; }
    void set(u32 p) { w[p >> 6] |= u64(1) << (p & 63); }
    friend bool operator==(const PropMask&, const PropMask&) = default;
};
/// The transition that applies to one property (assembled from the transition-* lists).
struct TransitionSpec {
    f32 duration = 0.f, delay = 0.f;
    Easing ease;
    bool allowDiscrete = false; ///< transition-behavior: allow-discrete
};
/// Back-compat alias of easeValue.
inline f32 transitionEase(const TransitionSpec& s, f32 t) { return easeValue(s.ease, t); }
enum class AnimDirection : u8 { Normal, Reverse, Alternate, AlternateReverse };
enum class AnimFill : u8 { None, Forwards, Backwards, Both };
/// How an animation's value combines with the underlying value (animation-composition / KeyframeEffect.composite).
enum class AnimComposite : u8 { Replace, Add, Accumulate };
/// Interned animation / keyframes names (the filter-name table; stable for the process). 0 = none.
inline u32 animationNameId(std::string_view name) { return filterNameId(name) + 1; }
inline std::string_view animationName(u32 id) { return id ? filterName(id - 1) : std::string_view("none"); }

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
    f32 gradientAngle = 180.f; ///< GradientDir::Angle: CSS degrees (0 = to top, 90 = to right)
    u8 gradientStopCount = 0;  ///< GradientDir::Angle stops (offsets 0..1, colours straight alpha)
    std::array<f32, kMaxGridTracks> gradientOffsets{};
    std::array<Color, kMaxGridTracks> gradientColors{};
    f32 borderWidth = 0.f;
    std::array<f32, 4> radius{}; ///< tl, tr, br, bl
    f32 fontSize = 14.f;
    TextAlignment textAlign = TextAlignment::Start;
    f32 opacity = 1.f;
    f32 shadowBlur = 0.f, shadowX = 0.f, shadowY = 0.f;
    f32 backdropBlur = 0.f; ///< backdrop-filter: blur(<len>) / backdrop-blur: <len> (frosted glass under the background)
    /// backdrop-filter: the function list in CSS order (blur amount = length; DropShadow amount = index into
    /// backdropShadows; Url amount = the bit pattern of the filter name id)
    u8 backdropFilterCount = 0;
    std::array<FilterFn, kMaxGridTracks> backdropFilterFns{};
    std::array<f32, kMaxGridTracks> backdropFilterAmounts{};
    std::array<FilterShadow, kMaxFilterShadows> backdropShadows{};
    /// CSS `filter` (element), same encoding; painted as nested isolated layers in CSS order
    u8 transformCount = 0; ///< transform: functions in list order (about transformOrigin)
    std::array<TransformFn, kMaxTransformArgs> transformFns{};
    std::array<f32, kMaxTransformArgs> transformArgs{};
    std::array<Length, 2> transformOrigin{Length::pct(50.f), Length::pct(50.f)}; ///< transform-origin (border box)
    f32 transformOriginZ = 0.f;          ///< transform-origin third value (px)
    f32 perspective = 0.f;               ///< perspective: px for the children's 3D transforms (0: none)
    std::array<Length, 2> perspectiveOrigin{Length::pct(50.f), Length::pct(50.f)};
    bool backfaceHidden = false;         ///< backface-visibility: hidden
    bool preserve3d = false;             ///< transform-style: preserve-3d
    /// view-timeline-inset (Unit::Auto: auto = 0)
    std::array<Length, 2> viewTimelineInset{};
    /// view(<axis> <inset>{1,2}) insets per animation-timeline entry
    std::array<std::array<Length, 2>, kMaxTransitions> animationTimelineInset{};
    /// timeline-scope: names (animationNameId) whose descendant-declared timelines become visible here; all: every name
    u8 timelineScopeCount = 0;
    bool timelineScopeAll = false;
    std::array<u32, kMaxTransitions> timelineScope{};
    /// animation-range-start / -end per animation entry: name (TimelineRange) + offset (% of the range or px)
    u8 animationRangeStartCount = 1, animationRangeEndCount = 1;
    std::array<u8, kMaxTransitions> animationRangeStartName{}, animationRangeEndName{};
    std::array<Length, kMaxTransitions> animationRangeStartOffset{}, animationRangeEndOffset{};
    /// animation-timeline entries (index like the other animation-* lists): kind 0 auto (document), 1 none, 2 a named
    /// scroll / view timeline (name = animationNameId of the dashed ident), 3 scroll(), 4 view(); arg: axis (bit 0:
    /// 1 = block / y) | scroller << 1 (scroll(): 0 nearest, 1 root, 2 self)
    u8 animationTimelineCount = 1;
    std::array<u8, kMaxTransitions> animationTimelineKind{};
    std::array<u32, kMaxTransitions> animationTimelineName{};
    std::array<u8, kMaxTransitions> animationTimelineArg{};
    /// scroll-timeline / view-timeline: the named timeline this element defines (0: none); axis 1 = block / y
    u32 scrollTimelineName = 0, viewTimelineName = 0;
    u8 scrollTimelineAxis = 1, viewTimelineAxis = 1;
    u8 filterCount = 0;
    std::array<FilterFn, kMaxGridTracks> filterFns{};
    std::array<f32, kMaxGridTracks> filterAmounts{};
    std::array<FilterShadow, kMaxFilterShadows> filterShadows{};
    BackgroundClip backgroundClip = BackgroundClip::BorderBox; ///< background-clip: border-box | padding-box | content-box
    bool pointerEvents = true;
    // transition-* lists (lengths independent; property entries index the others cyclically)
    u8 transitionPropertyCount = 1, transitionDurationCount = 1, transitionEaseCount = 1, transitionDelayCount = 1;
    std::array<PropMask, kMaxTransitions> transitionProperty{PropMask{{~u64(0), ~u64(0)}}}; ///< initial: all
    std::array<f32, kMaxTransitions> transitionDuration{}, transitionDelay{};
    std::array<Easing, kMaxTransitions> transitionEase{};
    // animation-* lists (animation-name entries index the others cyclically)
    u8 animationNameCount = 1, animationDurationCount = 1, animationEaseCount = 1, animationDelayCount = 1,
       animationIterationCount = 1, animationDirectionCount = 1, animationFillCount = 1, animationPlayCount = 1;
    std::array<u32, kMaxTransitions> animationName{}; ///< animationNameId; 0 = none
    std::array<f32, kMaxTransitions> animationDuration{}, animationDelay{};
    std::array<f32, kMaxTransitions> animationIterations{1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f}; ///< inf = infinite
    std::array<Easing, kMaxTransitions> animationEase{};
    std::array<AnimDirection, kMaxTransitions> animationDirection{};
    std::array<AnimFill, kMaxTransitions> animationFill{};
    std::array<u8, kMaxTransitions> animationPaused{};
    u8 transitionBehaviorCount = 1, animationCompositeCount = 1;
    std::array<u8, kMaxTransitions> transitionDiscrete{}; ///< transition-behavior: 1 = allow-discrete
    std::array<AnimComposite, kMaxTransitions> animationComposite{};
    /// The transition covering property `p` (the last transition-property entry naming it); false: none.
    bool transitionFor(u32 p, TransitionSpec& out) const {
        for (int k = int(transitionPropertyCount) - 1; k >= 0; --k)
            if (transitionProperty[std::size_t(k)].has(p)) {
                out.duration = transitionDuration[std::size_t(k % transitionDurationCount)];
                out.delay = transitionDelay[std::size_t(k % transitionDelayCount)];
                out.ease = transitionEase[std::size_t(k % transitionEaseCount)];
                out.allowDiscrete = transitionDiscrete[std::size_t(k % (transitionBehaviorCount ? transitionBehaviorCount : 1))] != 0;
                return true;
            }
        return false;
    }
    bool hasTransitions() const {
        for (u8 k = 0; k < transitionDurationCount; ++k) if (transitionDuration[k] > 0.f) return true;
        for (u8 k = 0; k < transitionDelayCount; ++k) if (transitionDelay[k] > 0.f) return true;
        return false;
    }
    bool hasAnimations() const {
        for (u8 k = 0; k < animationNameCount; ++k) if (animationName[k]) return true;
        return false;
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
    ShadowY, PointerEvents, BackdropBlur, GradientAngle, GradientStops, BackgroundClip, BackdropFilter, Filter,
    TransitionProperty, TransitionDuration, TransitionTimingFunction, TransitionDelay,
    AnimationName, AnimationDuration, AnimationTimingFunction, AnimationDelay, AnimationIterationCount,
    AnimationDirection, AnimationFillMode, AnimationPlayState, TransitionBehavior, AnimationComposition, Transform,
    TransformOrigin, AnimationTimeline, ScrollTimeline, ViewTimeline, Perspective, PerspectiveOrigin,
    BackfaceVisibility, TransformStyle, ViewTimelineInset, TimelineScope, AnimationRangeStart, AnimationRangeEnd, Count
};
inline constexpr int kPropCount = int(Prop::Count);
static_assert(kPropCount <= 128, "PropMask holds 128 properties");

enum class PropType : u8 { Bool, Enum, Length, Number, Color, Tracks, Stops, Filter, List, Transform }; ///< Transform: fns[] (TransformFn) + tracks[].value arguments ///< Filter: fns[] + tracks[].value amounts ///< Stops: offsets in tracks[].value, colours in stopColors
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
    std::array<fuse::ui::Color, kMaxGridTracks> stopColors{};
    std::array<u8, kMaxGridTracks> fns{}; ///< Filter: FilterFn per entry
    std::array<FilterShadow, kMaxFilterShadows> shadows{}; ///< Filter: drop-shadow() entries (fn amount = index)
    u8 shadowCount = 0;
    /// List (transition-* / animation-* longhands; count in trackCount): masks (property), tracks[k].value (times,
    /// iteration counts), eases (timing functions), ids (animation names), fns[k] (direction / fill / play state)
    std::array<PropMask, kMaxTransitions> masks{};
    std::array<Easing, kMaxTransitions> eases{};
    std::array<u32, kMaxTransitions> ids{};
    std::array<std::array<Length, 2>, kMaxTransitions> pairs{}; ///< List: view() insets / animation-range offsets
    /// Transform: functions + arguments (count in trackCount, up to kMaxTransformArgs)
    std::array<u8, kMaxTransformArgs> xfns{};
    std::array<f32, kMaxTransformArgs> xargs{};
    static PropValue ofBool(bool v) { PropValue p; p.type = PropType::Bool; p.b = v; return p; }
    static PropValue ofEnum(u8 v) { PropValue p; p.type = PropType::Enum; p.e = v; return p; }
    static PropValue ofLength(Length v) { PropValue p; p.type = PropType::Length; p.len = v; return p; }
    static PropValue ofNumber(f32 v) { PropValue p; p.type = PropType::Number; p.num = v; return p; }
    static PropValue ofColor(fuse::ui::Color v) { PropValue p; p.type = PropType::Color; p.color = v; return p; }
};
void applyProp(ComputedStyle& s, Prop p, const PropValue& v);
PropValue readProp(const ComputedStyle& s, Prop p);
/// Interpolates two values of an animatable property (lengths of different units and other types switch at 0.5;
/// colours in premultiplied sRGB as CSS does). k may leave [0, 1] (overshooting cubic-bezier).
PropValue interpolateProp(const PropValue& a, const PropValue& b, f32 k);
bool samePropValue(const PropValue& a, const PropValue& b);
/// Discrete interpolation of property p (CSS Transitions 2 / Web Animations): flips at k = 0.5, except `display`,
/// which shows the visible value over the whole (0, 1) interval.
PropValue interpolateDiscrete(Prop p, const PropValue& a, const PropValue& b, f32 k);
/// Combines an effect value with the underlying value: Replace keeps v; Add / Accumulate sum numbers, same-unit
/// lengths and colour components (clamped); other types (or mismatched units) replace.
/// Transform lists: Add appends v's functions to the underlying list (scale(2) + scale(3) = scale(2) scale(3), x6);
/// Accumulate sums matching functions (translate adds, scale factors add minus one: scale(2) + scale(3) = scale(4)).
PropValue compositeProp(const PropValue& underlying, const PropValue& v, AnimComposite c);
/// The 2D map a computed transform paints with, about its transform-origin in `box` (the border box); a 3D
/// transform gives its 2D part (use transform3dOf).
Affine2 transformOf(const ComputedStyle& s, const Rect& box);
/// The full 3D map about transform-origin (x, y, z).
Mat4 transform3dOf(const ComputedStyle& s, const Rect& box);
/// The perspective a parent with `perspective` applies to its children (about perspective-origin in its border box);
/// identity without perspective.
Mat4 perspectiveOf(const ComputedStyle& parent, const Rect& parentBox);
/// animation-range names (Scroll-driven Animations 1): Normal = the whole timeline
enum class TimelineRange : u8 { Normal, Cover, Contain, Entry, Exit, EntryCrossing, ExitCrossing };
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
};

// ---- Style sheets --------------------------------------------------------------------------------------------------
enum class StyleError : u8 { Syntax, UnknownProperty, BadValue };
struct StyleDiagnostic {
    StyleError error;
    u32 line;
    std::string message;
};

struct SimpleSelector {
    u32 typeHash = 0; ///< 0 = any
    u32 idHash = 0;
    std::array<u32, 4> classes{};
    u8 classCount = 0;
    u32 states = 0;
    enum class Combinator : u8 { None, Descendant, Child, Adjacent, Sibling } combinator = Combinator::None; ///< to the previous (left)
    /// [name], [name=v], [name~=v], [name|=v], [name^=v], [name$=v], [name*=v]; ci: ` i` flag
    struct Attr {
        enum class Op : u8 { Exists, Equals, Word, DashPrefix, Prefix, Suffix, Contains };
        u32 nameHash = 0;
        Op op = Op::Exists;
        bool ci = false;
        std::string value;
        friend bool operator==(const Attr&, const Attr&) = default;
    };
    std::vector<Attr> attrs;
    /// :nth-child(An+B) (Child), :nth-last-child (LastChild), :nth-of-type (OfType), :nth-last-of-type (LastOfType)
    struct Nth {
        enum class Kind : u8 { Child, LastChild, OfType, LastOfType };
        Kind kind = Kind::Child;
        i32 a = 0, b = 1;
        friend bool operator==(const Nth&, const Nth&) = default;
    };
    std::vector<Nth> nths;
    /// :not() / :is() / :where() / :has(): each argument is a complex selector (left to right); in :has() the first
    /// compound's combinator is the relation to the subject (Descendant by default).
    struct Logical {
        enum class Kind : u8 { Not, Is, Where, Has };
        Kind kind = Kind::Is;
        std::vector<std::vector<SimpleSelector>> args;
    };
    std::vector<Logical> logicals;
    enum Flags : u8 { kEmpty = 1, kRoot = 2 }; ///< :empty, :root inside a logical argument
    u8 flags = 0;
};

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
};

/// What the matcher needs to know about a node and its ancestors.
struct StyleSubject {
    virtual ~StyleSubject() = default;
    virtual u32 typeHash(u32 node) const = 0;
    virtual u32 idHash(u32 node) const = 0;
    virtual bool hasClass(u32 node, u32 classHash) const = 0;
    virtual u32 states(u32 node) const = 0;
    virtual u32 parent(u32 node) const = 0; ///< 0xFFFFFFFF at the root
    virtual u32 prevSibling(u32) const { return 0xFFFFFFFFu; }
    virtual u32 nextSibling(u32) const { return 0xFFFFFFFFu; }
    /// Attribute value of `node` (false: absent).
    virtual bool attribute(u32, u32 /*nameHash*/, std::string_view&) const { return false; }
    virtual u32 firstChild(u32) const { return 0xFFFFFFFFu; }
    /// :empty — no child nodes and no text
    virtual bool isEmpty(u32 node) const { return firstChild(node) == 0xFFFFFFFFu; }
};

/// Parses An+B (odd, even, 3, -n+2, 2n+1, ...).
bool parseNth(std::string_view text, i32& a, i32& b);

/// @keyframes: offsets 0..1 with their declarations (animatable properties) and optional per-keyframe easing.
struct Keyframe {
    f32 offset = 0.f;
    std::vector<Declaration> decls;
    bool hasEase = false;
    Easing ease;
};
struct KeyframesRule {
    u32 name = 0; ///< animationNameId
    std::vector<Keyframe> frames; ///< sorted by offset (stable)
};

class StyleSheet {
public:
    /// Parses CSS-subset text. Unknown properties and bad values are skipped and reported in `diagnostics`;
    /// unrecoverable syntax errors fail.
    static std::expected<StyleSheet, StyleDiagnostic> parse(std::string_view css,
                                                            std::vector<StyleDiagnostic>* diagnostics = nullptr);
    const std::vector<StyleRule>& rules() const { return m_rules; }
    /// Theme tokens (from :root).
    bool token(u32 nameHash, std::string_view& out) const;
    const std::vector<std::pair<u32, std::string>>& tokens() const { return m_tokens; }
    /// Rebuilds a parsed sheet from its parts (cooked `.fusestyle` loading; see fuse/ui/doc/cook.hpp).
    static StyleSheet fromParts(std::vector<StyleRule> rules, std::vector<std::pair<u32, std::string>> tokens,
                                std::vector<KeyframesRule> keyframes = {}) {
        StyleSheet s;
        s.m_rules = std::move(rules);
        s.m_tokens = std::move(tokens);
        s.m_keyframes = std::move(keyframes);
        return s;
    }
    const std::vector<KeyframesRule>& keyframes() const { return m_keyframes; }
    /// Uses structural (:nth-*, sibling) selectors: sibling changes restyle the siblings.
    bool structural() const;
    /// Uses :has(): changes below or after a node can restyle it.
    bool relational() const;

private:
    friend class StyleEngine;
    std::vector<StyleRule> m_rules;
    std::vector<std::pair<u32, std::string>> m_tokens;
    std::vector<KeyframesRule> m_keyframes;
};

/// Parses one value for `p` (also used by inline setters and markup). False on a bad value.
bool parsePropValue(Prop p, std::string_view text, PropValue& out);
bool parseColor(std::string_view text, Color& out);

/// Ordered sheets + deduplicated computed-style table + matcher.
class StyleEngine {
public:
    StyleEngine();
    void clearSheets();
    void addSheet(StyleSheet sheet);
    /// Swaps sheet `index` (cascade position kept) — used by style hot reload. False when out of range.
    bool replaceSheet(u32 index, StyleSheet sheet);
    u32 sheetCount() const { return u32(m_sheets.size()); }
    const StyleSheet& sheet(u32 i) const { return m_sheets[i]; }
    u32 sheetVersion() const { return m_version; }
    /// Computes the style of `node`: defaults → inherited from `parentStyle` → matched rules → `inlineValues`.
    /// Returns an index into the table (allocates only for a never-seen combination).
    u32 compute(const StyleSubject& subject, u32 node, const ComputedStyle* parentStyle,
                const ComputedStyle* defaults, const std::vector<std::pair<Prop, PropValue>>& inlineValues);
    const ComputedStyle& style(u32 index) const { return m_table[index]; }
    /// compute() without interning
    ComputedStyle computeValue(const StyleSubject& subject, u32 node, const ComputedStyle* parentStyle,
                               const ComputedStyle* defaults, const std::vector<std::pair<Prop, PropValue>>& inlineValues);
    /// The last @keyframes rule named `name` over all sheets (nullptr: none).
    const KeyframesRule* keyframes(u32 name) const;
    /// Resolves a keyframe declaration (var() against the sheets' tokens).
    bool resolveDecl(const Declaration& d, PropValue& out) const;
    bool structural() const { return m_structural; }
    bool relational() const { return m_relational; }
    /// Invalidation reach of a selector feature (class, attribute, state, tree position) changing on a node, over all
    /// sheets: which other nodes may change their match. Levels saturate at kDeep.
    struct Invalidation {
        static constexpr u8 kDeep = 255;
        u8 down = 0;          ///< descendant levels (child combinator: 1 each, descendant: deep)
        u8 sib = 0;           ///< later element siblings reached (`+`: 1 each, `~`: kDeep = all), with descendants to `down`
        u8 hasUp = 0;         ///< :has() anchors: ancestor levels above the node (0 with hasPrev: siblings only)
        bool hasPrev = false; ///< :has() anchors among earlier siblings (of the node and of each ancestor walked)
        bool has = false;     ///< inside some :has() argument
        u8 hasDown = 0;       ///< from each anchor: descendant levels whose match depends on the anchor
        bool hasSib = false;  ///< from each anchor: later siblings
        void merge(const Invalidation& o);
    };
    const Invalidation& classInvalidation(u32 classHash) const;
    const Invalidation& attrInvalidation(u32 nameHash) const;
    const Invalidation& stateInvalidation() const { return m_stateInv; }
    /// Insert / remove / reorder / :empty changes (type, position): reach through :has() and the deepest `down`.
    const Invalidation& treeInvalidation() const { return m_treeInv; }
    bool isScratch(u32 index) const { return index < m_isScratch.size() && m_isScratch[index]; }
    /// Unshared table slots for per-node styles that change every frame (transitions): never deduplicated, reused.
    u32 allocScratch(const ComputedStyle& cs);
    void setScratch(u32 index, const ComputedStyle& cs) { m_table[index] = cs; }
    void freeScratch(u32 index) { m_freeScratch.push_back(index); }
    u32 tableSize() const { return u32(m_table.size()); }
    bool matches(const StyleSubject& subject, u32 node, const StyleRule& rule) const;
    bool matchFrom(const StyleSubject& subject, u32 node, const StyleRule& rule, int k) const;
    /// Matches a complex selector (left to right) ending at `node`.
    static bool matchChain(const StyleSubject& subject, u32 node, const std::vector<SimpleSelector>& chain);

private:
    static bool matchSimple(const StyleSubject& s, u32 node, const SimpleSelector& sel);
    bool resolveVar(std::string_view raw, Prop p, PropValue& out, int depth) const;
    u32 intern(const ComputedStyle& cs);
    void reorder();
    std::vector<StyleSheet> m_sheets;
    std::vector<const StyleRule*> m_ordered; ///< all rules, sorted by (specificity, sheet, order)
    std::vector<const StyleRule*> m_scratch;
    std::vector<std::pair<u32, std::string_view>> m_localTokens;
    std::vector<ComputedStyle> m_table;
    std::vector<u32> m_hashIndex; ///< open addressing into m_table (+1)
    std::vector<u8> m_isScratch;  ///< per table entry
    bool m_structural = false, m_relational = false;
    std::unordered_map<u32, Invalidation> m_classInv, m_attrInv;
    Invalidation m_stateInv, m_treeInv, m_noInv;
    void buildInvalidation();
    std::vector<u32> m_freeScratch;
    u32 m_version = 0;
};

} // namespace fuse::ui
