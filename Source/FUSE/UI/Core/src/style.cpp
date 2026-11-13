#include "fuse/ui/style.hpp"

#include "style_internal.hpp"

#include "fuse/text/font_family.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <charconv>
#include <cstring>

namespace fuse::ui {

using namespace detail;

namespace {
using T = PropType;
// clang-format off
constexpr PropInfo kProps[] = {
    {Prop::Display, "display", T::Bool, false, false, true},
    {Prop::Layout, "layout", T::Enum, false, false, true},
    {Prop::Direction, "flex-direction", T::Enum, false, false, true},
    {Prop::Width, "width", T::Length, false, true, true},
    {Prop::Height, "height", T::Length, false, true, true},
    {Prop::MinWidth, "min-width", T::Length, false, true, true},
    {Prop::MinHeight, "min-height", T::Length, false, true, true},
    {Prop::MaxWidth, "max-width", T::Length, false, true, true},
    {Prop::MaxHeight, "max-height", T::Length, false, true, true},
    {Prop::Basis, "flex-basis", T::Length, false, true, true},
    {Prop::Grow, "flex-grow", T::Number, false, true, true},
    {Prop::Shrink, "flex-shrink", T::Number, false, true, true},
    {Prop::PaddingLeft, "padding-left", T::Length, false, true, true},
    {Prop::PaddingTop, "padding-top", T::Length, false, true, true},
    {Prop::PaddingRight, "padding-right", T::Length, false, true, true},
    {Prop::PaddingBottom, "padding-bottom", T::Length, false, true, true},
    {Prop::MarginLeft, "margin-left", T::Length, false, true, true},
    {Prop::MarginTop, "margin-top", T::Length, false, true, true},
    {Prop::MarginRight, "margin-right", T::Length, false, true, true},
    {Prop::MarginBottom, "margin-bottom", T::Length, false, true, true},
    {Prop::Gap, "gap", T::Length, false, true, true},
    {Prop::AlignItems, "align-items", T::Enum, false, false, true},
    {Prop::AlignSelf, "align-self", T::Enum, false, false, true},
    {Prop::Justify, "justify-content", T::Enum, false, false, true},
    {Prop::Wrap, "flex-wrap", T::Bool, false, false, true},
    {Prop::Overflow, "overflow", T::Enum, false, false, true},
    {Prop::GridColumns, "grid-template-columns", T::Tracks, false, false, true},
    {Prop::GridColumn, "grid-column", T::Number, false, false, true},
    {Prop::GridColumnSpan, "grid-column-span", T::Number, false, false, true},
    {Prop::GridRow, "grid-row", T::Number, false, false, true},
    {Prop::GridRowSpan, "grid-row-span", T::Number, false, false, true},
    {Prop::Left, "left", T::Length, false, true, true},
    {Prop::Top, "top", T::Length, false, true, true},
    {Prop::Right, "right", T::Length, false, true, true},
    {Prop::Bottom, "bottom", T::Length, false, true, true},
    {Prop::CenterX, "center-x", T::Bool, false, false, true},
    {Prop::CenterY, "center-y", T::Bool, false, false, true},
    {Prop::Background, "background", T::Color, false, true, false},
    {Prop::Background2, "background-end", T::Color, false, true, false},
    {Prop::Gradient, "gradient", T::Enum, false, false, false},
    {Prop::BorderColor, "border-color", T::Color, false, true, false},
    {Prop::BorderWidth, "border-width", T::Length, false, true, false},
    {Prop::RadiusTL, "border-top-left-radius", T::Length, false, true, false},
    {Prop::RadiusTR, "border-top-right-radius", T::Length, false, true, false},
    {Prop::RadiusBR, "border-bottom-right-radius", T::Length, false, true, false},
    {Prop::RadiusBL, "border-bottom-left-radius", T::Length, false, true, false},
    {Prop::Color, "color", T::Color, true, true, false},
    {Prop::FontSize, "font-size", T::Length, true, true, true},
    {Prop::TextAlign, "text-align", T::Enum, true, false, false},
    {Prop::Opacity, "opacity", T::Number, false, true, false},
    {Prop::ShadowColor, "shadow-color", T::Color, false, true, false},
    {Prop::ShadowBlur, "shadow-blur", T::Length, false, true, false},
    {Prop::ShadowX, "shadow-x", T::Length, false, true, false},
    {Prop::ShadowY, "shadow-y", T::Length, false, true, false},
    {Prop::PointerEvents, "pointer-events", T::Bool, false, false, false},
    {Prop::BackdropBlur, "backdrop-blur", T::Length, false, true, false},
    {Prop::FontWeight, "font-weight", T::Number, true, false, true},
    {Prop::FontStyle, "font-style", T::Enum, true, false, true},
    {Prop::FontFamily, "font-family", T::Tracks, true, false, true}, // each track: the 24-bit family hash as px
    {Prop::TextDecoration, "text-decoration", T::Enum, false, false, false},
    {Prop::LetterSpacing, "letter-spacing", T::Length, true, true, true},
    {Prop::LineHeight, "line-height", T::Length, true, false, true},
    {Prop::Cursor, "cursor", T::Enum, false, false, false},
    {Prop::TextDirection, "direction", T::Enum, true, false, true},
    {Prop::PaddingStart, "padding-start", T::Length, false, false, true},
    {Prop::PaddingEnd, "padding-end", T::Length, false, false, true},
    {Prop::Transition, "transition-entry", T::Number, false, false, false}, // written through the `transition` shorthand
    {Prop::EnterAnimation, "enter-animation", T::Enum, false, false, false},
    {Prop::EnterDuration, "enter-duration", T::Number, false, false, false},
    {Prop::ExitAnimation, "exit-animation", T::Enum, false, false, false},
    {Prop::ExitDuration, "exit-duration", T::Number, false, false, false},
    {Prop::Position, "position", T::Enum, false, false, true},
    {Prop::ZIndex, "z-index", T::Number, false, false, false},
    {Prop::AspectRatio, "aspect-ratio", T::Number, false, false, true},
    {Prop::Transform, "transform", T::Raw, false, true, false},
    {Prop::TransformOriginX, "transform-origin-x", T::Length, false, true, false},
    {Prop::TransformOriginY, "transform-origin-y", T::Length, false, true, false},
    {Prop::Filter, "filter", T::Raw, false, true, false},
    {Prop::ClipPath, "clip-path", T::Raw, false, false, false},
    {Prop::MaskImage, "mask-image", T::Raw, false, false, false},
    {Prop::MaskMode, "mask-mode", T::Enum, false, false, false},
    {Prop::BackgroundImage, "background-image", T::Raw, false, false, false},
    {Prop::AnimationName, "animation-name", T::Raw, false, false, false},
    {Prop::AnimationDuration, "animation-duration", T::Raw, false, false, false},
    {Prop::AnimationTimingFunction, "animation-timing-function", T::Raw, false, false, false},
    {Prop::AnimationDelay, "animation-delay", T::Raw, false, false, false},
    {Prop::AnimationIterationCount, "animation-iteration-count", T::Raw, false, false, false},
    {Prop::AnimationDirection, "animation-direction", T::Raw, false, false, false},
    {Prop::AnimationFillMode, "animation-fill-mode", T::Raw, false, false, false},
    {Prop::AnimationPlayState, "animation-play-state", T::Raw, false, false, false},
    {Prop::ContainerType, "container-type", T::Enum, false, false, false},
    {Prop::ContainerName, "container-name", T::Number, false, false, false},
};
// clang-format on
static_assert(sizeof(kProps) / sizeof(kProps[0]) == kPropCount);

struct EnumWord {
    Prop prop;
    std::string_view word;
    u8 value;
};
constexpr EnumWord kEnums[] = {
    {Prop::Layout, "box", u8(LayoutKind::Box)},       {Prop::Layout, "flex", u8(LayoutKind::Box)},
    {Prop::Layout, "grid", u8(LayoutKind::Grid)},     {Prop::Layout, "stack", u8(LayoutKind::Stack)},
    {Prop::Layout, "anchor", u8(LayoutKind::Anchor)}, {Prop::Layout, "absolute", u8(LayoutKind::Absolute)},
    {Prop::Layout, "flow", u8(LayoutKind::Flow)},     {Prop::Direction, "row", u8(Direction::Row)},
    {Prop::Direction, "column", u8(Direction::Column)},
    {Prop::AlignItems, "start", u8(Align::Start)},    {Prop::AlignItems, "flex-start", u8(Align::Start)},
    {Prop::AlignItems, "center", u8(Align::Center)},  {Prop::AlignItems, "end", u8(Align::End)},
    {Prop::AlignItems, "flex-end", u8(Align::End)},   {Prop::AlignItems, "stretch", u8(Align::Stretch)},
    {Prop::AlignSelf, "auto", u8(Align::Auto)},       {Prop::AlignSelf, "start", u8(Align::Start)},
    {Prop::AlignSelf, "center", u8(Align::Center)},   {Prop::AlignSelf, "end", u8(Align::End)},
    {Prop::AlignSelf, "stretch", u8(Align::Stretch)},
    {Prop::Justify, "start", u8(Justify::Start)},     {Prop::Justify, "flex-start", u8(Justify::Start)},
    {Prop::Justify, "center", u8(Justify::Center)},   {Prop::Justify, "end", u8(Justify::End)},
    {Prop::Justify, "flex-end", u8(Justify::End)},    {Prop::Justify, "space-between", u8(Justify::SpaceBetween)},
    {Prop::Justify, "space-around", u8(Justify::SpaceAround)},
    {Prop::Overflow, "visible", u8(Overflow::Visible)}, {Prop::Overflow, "hidden", u8(Overflow::Clip)},
    {Prop::Overflow, "clip", u8(Overflow::Clip)},     {Prop::Overflow, "scroll", u8(Overflow::Scroll)},
    {Prop::Overflow, "auto", u8(Overflow::Scroll)},
    {Prop::Gradient, "none", u8(GradientDir::None)},  {Prop::Gradient, "vertical", u8(GradientDir::Vertical)},
    {Prop::Gradient, "horizontal", u8(GradientDir::Horizontal)},
    {Prop::TextAlign, "left", u8(TextAlignment::Start)}, {Prop::TextAlign, "start", u8(TextAlignment::Start)},
    {Prop::TextAlign, "center", u8(TextAlignment::Center)}, {Prop::TextAlign, "right", u8(TextAlignment::End)},
    {Prop::TextAlign, "end", u8(TextAlignment::End)},
    {Prop::FontStyle, "normal", 0}, {Prop::FontStyle, "italic", 1}, {Prop::FontStyle, "oblique", 1},
    {Prop::Cursor, "auto", u8(Cursor::Auto)},         {Prop::Cursor, "default", u8(Cursor::Arrow)},
    {Prop::Cursor, "text", u8(Cursor::IBeam)},        {Prop::Cursor, "pointer", u8(Cursor::PointingHand)},
    {Prop::Cursor, "ew-resize", u8(Cursor::ResizeH)}, {Prop::Cursor, "col-resize", u8(Cursor::ResizeH)},
    {Prop::Cursor, "ns-resize", u8(Cursor::ResizeV)}, {Prop::Cursor, "row-resize", u8(Cursor::ResizeV)},
    {Prop::Cursor, "nwse-resize", u8(Cursor::ResizeNWSE)}, {Prop::Cursor, "nesw-resize", u8(Cursor::ResizeNESW)},
    {Prop::Cursor, "move", u8(Cursor::Move)},         {Prop::Cursor, "crosshair", u8(Cursor::Crosshair)},
    {Prop::Cursor, "not-allowed", u8(Cursor::NotAllowed)}, {Prop::Cursor, "wait", u8(Cursor::Wait)},
    {Prop::Cursor, "none", u8(Cursor::Hidden)},
    {Prop::TextDirection, "ltr", 0}, {Prop::TextDirection, "rtl", 1},
    {Prop::EnterAnimation, "none", u8(MotionKind::None)}, {Prop::EnterAnimation, "fade", u8(MotionKind::Fade)},
    {Prop::EnterAnimation, "slide-up", u8(MotionKind::SlideUp)}, {Prop::EnterAnimation, "slide-down", u8(MotionKind::SlideDown)},
    {Prop::ExitAnimation, "none", u8(MotionKind::None)}, {Prop::ExitAnimation, "fade", u8(MotionKind::Fade)},
    {Prop::ExitAnimation, "slide-up", u8(MotionKind::SlideUp)}, {Prop::ExitAnimation, "slide-down", u8(MotionKind::SlideDown)},
    {Prop::Position, "static", u8(PositionKind::Static)}, {Prop::Position, "relative", u8(PositionKind::Relative)},
    {Prop::Position, "absolute", u8(PositionKind::Absolute)}, {Prop::Position, "fixed", u8(PositionKind::Fixed)},
    {Prop::Position, "sticky", u8(PositionKind::Sticky)},
    {Prop::MaskMode, "alpha", 0}, {Prop::MaskMode, "match-source", 0}, {Prop::MaskMode, "luminance", 1},
    {Prop::ContainerType, "normal", 0}, {Prop::ContainerType, "inline-size", 1}, {Prop::ContainerType, "size", 2},
};

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = char(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    return -1;
}

struct NamedColor { std::string_view name; u32 hex; };
constexpr NamedColor kNamed[] = {{"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000}, {"green", 0x008000},
                                 {"blue", 0x0000FF}, {"yellow", 0xFFFF00}, {"gray", 0x808080}, {"grey", 0x808080},
                                 {"orange", 0xFFA500}, {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF}};
} // namespace

const PropInfo& propInfo(Prop p) { return kProps[int(p)]; }
bool findProp(std::string_view name, Prop& out) {
    for (const auto& pi : kProps)
        if (pi.cssName == name) { out = pi.id; return true; }
    return false;
}

bool parseColor(std::string_view s, Color& out) {
    s = trim(s);
    if (s == "transparent") { out = {}; return true; }
    for (const auto& n : kNamed)
        if (n.name == s) { out = Color::rgb(n.hex); return true; }
    if (!s.empty() && s[0] == '#') {
        const std::string_view h = s.substr(1);
        int d[8];
        for (std::size_t i = 0; i < h.size() && i < 8; ++i)
            if ((d[i] = hexval(h[i])) < 0) return false;
        if (h.size() == 3 || h.size() == 4) {
            out = {f32(d[0] * 17) / 255.f, f32(d[1] * 17) / 255.f, f32(d[2] * 17) / 255.f,
                   h.size() == 4 ? f32(d[3] * 17) / 255.f : 1.f};
            return true;
        }
        if (h.size() == 6 || h.size() == 8) {
            out = {f32(d[0] * 16 + d[1]) / 255.f, f32(d[2] * 16 + d[3]) / 255.f, f32(d[4] * 16 + d[5]) / 255.f,
                   h.size() == 8 ? f32(d[6] * 16 + d[7]) / 255.f : 1.f};
            return true;
        }
        return false;
    }
    if (s.starts_with("rgb")) {
        const auto open = s.find('('), close = s.rfind(')');
        if (open == std::string_view::npos || close == std::string_view::npos) return false;
        std::string_view args = s.substr(open + 1, close - open - 1);
        f32 v[4] = {0, 0, 0, 1};
        int n = 0;
        while (!args.empty() && n < 4) {
            const auto comma = args.find(',');
            std::string_view unit;
            if (!parseNumber(args.substr(0, comma), v[n], unit)) return false;
            if (unit == "%") v[n] = v[n] * (n < 3 ? 255.f : 1.f) / 100.f;
            ++n;
            if (comma == std::string_view::npos) break;
            args.remove_prefix(comma + 1);
        }
        if (n < 3) return false;
        out = {v[0] / 255.f, v[1] / 255.f, v[2] / 255.f, v[3]};
        return true;
    }
    return false;
}

bool parsePropValue(Prop p, std::string_view text, PropValue& out) {
    text = trim(text);
    const PropInfo& pi = propInfo(p);
    out = {};
    out.type = pi.type;
    switch (p) { // text properties with keyword / list syntax
    case Prop::FontWeight: {
        if (text == "normal") { out.num = 400.f; return true; }
        if (text == "bold" || text == "bolder") { out.num = 700.f; return true; }
        if (text == "lighter") { out.num = 300.f; return true; }
        std::string_view unit;
        return parseNumber(text, out.num, unit) && unit.empty() && out.num >= 1.f && out.num <= 1000.f;
    }
    case Prop::FontFamily: {
        // comma list; each name is stored as its case-insensitive 24-bit hash (stable across runs and in cooked sheets)
        out.trackCount = 0;
        while (!text.empty()) {
            const auto comma = text.find(',');
            const std::string_view name = trim(text.substr(0, comma));
            if (!name.empty() && out.trackCount < kMaxFontFamilies)
                out.tracks[out.trackCount++] = Length::px(f32(fuse::text::familyHash(name)));
            if (comma == std::string_view::npos) break;
            text.remove_prefix(comma + 1);
        }
        return out.trackCount > 0;
    }
    case Prop::TextDecoration: {
        out.e = 0;
        while (!text.empty()) {
            const auto sp = text.find_first_of(" \t");
            const std::string_view w = text.substr(0, sp);
            if (w == "underline") out.e |= kTextUnderline;
            else if (w == "line-through") out.e |= kTextLineThrough;
            else if (w != "none" && !w.empty()) return false;
            if (sp == std::string_view::npos) break;
            text = trim(text.substr(sp));
        }
        return true;
    }
    case Prop::Transform: return parseTransformValue(text, out);
    case Prop::Filter: return parseFilterValue(text, out);
    case Prop::ClipPath: return parseClipPathValue(text, out);
    case Prop::MaskImage:
    case Prop::BackgroundImage: return parseGradientValue(text, out);
    case Prop::AnimationName: case Prop::AnimationDuration: case Prop::AnimationTimingFunction: case Prop::AnimationDelay:
    case Prop::AnimationIterationCount: case Prop::AnimationDirection: case Prop::AnimationFillMode: case Prop::AnimationPlayState:
        return parseAnimationList(p, text, out);
    case Prop::ZIndex: {
        if (text == "auto") { out.e = 1; return true; }
        std::string_view unit;
        return parseNumber(text, out.num, unit) && unit.empty() && out.num == std::floor(out.num) && std::abs(out.num) < 1.0e6f;
    }
    case Prop::AspectRatio: { // auto | <number> | <number> / <number>
        if (text == "auto") { out.num = 0.f; return true; }
        const auto slash = text.find('/');
        f32 a = 0.f, b = 1.f;
        std::string_view unit;
        if (!parseNumber(text.substr(0, slash), a, unit) || !trim(unit).empty()) return false;
        if (slash != std::string_view::npos && (!parseNumber(text.substr(slash + 1), b, unit) || !unit.empty())) return false;
        if (!(a > 0.f) || !(b > 0.f)) return false;
        out.num = a / b;
        return true;
    }
    case Prop::TransformOriginX:
    case Prop::TransformOriginY: {
        if (text == "center") { out.len = Length::pct(50.f); return true; }
        if (text == (p == Prop::TransformOriginX ? "left" : "top")) { out.len = Length::pct(0.f); return true; }
        if (text == (p == Prop::TransformOriginX ? "right" : "bottom")) { out.len = Length::pct(100.f); return true; }
        return parseLength(text, out.len) && (out.len.unit == Unit::Px || out.len.unit == Unit::Percent);
    }
    case Prop::ContainerName:
        if (text.empty() || text.find_first_of(" \t,") != std::string_view::npos) return false;
        out.num = text == "none" ? 0.f : f32(animationNameId(text));
        return true;
    case Prop::GridColumns:
        if (text == "subgrid") { out.b = true; out.trackCount = 0; return true; }
        break;
    case Prop::LineHeight: {
        if (text == "normal") { out.len = {}; return true; }
        f32 v;
        std::string_view unit;
        if (!parseNumber(text, v, unit) || v < 0.f) return false;
        if (unit.empty()) out.len = Length::pct(v * 100.f); // a bare number multiplies the font size
        else if (unit == "px" || unit == "dp") out.len = Length::px(v);
        else if (unit == "%") out.len = Length::pct(v);
        else return false;
        return true;
    }
    default: break;
    }
    switch (pi.type) {
    case T::Bool:
        if (p == Prop::Display) { out.b = text != "none"; return true; }
        if (p == Prop::Wrap) { out.b = text == "wrap" || text == "true"; return text == "wrap" || text == "nowrap" || text == "true" || text == "false"; }
        if (p == Prop::PointerEvents) { out.b = text != "none"; return true; }
        out.b = text == "true" || text == "1" || text == "yes";
        return text == "true" || text == "false" || text == "1" || text == "0" || text == "yes" || text == "no";
    case T::Enum:
        for (const auto& e : kEnums)
            if (e.prop == p && e.word == text) { out.e = e.value; return true; }
        return false;
    case T::Length: return parseLength(text, out.len);
    case T::Number: {
        if (p == Prop::EnterDuration || p == Prop::ExitDuration) return parseSeconds(text, out.num);
        if (p == Prop::Transition) { // one entry: <property|all|none> [duration] [easing] [delay]
            std::string_view w[4];
            int n = 0;
            for (std::size_t i = 0; i < text.size() && n < 4;) {
                while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
                const std::size_t s0 = i;
                while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
                if (i > s0) w[n++] = text.substr(s0, i - s0);
            }
            if (n == 0) return false;
            Prop target;
            if (w[0] == "none") { out.e = kTransitionNone; return n == 1; }
            if (w[0] == "all") target = kTransitionAll;
            else if (!findProp(w[0], target) || !propInfo(target).animatable) return false;
            Easing ease = Easing::InOutCubic;
            f32 dur = 0.f, delay = 0.f;
            bool haveDur = false;
            for (int k = 1; k < n; ++k) {
                f32 t;
                if (parseEasing(w[k], ease)) continue;
                if (!parseSeconds(w[k], t)) return false;
                (haveDur ? delay : dur) = t;
                haveDur = true;
            }
            out = PropValue::ofTransition(target, dur, ease, delay);
            return true;
        }
        std::string_view unit;
        if (!parseNumber(text, out.num, unit)) return false;
        if (unit == "%") out.num /= 100.f;
        return unit.empty() || unit == "%";
    }
    case T::Color: return parseColor(text, out.color);
    case T::Tracks: {
        out.trackCount = 0;
        while (!text.empty()) {
            text = trim(text);
            const auto sp = text.find_first_of(" \t");
            const std::string_view tok = text.substr(0, sp);
            if (!tok.empty()) {
                if (out.trackCount >= kMaxGridTracks || !parseLength(tok, out.tracks[out.trackCount])) return false;
                ++out.trackCount;
            }
            if (sp == std::string_view::npos) break;
            text.remove_prefix(sp);
        }
        return out.trackCount > 0;
    }
    case T::Raw: break; // handled by property above
    }
    return false;
}

void applyProp(ComputedStyle& s, Prop p, const PropValue& v) {
    switch (p) {
    case Prop::Display: s.visible = v.b; break;
    case Prop::Layout: s.layout = LayoutKind(v.e); break;
    case Prop::Direction: s.direction = Direction(v.e); break;
    case Prop::Width: s.width = v.len; break;
    case Prop::Height: s.height = v.len; break;
    case Prop::MinWidth: s.minWidth = v.len; break;
    case Prop::MinHeight: s.minHeight = v.len; break;
    case Prop::MaxWidth: s.maxWidth = v.len; break;
    case Prop::MaxHeight: s.maxHeight = v.len; break;
    case Prop::Basis: s.basis = v.len; break;
    case Prop::Grow: s.grow = v.num; break;
    case Prop::Shrink: s.shrink = v.num; break;
    case Prop::PaddingLeft: s.padding.l = v.len.value; break;
    case Prop::PaddingTop: s.padding.t = v.len.value; break;
    case Prop::PaddingRight: s.padding.r = v.len.value; break;
    case Prop::PaddingBottom: s.padding.b = v.len.value; break;
    case Prop::MarginLeft: s.margin.l = v.len.value; break;
    case Prop::MarginTop: s.margin.t = v.len.value; break;
    case Prop::MarginRight: s.margin.r = v.len.value; break;
    case Prop::MarginBottom: s.margin.b = v.len.value; break;
    case Prop::Gap: s.gap = v.len.value; break;
    case Prop::AlignItems: s.alignItems = Align(v.e); break;
    case Prop::AlignSelf: s.alignSelf = Align(v.e); break;
    case Prop::Justify: s.justify = Justify(v.e); break;
    case Prop::Wrap: s.wrap = v.b; break;
    case Prop::Overflow: s.overflow = Overflow(v.e); break;
    case Prop::GridColumns: s.gridColumns = v.tracks; s.gridColumnCount = v.trackCount; s.subgrid = v.b && v.trackCount == 0; break;
    case Prop::GridColumn: s.gridColumn = u8(v.num); break;
    case Prop::GridColumnSpan: s.gridColumnSpan = u8(std::max(1.f, v.num)); break;
    case Prop::GridRow: s.gridRow = u8(v.num); break;
    case Prop::GridRowSpan: s.gridRowSpan = u8(std::max(1.f, v.num)); break;
    case Prop::Left: s.left = v.len; break;
    case Prop::Top: s.top = v.len; break;
    case Prop::Right: s.right = v.len; break;
    case Prop::Bottom: s.bottom = v.len; break;
    case Prop::CenterX: s.centerX = v.b; break;
    case Prop::CenterY: s.centerY = v.b; break;
    case Prop::Background: s.background = v.color; break;
    case Prop::Background2: s.background2 = v.color; break;
    case Prop::Gradient: s.gradient = GradientDir(v.e); break;
    case Prop::BorderColor: s.borderColor = v.color; break;
    case Prop::BorderWidth: s.borderWidth = v.len.value; break;
    case Prop::RadiusTL: s.radius[0] = v.len.value; break;
    case Prop::RadiusTR: s.radius[1] = v.len.value; break;
    case Prop::RadiusBR: s.radius[2] = v.len.value; break;
    case Prop::RadiusBL: s.radius[3] = v.len.value; break;
    case Prop::Color: s.color = v.color; break;
    case Prop::FontSize: s.fontSize = v.len.value; break;
    case Prop::TextAlign: s.textAlign = TextAlignment(v.e); break;
    case Prop::Opacity: s.opacity = std::clamp(v.num, 0.f, 1.f); break;
    case Prop::ShadowColor: s.shadowColor = v.color; break;
    case Prop::ShadowBlur: s.shadowBlur = v.len.value; break;
    case Prop::BackdropBlur: s.backdropBlur = v.len.value; break;
    case Prop::ShadowX: s.shadowX = v.len.value; break;
    case Prop::ShadowY: s.shadowY = v.len.value; break;
    case Prop::PointerEvents: s.pointerEvents = v.b; break;
    case Prop::FontWeight: s.fontWeight = u16(std::clamp(v.num, 1.f, 1000.f)); break;
    case Prop::FontStyle: s.fontItalic = v.e != 0; break;
    case Prop::FontFamily:
        s.fontFamily = {};
        for (u8 k = 0; k < v.trackCount && k < kMaxFontFamilies; ++k) s.fontFamily[k] = u32(v.tracks[k].value);
        break;
    case Prop::TextDecoration: s.textDecoration = v.e; break;
    case Prop::LetterSpacing: s.letterSpacing = v.len.value; break;
    case Prop::LineHeight: s.lineHeight = v.len; break;
    case Prop::Cursor: s.cursor = fuse::ui::Cursor(v.e < kCursorCount ? v.e : 0); break;
    case Prop::TextDirection: s.rtl = v.e != 0; break;
    case Prop::PaddingStart: s.paddingStart = v.len.value; break;
    case Prop::PaddingEnd: s.paddingEnd = v.len.value; break;
    case Prop::Transition: {
        if (v.e == kTransitionNone) { s.transitionCount = 0; s.transitions = {}; break; }
        const Transition t{Prop(v.e), Easing(std::min<u8>(v.trackCount, u8(Easing::Out))), std::max(0.f, v.num), std::max(0.f, v.len.value)};
        u8 k = 0;
        while (k < s.transitionCount && s.transitions[k].prop != t.prop) ++k;
        if (k == kMaxTransitions) break; // full: further entries are dropped
        s.transitions[k] = t;
        if (k == s.transitionCount) ++s.transitionCount;
        break;
    }
    case Prop::EnterAnimation: s.enterAnim = MotionKind(v.e); break;
    case Prop::EnterDuration: s.enterDuration = std::max(0.f, v.num); break;
    case Prop::ExitAnimation: s.exitAnim = MotionKind(v.e); break;
    case Prop::ExitDuration: s.exitDuration = std::max(0.f, v.num); break;
    case Prop::Position: s.position = PositionKind(std::min<u8>(v.e, u8(PositionKind::Sticky))); break;
    case Prop::ZIndex: s.zIndex = v.e ? kZAuto : toIndex(v.num, -1000000, 1000000); break;
    case Prop::AspectRatio: s.aspectRatio = std::clamp(finiteOr(v.num, 0.f), 0.f, 1.0e6f); break;
    case Prop::Transform: s.transform = transformOf(v); break;
    case Prop::TransformOriginX: s.originX = v.len; break;
    case Prop::TransformOriginY: s.originY = v.len; break;
    case Prop::Filter: s.filter = filterOf(v); break;
    case Prop::ClipPath: s.clipPath = clipShapeOf(v); break;
    case Prop::MaskImage: s.mask = gradientSpecOf(v); break;
    case Prop::MaskMode: s.maskLuminance = v.e != 0; break;
    case Prop::BackgroundImage: s.backgroundImage = gradientSpecOf(v); break;
    case Prop::AnimationName:
        s.animationCount = std::min<u8>(v.fCount, kMaxAnimations);
        for (u8 k = 0; k < kMaxAnimations; ++k) s.animations[k].name = k < s.animationCount ? u32(toIndex(v.f[k], 0, 0xFFFFFF)) : 0u;
        if (s.animationCount == 1 && s.animations[0].name == 0) s.animationCount = 0; // animation-name: none
        break;
    case Prop::AnimationDuration: case Prop::AnimationTimingFunction: case Prop::AnimationDelay: case Prop::AnimationIterationCount:
    case Prop::AnimationDirection: case Prop::AnimationFillMode: case Prop::AnimationPlayState:
        if (!v.fCount) break;
        for (u8 k = 0; k < kMaxAnimations; ++k) { // a shorter list repeats (CSS)
            const f32 x = v.f[k % std::min<u8>(v.fCount, kMaxAnimations)];
            AnimationSpec& a = s.animations[k];
            switch (p) {
            case Prop::AnimationDuration: a.duration = std::max(0.f, finiteOr(x, 0.f)); break;
            case Prop::AnimationTimingFunction: a.ease = Easing(toIndex(x, 0, int(Easing::Out))); break;
            case Prop::AnimationDelay: a.delay = finiteOr(x, 0.f); break;
            case Prop::AnimationIterationCount: a.iterations = x == x ? std::max(0.f, x) : 1.f; break; // infinity is a count
            case Prop::AnimationDirection: a.direction = AnimDirection(toIndex(x, 0, 3)); break;
            case Prop::AnimationFillMode: a.fill = AnimFill(toIndex(x, 0, 3)); break;
            default: a.paused = x != 0.f; break;
            }
        }
        break;
    case Prop::ContainerType: s.containerType = std::min<u8>(v.e, 2); break;
    case Prop::ContainerName: s.containerName = u32(toIndex(v.num, 0, 0xFFFFFF)); break;
    case Prop::Count: break;
    }
}

PropValue readProp(const ComputedStyle& s, Prop p) {
    const auto px = [](f32 v) { return PropValue::ofLength(Length::px(v)); };
    switch (p) {
    case Prop::Display: return PropValue::ofBool(s.visible);
    case Prop::Layout: return PropValue::ofEnum(u8(s.layout));
    case Prop::Direction: return PropValue::ofEnum(u8(s.direction));
    case Prop::Width: return PropValue::ofLength(s.width);
    case Prop::Height: return PropValue::ofLength(s.height);
    case Prop::MinWidth: return PropValue::ofLength(s.minWidth);
    case Prop::MinHeight: return PropValue::ofLength(s.minHeight);
    case Prop::MaxWidth: return PropValue::ofLength(s.maxWidth);
    case Prop::MaxHeight: return PropValue::ofLength(s.maxHeight);
    case Prop::Basis: return PropValue::ofLength(s.basis);
    case Prop::Grow: return PropValue::ofNumber(s.grow);
    case Prop::Shrink: return PropValue::ofNumber(s.shrink);
    case Prop::PaddingLeft: return px(s.padding.l);
    case Prop::PaddingTop: return px(s.padding.t);
    case Prop::PaddingRight: return px(s.padding.r);
    case Prop::PaddingBottom: return px(s.padding.b);
    case Prop::MarginLeft: return px(s.margin.l);
    case Prop::MarginTop: return px(s.margin.t);
    case Prop::MarginRight: return px(s.margin.r);
    case Prop::MarginBottom: return px(s.margin.b);
    case Prop::Gap: return px(s.gap);
    case Prop::AlignItems: return PropValue::ofEnum(u8(s.alignItems));
    case Prop::AlignSelf: return PropValue::ofEnum(u8(s.alignSelf));
    case Prop::Justify: return PropValue::ofEnum(u8(s.justify));
    case Prop::Wrap: return PropValue::ofBool(s.wrap);
    case Prop::Overflow: return PropValue::ofEnum(u8(s.overflow));
    case Prop::GridColumns: { PropValue v; v.type = PropType::Tracks; v.tracks = s.gridColumns; v.trackCount = s.gridColumnCount; v.b = s.subgrid; return v; }
    case Prop::GridColumn: return PropValue::ofNumber(f32(s.gridColumn));
    case Prop::GridColumnSpan: return PropValue::ofNumber(f32(s.gridColumnSpan));
    case Prop::GridRow: return PropValue::ofNumber(f32(s.gridRow));
    case Prop::GridRowSpan: return PropValue::ofNumber(f32(s.gridRowSpan));
    case Prop::Left: return PropValue::ofLength(s.left);
    case Prop::Top: return PropValue::ofLength(s.top);
    case Prop::Right: return PropValue::ofLength(s.right);
    case Prop::Bottom: return PropValue::ofLength(s.bottom);
    case Prop::CenterX: return PropValue::ofBool(s.centerX);
    case Prop::CenterY: return PropValue::ofBool(s.centerY);
    case Prop::Background: return PropValue::ofColor(s.background);
    case Prop::Background2: return PropValue::ofColor(s.background2);
    case Prop::Gradient: return PropValue::ofEnum(u8(s.gradient));
    case Prop::BorderColor: return PropValue::ofColor(s.borderColor);
    case Prop::BorderWidth: return px(s.borderWidth);
    case Prop::RadiusTL: return px(s.radius[0]);
    case Prop::RadiusTR: return px(s.radius[1]);
    case Prop::RadiusBR: return px(s.radius[2]);
    case Prop::RadiusBL: return px(s.radius[3]);
    case Prop::Color: return PropValue::ofColor(s.color);
    case Prop::FontSize: return px(s.fontSize);
    case Prop::TextAlign: return PropValue::ofEnum(u8(s.textAlign));
    case Prop::FontWeight: return PropValue::ofNumber(f32(s.fontWeight));
    case Prop::FontStyle: return PropValue::ofEnum(s.fontItalic ? 1 : 0);
    case Prop::TextDecoration: return PropValue::ofEnum(s.textDecoration);
    case Prop::LetterSpacing: return PropValue::ofLength(Length::px(s.letterSpacing));
    case Prop::LineHeight: return PropValue::ofLength(s.lineHeight);
    case Prop::FontFamily: {
        PropValue v;
        v.type = PropType::Tracks;
        for (u32 h : s.fontFamily)
            if (h) v.tracks[v.trackCount++] = Length::px(f32(h));
        return v;
    }
    case Prop::Cursor: return PropValue::ofEnum(u8(s.cursor));
    case Prop::Opacity: return PropValue::ofNumber(s.opacity);
    case Prop::ShadowColor: return PropValue::ofColor(s.shadowColor);
    case Prop::ShadowBlur: return px(s.shadowBlur);
    case Prop::ShadowX: return px(s.shadowX);
    case Prop::ShadowY: return px(s.shadowY);
    case Prop::PointerEvents: return PropValue::ofBool(s.pointerEvents);
    case Prop::BackdropBlur: return px(s.backdropBlur);
    case Prop::TextDirection: return PropValue::ofEnum(s.rtl ? 1 : 0);
    case Prop::PaddingStart: return px(s.paddingStart);
    case Prop::PaddingEnd: return px(s.paddingEnd);
    case Prop::EnterAnimation: return PropValue::ofEnum(u8(s.enterAnim));
    case Prop::EnterDuration: return PropValue::ofNumber(s.enterDuration);
    case Prop::ExitAnimation: return PropValue::ofEnum(u8(s.exitAnim));
    case Prop::ExitDuration: return PropValue::ofNumber(s.exitDuration);
    case Prop::Transition: // the first entry (the whole list is ComputedStyle::transitions)
        if (s.transitionCount) return PropValue::ofTransition(s.transitions[0].prop, s.transitions[0].duration, s.transitions[0].ease, s.transitions[0].delay);
        break;
    case Prop::Position: return PropValue::ofEnum(u8(s.position));
    case Prop::ZIndex: { PropValue v = PropValue::ofNumber(s.zIndex == kZAuto ? 0.f : f32(s.zIndex)); v.e = s.zIndex == kZAuto; return v; }
    case Prop::AspectRatio: return PropValue::ofNumber(s.aspectRatio);
    case Prop::Transform: return transformValue(s.transform);
    case Prop::TransformOriginX: return PropValue::ofLength(s.originX);
    case Prop::TransformOriginY: return PropValue::ofLength(s.originY);
    case Prop::Filter: return filterValue(s.filter);
    case Prop::ClipPath: return clipShapeValue(s.clipPath);
    case Prop::MaskImage: return gradientSpecValue(s.mask);
    case Prop::MaskMode: return PropValue::ofEnum(s.maskLuminance ? 1 : 0);
    case Prop::BackgroundImage: return gradientSpecValue(s.backgroundImage);
    case Prop::AnimationName: case Prop::AnimationDuration: case Prop::AnimationTimingFunction: case Prop::AnimationDelay:
    case Prop::AnimationIterationCount: case Prop::AnimationDirection: case Prop::AnimationFillMode: case Prop::AnimationPlayState: {
        PropValue v;
        v.type = PropType::Raw;
        v.fCount = s.animationCount;
        for (u8 k = 0; k < s.animationCount; ++k) {
            const AnimationSpec& a = s.animations[k];
            v.f[k] = p == Prop::AnimationName ? f32(a.name) : p == Prop::AnimationDuration ? a.duration
                   : p == Prop::AnimationTimingFunction ? f32(u8(a.ease)) : p == Prop::AnimationDelay ? a.delay
                   : p == Prop::AnimationIterationCount ? a.iterations : p == Prop::AnimationDirection ? f32(u8(a.direction))
                   : p == Prop::AnimationFillMode ? f32(u8(a.fill)) : (a.paused ? 1.f : 0.f);
        }
        return v;
    }
    case Prop::ContainerType: return PropValue::ofEnum(s.containerType);
    case Prop::ContainerName: return PropValue::ofNumber(f32(s.containerName));
    case Prop::Count: break;
    }
    PropValue v;
    v.type = propInfo(p).type;
    return v;
}

std::string_view cursorCssName(Cursor c) {
    static constexpr std::string_view kNames[kCursorCount] = {"auto", "default", "text", "pointer", "ew-resize", "ns-resize", "nwse-resize",
                                                              "nesw-resize", "move", "crosshair", "not-allowed", "wait", "none"};
    return kNames[u8(c) < kCursorCount ? u8(c) : 0];
}

// ---- Motion helpers ------------------------------------------------------------------------------------------------
f32 applyEasing(Easing e, f32 k) {
    switch (e) {
    case Easing::Linear: break;
    case Easing::InOutCubic: k = k < 0.5f ? 4.f * k * k * k : 1.f - std::pow(-2.f * k + 2.f, 3.f) * 0.5f; break;
    case Easing::OutBack: { const f32 c1 = 1.70158f, c3 = c1 + 1.f; k = 1.f + c3 * std::pow(k - 1.f, 3.f) + c1 * std::pow(k - 1.f, 2.f); break; }
    case Easing::In: k = k * k * k; break;
    case Easing::Out: k = 1.f - std::pow(1.f - k, 3.f); break;
    }
    return k;
}

const Transition* findTransition(const ComputedStyle& s, Prop p) {
    const Transition* all = nullptr;
    for (u8 k = 0; k < s.transitionCount; ++k) {
        if (s.transitions[k].prop == p) return &s.transitions[k];
        if (s.transitions[k].prop == kTransitionAll) all = &s.transitions[k];
    }
    return all;
}

namespace {
/// The field a property animates (one float, one Length, one Colour, a box edge or a corner); none: not interpolated.
struct PropField {
    f32 ComputedStyle::*num = nullptr;
    Length ComputedStyle::*len = nullptr;
    Color ComputedStyle::*col = nullptr;
    int edge = -1;   ///< 0..3 padding l t r b, 4..7 margin
    int radius = -1; ///< corner index
};
PropField propField(Prop p) {
    PropField f;
    switch (p) {
    case Prop::Width: f.len = &ComputedStyle::width; break;
    case Prop::Height: f.len = &ComputedStyle::height; break;
    case Prop::MinWidth: f.len = &ComputedStyle::minWidth; break;
    case Prop::MinHeight: f.len = &ComputedStyle::minHeight; break;
    case Prop::MaxWidth: f.len = &ComputedStyle::maxWidth; break;
    case Prop::MaxHeight: f.len = &ComputedStyle::maxHeight; break;
    case Prop::Basis: f.len = &ComputedStyle::basis; break;
    case Prop::Left: f.len = &ComputedStyle::left; break;
    case Prop::Top: f.len = &ComputedStyle::top; break;
    case Prop::Right: f.len = &ComputedStyle::right; break;
    case Prop::Bottom: f.len = &ComputedStyle::bottom; break;
    case Prop::TransformOriginX: f.len = &ComputedStyle::originX; break;
    case Prop::TransformOriginY: f.len = &ComputedStyle::originY; break;
    case Prop::Grow: f.num = &ComputedStyle::grow; break;
    case Prop::Shrink: f.num = &ComputedStyle::shrink; break;
    case Prop::Gap: f.num = &ComputedStyle::gap; break;
    case Prop::BorderWidth: f.num = &ComputedStyle::borderWidth; break;
    case Prop::FontSize: f.num = &ComputedStyle::fontSize; break;
    case Prop::Opacity: f.num = &ComputedStyle::opacity; break;
    case Prop::ShadowBlur: f.num = &ComputedStyle::shadowBlur; break;
    case Prop::ShadowX: f.num = &ComputedStyle::shadowX; break;
    case Prop::ShadowY: f.num = &ComputedStyle::shadowY; break;
    case Prop::BackdropBlur: f.num = &ComputedStyle::backdropBlur; break;
    case Prop::LetterSpacing: f.num = &ComputedStyle::letterSpacing; break;
    case Prop::Background: f.col = &ComputedStyle::background; break;
    case Prop::Background2: f.col = &ComputedStyle::background2; break;
    case Prop::BorderColor: f.col = &ComputedStyle::borderColor; break;
    case Prop::Color: f.col = &ComputedStyle::color; break;
    case Prop::ShadowColor: f.col = &ComputedStyle::shadowColor; break;
    case Prop::PaddingLeft: f.edge = 0; break;
    case Prop::PaddingTop: f.edge = 1; break;
    case Prop::PaddingRight: f.edge = 2; break;
    case Prop::PaddingBottom: f.edge = 3; break;
    case Prop::MarginLeft: f.edge = 4; break;
    case Prop::MarginTop: f.edge = 5; break;
    case Prop::MarginRight: f.edge = 6; break;
    case Prop::MarginBottom: f.edge = 7; break;
    case Prop::RadiusTL: f.radius = 0; break;
    case Prop::RadiusTR: f.radius = 1; break;
    case Prop::RadiusBR: f.radius = 2; break;
    case Prop::RadiusBL: f.radius = 3; break;
    default: break;
    }
    return f;
}
f32 edgeOf(const ComputedStyle& s, int e) {
    const Edges& g = e < 4 ? s.padding : s.margin;
    switch (e & 3) { case 0: return g.l; case 1: return g.t; case 2: return g.r; default: return g.b; }
}
void setEdge(ComputedStyle& s, int e, f32 v) {
    Edges& g = e < 4 ? s.padding : s.margin;
    switch (e & 3) { case 0: g.l = v; break; case 1: g.t = v; break; case 2: g.r = v; break; default: g.b = v; break; }
}
f32 mixf(f32 a, f32 b, f32 k) { return a + (b - a) * k; }
} // namespace

bool sameStyleProp(const ComputedStyle& a, const ComputedStyle& b, Prop p) {
    const PropField f = propField(p);
    if (f.num) return a.*f.num == b.*f.num;
    if (f.len) return a.*f.len == b.*f.len;
    if (f.col) return a.*f.col == b.*f.col;
    if (f.edge >= 0) return edgeOf(a, f.edge) == edgeOf(b, f.edge);
    if (f.radius >= 0) return a.radius[std::size_t(f.radius)] == b.radius[std::size_t(f.radius)];
    switch (p) { // not interpolated (lerpStyleProp switches to the end value)
    case Prop::FontWeight: return a.fontWeight == b.fontWeight;
    case Prop::FontStyle: return a.fontItalic == b.fontItalic;
    case Prop::FontFamily: return a.fontFamily == b.fontFamily;
    case Prop::TextDecoration: return a.textDecoration == b.textDecoration;
    case Prop::LineHeight: return a.lineHeight == b.lineHeight;
    case Prop::Cursor: return a.cursor == b.cursor;
    case Prop::Transform: return a.transform == b.transform;
    case Prop::Filter: return a.filter == b.filter;
    default: break;
    }
    return true;
}

void lerpStyleProp(ComputedStyle& out, const ComputedStyle& a, const ComputedStyle& b, Prop p, f32 k) {
    if (p == Prop::Transform) { out.transform = lerpTransform(a.transform, b.transform, k); return; }
    if (p == Prop::Filter) { out.filter = lerpFilter(a.filter, b.filter, k); return; }
    const PropField f = propField(p);
    if (f.num) {
        f32 v = mixf(a.*f.num, b.*f.num, k);
        if (p == Prop::Opacity) v = std::clamp(v, 0.f, 1.f);
        else if (p != Prop::ShadowX && p != Prop::ShadowY && p != Prop::LetterSpacing) v = std::max(0.f, v);
        out.*f.num = v;
    } else if (f.len) {
        const Length la = a.*f.len, lb = b.*f.len;
        out.*f.len = (la.unit == lb.unit && la.unit != Unit::Auto) ? Length{mixf(la.value, lb.value, k), lb.unit} : lb;
    } else if (f.col) {
        // premultiplied blend: a fade from / to transparent keeps the visible colour's hue
        const Color ca = a.*f.col, cb = b.*f.col;
        const f32 al = std::clamp(mixf(ca.a, cb.a, k), 0.f, 1.f);
        Color c{mixf(ca.r * ca.a, cb.r * cb.a, k), mixf(ca.g * ca.a, cb.g * cb.a, k), mixf(ca.b * ca.a, cb.b * cb.a, k), al};
        if (al > 1e-5f) { c.r = std::clamp(c.r / al, 0.f, 1.f); c.g = std::clamp(c.g / al, 0.f, 1.f); c.b = std::clamp(c.b / al, 0.f, 1.f); }
        else c = Color{cb.r, cb.g, cb.b, 0.f};
        out.*f.col = c;
    } else if (f.edge >= 0) {
        const f32 v = mixf(edgeOf(a, f.edge), edgeOf(b, f.edge), k);
        setEdge(out, f.edge, f.edge < 4 ? std::max(0.f, v) : v);
    } else if (f.radius >= 0) {
        const std::size_t r = std::size_t(f.radius);
        out.radius[r] = std::max(0.f, mixf(a.radius[r], b.radius[r], k));
    } else {
        applyProp(out, p, readProp(b, p));
    }
}

void inheritFrom(ComputedStyle& s, const ComputedStyle& parent) {
    s.color = parent.color;
    s.fontSize = parent.fontSize;
    s.textAlign = parent.textAlign;
    s.fontWeight = parent.fontWeight;
    s.fontItalic = parent.fontItalic;
    s.fontFamily = parent.fontFamily;
    s.letterSpacing = parent.letterSpacing;
    s.lineHeight = parent.lineHeight;
    s.rtl = parent.rtl;
}

bool StyleSheet::token(u32 h, std::string_view& out) const {
    for (auto it = m_tokens.rbegin(); it != m_tokens.rend(); ++it)
        if (it->first == h) { out = it->second; return true; }
    return false;
}

// ---- Engine --------------------------------------------------------------------------------------------------------
StyleEngine::StyleEngine() {
    m_table.reserve(256);
    m_isScratch.reserve(256);
    m_freeScratch.reserve(32);
    m_hashIndex.assign(1024, 0);
    m_scratch.reserve(64);
    m_localTokens.reserve(16);
    intern(ComputedStyle{});
}

void StyleEngine::clearSheets() {
    m_sheets.clear();
    rebuild();
    ++m_version;
}

void StyleEngine::addSheet(StyleSheet sheet) {
    m_sheets.push_back(std::move(sheet));
    rebuild();
    ++m_version;
}

// ---- Conditions (@media, @container) -------------------------------------------------------------------------------
bool evalQuery(const std::vector<QueryTerm>& program, const MediaEnv& env) {
    bool stack[32];
    int n = 0;
    for (const QueryTerm& t : program) {
        switch (t.op) {
        case QueryTerm::Op::True:
        case QueryTerm::Op::False:
            if (n < 32) stack[n++] = t.op == QueryTerm::Op::True;
            break;
        case QueryTerm::Op::Not:
            if (n >= 1) stack[n - 1] = !stack[n - 1];
            break;
        case QueryTerm::Op::And:
        case QueryTerm::Op::Or:
            if (n >= 2) {
                stack[n - 2] = t.op == QueryTerm::Op::And ? (stack[n - 2] && stack[n - 1]) : (stack[n - 2] || stack[n - 1]);
                --n;
            }
            break;
        case QueryTerm::Op::Feature: {
            f32 v = 0.f;
            switch (t.feature) {
            case QueryTerm::Feature::Width: v = env.width; break;
            case QueryTerm::Feature::Height: v = env.height; break;
            case QueryTerm::Feature::AspectRatio: v = env.height > 0.f ? env.width / env.height : 0.f; break;
            case QueryTerm::Feature::Orientation: v = env.width > env.height ? 1.f : 0.f; break;
            case QueryTerm::Feature::PrefersDark: v = env.dark ? 1.f : 0.f; break;
            case QueryTerm::Feature::PrefersReducedMotion: v = env.reducedMotion ? 1.f : 0.f; break;
            case QueryTerm::Feature::Hover: v = env.hover ? 1.f : 0.f; break;
            case QueryTerm::Feature::Pointer: v = f32(env.pointer); break;
            case QueryTerm::Feature::Resolution: v = env.resolution; break;
            }
            bool r = false;
            switch (t.cmp) {
            case QueryTerm::Cmp::Truthy: r = v != 0.f; break;
            case QueryTerm::Cmp::Eq: r = std::abs(v - t.value) < 1e-4f; break;
            case QueryTerm::Cmp::Lt: r = v < t.value; break;
            case QueryTerm::Cmp::Le: r = v <= t.value; break;
            case QueryTerm::Cmp::Gt: r = v > t.value; break;
            case QueryTerm::Cmp::Ge: r = v >= t.value; break;
            }
            if (n < 32) stack[n++] = r;
            break;
        }
        }
    }
    return n > 0 && stack[n - 1];
}

namespace {
/// Per condition of a sheet: do the @media blocks it sits in hold? (@container blocks are decided per node.)
void evalMediaChain(const StyleSheet& sheet, const MediaEnv& env, std::vector<u8>& out) {
    const auto& conds = sheet.conditions();
    out.assign(conds.size(), 1);
    for (std::size_t i = 0; i < conds.size(); ++i) { // parents come first
        const StyleCondition& c = conds[i];
        bool ok = c.parent < 0 || (std::size_t(c.parent) < i && out[std::size_t(c.parent)]);
        if (ok && c.kind == StyleCondition::Kind::Media) ok = evalQuery(c.program, env);
        out[i] = ok ? 1 : 0;
    }
}
} // namespace

void StyleEngine::setMediaEnv(const MediaEnv& env) {
    if (env == m_media) return;
    m_media = env;
    std::vector<u8> now;
    bool flipped = false;
    for (std::size_t i = 0; i < m_sheets.size() && !flipped; ++i) {
        if (m_sheets[i].conditions().empty()) continue;
        evalMediaChain(m_sheets[i], env, now);
        flipped = i >= m_condOk.size() || now != m_condOk[i];
    }
    if (!flipped) return;
    rebuild();
    ++m_version;
}

bool StyleEngine::conditionHolds(const StyleSubject& s, u32 node, const Ordered& o) const {
    const auto& conds = o.sheet->conditions();
    for (i32 c = o.rule->cond; c >= 0 && std::size_t(c) < conds.size(); c = conds[std::size_t(c)].parent) {
        const StyleCondition& sc = conds[std::size_t(c)];
        if (sc.kind != StyleCondition::Kind::Container) continue;
        Size size;
        if (!s.containerSize(node, sc.containerName, sc.needsHeight, size)) return false;
        MediaEnv env = m_media;
        env.width = size.w;
        env.height = size.h;
        if (!evalQuery(sc.program, env)) return false;
    }
    return true;
}

// ---- Rule order, features, invalidation keys -----------------------------------------------------------------------
void StyleEngine::addKey(u8 kind, u32 key, StyleInvalidation inv) {
    if (!inv.descend && !inv.siblings && !inv.has) return;
    for (InvalKey& k : m_keys)
        if (k.kind == kind && k.key == key) {
            k.inv.descend = std::max(k.inv.descend, inv.descend);
            k.inv.siblings = k.inv.siblings || inv.siblings;
            k.inv.has = k.inv.has || inv.has;
            return;
        }
    m_keys.push_back({key, kind, inv});
}

void StyleEngine::collectKeys(const StyleRule& r, const SimpleSelector& sel, StyleInvalidation inv, bool inHas) {
    for (u8 k = 0; k < sel.classCount; ++k) addKey(0, sel.classes[k], inv);
    for (u32 bit = 1; bit && bit <= sel.states; bit <<= 1)
        if (sel.states & bit) addKey(1, bit, inv);
    for (u32 pi = sel.firstPart; pi < u32(sel.firstPart) + sel.partCount && pi < r.parts.size(); ++pi) {
        const SelectorPart& part = r.parts[pi];
        switch (part.kind) {
        case SelectorPart::Kind::Attr: addKey(2, part.name, inv); break;
        case SelectorPart::Kind::Empty: m_features |= kStyleStructural | kStyleEmptyText; break;
        case SelectorPart::Kind::NthChild:
        case SelectorPart::Kind::NthLastChild: m_features |= kStyleStructural; break;
        case SelectorPart::Kind::Not:
        case SelectorPart::Kind::Is:
        case SelectorPart::Kind::Where:
        case SelectorPart::Kind::Has: {
            StyleInvalidation in = inv;
            bool has = inHas;
            if (part.kind == SelectorPart::Kind::Has) {
                in.has = true;
                has = true;
                m_features |= kStyleHas;
            }
            for (u32 li = part.firstList; li < part.firstList + part.listCount && li < r.lists.size(); ++li) {
                const SelectorRange& range = r.lists[li];
                for (u32 ci = range.first; ci < range.first + range.count && ci < r.sub.size(); ++ci) {
                    StyleInvalidation x = in;
                    if (ci + 1 < range.first + range.count) { // left of the argument's subject: any depth, any sibling
                        x.descend = 255;
                        x.siblings = true;
                        if (!has) m_features |= kStyleStructDeep;
                    }
                    const auto comb = r.sub[ci].combinator;
                    if (comb == SimpleSelector::Combinator::NextSibling || comb == SimpleSelector::Combinator::LaterSibling)
                        m_features |= kStyleSibling | (has ? 0u : u32(kStyleStructDeep));
                    collectKeys(r, r.sub[ci], x, has);
                }
            }
            break;
        }
        default: break;
        }
    }
}

void StyleEngine::rebuild() {
    m_ordered.clear();
    m_keys.clear();
    m_features = 0;
    m_condOk.resize(m_sheets.size());
    // cascade layers: names merge across sheets in order of first appearance; a layer's own rules come after its
    // sub-layers; rules outside any layer come last (and so win)
    // (an anonymous layer, marked by a \x01 in its name, belongs to its sheet alone)
    auto layerKey = [](const std::string& l, u32 sheet) {
        if (l.find('\x01') == std::string::npos) return l;
        std::string key;
        for (char c : l) {
            key.push_back(c);
            if (c == '\x01') key += std::to_string(sheet) + ":";
        }
        return key;
    };
    std::vector<std::string> names;
    for (u32 si = 0; si < m_sheets.size(); ++si)
        for (const std::string& l : m_sheets[si].m_layers)
            if (const std::string key = layerKey(l, si); std::find(names.begin(), names.end(), key) == names.end()) names.push_back(key);
    auto indexOf = [&](std::string_view n) {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == n) return u32(i);
        return u32(names.size());
    };
    std::vector<std::vector<u32>> paths(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string_view n = names[i];
        for (std::size_t dot = n.find('.');; dot = n.find('.', dot + 1)) { // (a prefix nothing declared sorts last)
            paths[i].push_back(indexOf(n.substr(0, dot)));
            if (dot == std::string_view::npos) break;
        }
        paths[i].push_back(0xFFFFFFFFu); // own rules after the sub-layers
    }
    std::vector<u32> byRank(names.size());
    for (u32 i = 0; i < byRank.size(); ++i) byRank[i] = i;
    std::sort(byRank.begin(), byRank.end(), [&](u32 a, u32 b) { return paths[a] < paths[b]; });
    std::vector<u32> rank(names.size());
    for (u32 i = 0; i < byRank.size(); ++i) rank[byRank[i]] = i;

    struct Keyed {
        u32 layer, specificity, sheet, order;
        Ordered o;
    };
    std::vector<Keyed> keyed;
    u32 sheetIdx = 0;
    for (const auto& sh : m_sheets) {
        evalMediaChain(sh, m_media, m_condOk[sheetIdx]);
        for (const StyleCondition& c : sh.m_conditions)
            if (c.kind == StyleCondition::Kind::Container) m_features |= kStyleContainer;
        for (const auto& r : sh.m_rules) {
            if (r.cond >= 0 && (std::size_t(r.cond) >= m_condOk[sheetIdx].size() || !m_condOk[sheetIdx][std::size_t(r.cond)])) continue;
            const u32 layer = r.layer && std::size_t(r.layer - 1) < sh.m_layers.size() ? rank[indexOf(layerKey(sh.m_layers[r.layer - 1u], sheetIdx))] : 0xFFFFFFFFu;
            keyed.push_back({layer, r.specificity, sheetIdx, r.order, {&r, &sh}});
            // what a change left of the subject reaches
            if (r.chain.size() > 1) m_features |= kStyleDescend;
            StyleInvalidation inv;
            for (std::size_t k = r.chain.size(); k-- > 0;) {
                const bool subject = k + 1 == r.chain.size();
                collectKeys(r, r.chain[k], inv, false);
                if (!subject) {
                    for (u32 pi = r.chain[k].firstPart; pi < u32(r.chain[k].firstPart) + r.chain[k].partCount && pi < r.parts.size(); ++pi) {
                        const auto kind = r.parts[pi].kind;
                        if (kind == SelectorPart::Kind::Has) m_features |= kStyleHasDeep;
                        if (kind == SelectorPart::Kind::Empty || kind == SelectorPart::Kind::NthChild || kind == SelectorPart::Kind::NthLastChild)
                            m_features |= kStyleStructDeep;
                    }
                }
                switch (r.chain[k].combinator) { // the hop from chain[k - 1] to chain[k]
                case SimpleSelector::Combinator::Child:
                    if (inv.descend < 254) ++inv.descend;
                    break;
                case SimpleSelector::Combinator::NextSibling:
                case SimpleSelector::Combinator::LaterSibling:
                    inv.siblings = true;
                    m_features |= kStyleSibling;
                    if (!subject) m_features |= kStyleStructDeep;
                    break;
                default: inv.descend = 255; break;
                }
            }
            if (r.scoped) { // the scope root / limit selectors sit above the subject at any depth
                StyleInvalidation any;
                any.descend = 255;
                any.siblings = true;
                for (u32 li : {r.scopeFirst, r.limitFirst}) {
                    const u32 count = li == r.scopeFirst ? r.scopeCount : r.limitCount;
                    for (u32 x = li; x < li + count && x < r.lists.size(); ++x)
                        for (u32 ci = r.lists[x].first; ci < r.lists[x].first + r.lists[x].count && ci < r.sub.size(); ++ci)
                            collectKeys(r, r.sub[ci], any, false);
                }
                m_features |= kStyleDescend;
            }
        }
        ++sheetIdx;
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.specificity != b.specificity) return a.specificity < b.specificity;
        if (a.sheet != b.sheet) return a.sheet < b.sheet;
        return a.order < b.order;
    });
    for (const Keyed& k : keyed) m_ordered.push_back(k.o);
}

StyleInvalidation StyleEngine::lookupInvalidation(u8 kind, u32 key) const {
    for (const InvalKey& k : m_keys)
        if (k.kind == kind && k.key == key) return k.inv;
    return {};
}
StyleInvalidation StyleEngine::stateInvalidation(u32 bits) const {
    StyleInvalidation out;
    if (m_keys.empty()) return out;
    for (const InvalKey& k : m_keys)
        if (k.kind == 1 && (k.key & bits)) {
            out.descend = std::max(out.descend, k.inv.descend);
            out.siblings = out.siblings || k.inv.siblings;
            out.has = out.has || k.inv.has;
        }
    return out;
}

const KeyframesRule* StyleEngine::findKeyframes(u32 name) const {
    for (auto sh = m_sheets.rbegin(); sh != m_sheets.rend(); ++sh)
        for (auto k = sh->m_keyframes.rbegin(); k != sh->m_keyframes.rend(); ++k)
            if (k->name == name) return &*k;
    return nullptr;
}

bool StyleEngine::resolveDeclaration(const Declaration& d, PropValue& out) {
    if (d.prop == Prop::Count) return false;
    if (d.raw.empty()) { out = d.value; return true; }
    m_localTokens.clear();
    return resolveVar(d.raw, d.prop, out, 0);
}

// ---- Matching ------------------------------------------------------------------------------------------------------
struct StyleEngine::MatchCtx {
    const StyleSubject& s;
    const StyleRule& r;
    u32 scope; ///< the @scope root the rule is matched under (kNone: none)
};
namespace {
constexpr u32 kNone = 0xFFFFFFFFu;
char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
bool equalText(std::string_view a, std::string_view b, bool noCase) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (noCase ? lower(a[i]) != lower(b[i]) : a[i] != b[i]) return false;
    return true;
}
bool attrMatches(AttrOp op, std::string_view have, std::string_view want, bool noCase) {
    switch (op) {
    case AttrOp::Exists: return true;
    case AttrOp::Equals: return equalText(have, want, noCase);
    case AttrOp::Includes: // one of the whitespace separated words
        if (want.empty()) return false;
        for (std::size_t i = 0; i < have.size();) {
            while (i < have.size() && isSpace(have[i])) ++i;
            std::size_t e = i;
            while (e < have.size() && !isSpace(have[e])) ++e;
            if (e > i && equalText(have.substr(i, e - i), want, noCase)) return true;
            i = e;
        }
        return false;
    case AttrOp::DashMatch:
        return equalText(have, want, noCase) ||
               (have.size() > want.size() && have[want.size()] == '-' && equalText(have.substr(0, want.size()), want, noCase));
    case AttrOp::Prefix: return !want.empty() && have.size() >= want.size() && equalText(have.substr(0, want.size()), want, noCase);
    case AttrOp::Suffix:
        return !want.empty() && have.size() >= want.size() && equalText(have.substr(have.size() - want.size()), want, noCase);
    case AttrOp::Substring:
        if (want.empty()) return false;
        for (std::size_t i = 0; i + want.size() <= have.size(); ++i)
            if (equalText(have.substr(i, want.size()), want, noCase)) return true;
        return false;
    }
    return false;
}
bool nthMatches(i32 a, i32 b, i32 index) {
    if (a == 0) return index == b;
    const i32 d = index - b;
    return d % a == 0 && d / a >= 0;
}
} // namespace

bool StyleEngine::matchSimple(const StyleSubject& s, u32 node, const SimpleSelector& sel) const {
    if (sel.typeHash && s.typeHash(node) != sel.typeHash) return false;
    if (sel.idHash && s.idHash(node) != sel.idHash) return false;
    for (u8 k = 0; k < sel.classCount; ++k)
        if (!s.hasClass(node, sel.classes[k])) return false;
    if ((s.states(node) & sel.states) != sel.states) return false;
    return true;
}

bool StyleEngine::matchList(const MatchCtx& c, u32 node, u32 firstList, u32 listCount) const {
    for (u32 li = firstList; li < firstList + listCount && li < c.r.lists.size(); ++li) {
        const SelectorRange& range = c.r.lists[li];
        if (range.count == 0 || std::size_t(range.first) + range.count > c.r.sub.size()) continue;
        if (matchChain(c, node, c.r.sub.data() + range.first, int(range.count) - 1, kNone, range.lead)) return true;
    }
    return false;
}

bool StyleEngine::matchHas(const MatchCtx& c, u32 anchor, const SelectorRange& range) const {
    if (range.count == 0 || std::size_t(range.first) + range.count > c.r.sub.size()) return false;
    const SimpleSelector* chain = c.r.sub.data() + range.first;
    const int last = int(range.count) - 1;
    using Comb = SimpleSelector::Combinator;
    const bool sideways = range.lead == Comb::NextSibling || range.lead == Comb::LaterSibling;
    // candidates for the argument's subject: the anchor's descendants, or its later siblings and what is below them
    auto subtree = [&](u32 top, bool includeTop, bool childrenOnly) {
        if (includeTop && matchChain(c, top, chain, last, anchor, range.lead)) return true;
        if (includeTop && last == 0) return false; // one compound right of `+` / `~`: the sibling itself
        u32 n = c.s.firstChild(top);
        while (n != kNone) {
            if (matchChain(c, n, chain, last, anchor, range.lead)) return true;
            u32 next = childrenOnly ? kNone : c.s.firstChild(n);
            if (next == kNone) {
                next = c.s.nextSibling(n);
                while (next == kNone) {
                    n = c.s.parent(n);
                    if (n == kNone || n == top) return false;
                    next = c.s.nextSibling(n);
                }
            }
            n = next;
        }
        return false;
    };
    if (!sideways) return subtree(anchor, false, range.lead == Comb::Child && last == 0);
    for (u32 sib = c.s.nextSibling(anchor); sib != kNone; sib = c.s.nextSibling(sib)) {
        if (subtree(sib, true, false)) return true;
        if (range.lead == Comb::NextSibling && last == 0) break;
    }
    return false;
}

bool StyleEngine::matchCompound(const MatchCtx& c, u32 node, const SimpleSelector& sel) const {
    if (!matchSimple(c.s, node, sel)) return false;
    if (!sel.partCount) return true;
    static constexpr u32 kClassAttr = hashId("class");
    for (u32 pi = sel.firstPart; pi < u32(sel.firstPart) + sel.partCount; ++pi) {
        if (pi >= c.r.parts.size()) return false;
        const SelectorPart& part = c.r.parts[pi];
        switch (part.kind) {
        case SelectorPart::Kind::Attr: {
            const std::string_view want = std::size_t(part.text) + part.textLen <= c.r.strings.size()
                                              ? std::string_view(c.r.strings).substr(part.text, part.textLen) : std::string_view{};
            if (part.name == kClassAttr && part.attrOp == AttrOp::Includes && !part.noCase) {
                if (want.empty() || !c.s.hasClass(node, hashId(want))) return false;
                break;
            }
            std::string_view have;
            if (!c.s.attribute(node, part.name, have) || !attrMatches(part.attrOp, have, want, part.noCase)) return false;
            break;
        }
        case SelectorPart::Kind::NthChild:
        case SelectorPart::Kind::NthLastChild: {
            i32 index = 1;
            const bool fromEnd = part.kind == SelectorPart::Kind::NthLastChild;
            for (u32 k = fromEnd ? c.s.nextSibling(node) : c.s.prevSibling(node); k != kNone;
                 k = fromEnd ? c.s.nextSibling(k) : c.s.prevSibling(k))
                ++index;
            if (!nthMatches(part.a, part.b, index)) return false;
            break;
        }
        case SelectorPart::Kind::Empty:
            if (!c.s.empty(node)) return false;
            break;
        case SelectorPart::Kind::Not:
            if (matchList(c, node, part.firstList, part.listCount)) return false;
            break;
        case SelectorPart::Kind::Is:
        case SelectorPart::Kind::Where:
            if (!matchList(c, node, part.firstList, part.listCount)) return false;
            break;
        case SelectorPart::Kind::Has: {
            bool any = false;
            for (u32 li = part.firstList; li < part.firstList + part.listCount && li < c.r.lists.size() && !any; ++li)
                any = matchHas(c, node, c.r.lists[li]);
            if (!any) return false;
            break;
        }
        case SelectorPart::Kind::Scope:
            if (c.scope != kNone ? node != c.scope : c.s.parent(node) != kNone) return false;
            break;
        case SelectorPart::Kind::Root:
            if (c.s.parent(node) != kNone) return false;
            break;
        }
    }
    return true;
}

// Matches chain[0..k] ending at `node` (right to left, backtracking over descendant / later-sibling hops). With an
// `anchor` the leftmost compound must also stand in relation `lead` to it (the relative selectors of :has()).
bool StyleEngine::matchChain(const MatchCtx& c, u32 node, const SimpleSelector* chain, int k, u32 anchor,
                             SimpleSelector::Combinator lead) const {
    using Comb = SimpleSelector::Combinator;
    if (!matchCompound(c, node, chain[k])) return false;
    if (k == 0) {
        if (anchor == kNone) return true;
        switch (lead) {
        case Comb::Child: return c.s.parent(node) == anchor;
        case Comb::NextSibling: return c.s.prevSibling(node) == anchor;
        case Comb::LaterSibling:
            for (u32 p = c.s.prevSibling(node); p != kNone; p = c.s.prevSibling(p))
                if (p == anchor) return true;
            return false;
        default:
            for (u32 p = c.s.parent(node); p != kNone; p = c.s.parent(p))
                if (p == anchor) return true;
            return false;
        }
    }
    switch (chain[k].combinator) {
    case Comb::Child: {
        const u32 p = c.s.parent(node);
        return p != kNone && matchChain(c, p, chain, k - 1, anchor, lead);
    }
    case Comb::NextSibling: {
        const u32 p = c.s.prevSibling(node);
        return p != kNone && matchChain(c, p, chain, k - 1, anchor, lead);
    }
    case Comb::LaterSibling:
        for (u32 p = c.s.prevSibling(node); p != kNone; p = c.s.prevSibling(p))
            if (matchChain(c, p, chain, k - 1, anchor, lead)) return true;
        return false;
    default:
        for (u32 p = c.s.parent(node); p != kNone; p = c.s.parent(p))
            if (matchChain(c, p, chain, k - 1, anchor, lead)) return true;
        return false;
    }
}

bool StyleEngine::matches(const StyleSubject& s, u32 node, const StyleRule& rule) const {
    if (rule.root) return s.parent(node) == kNone;
    if (rule.chain.empty()) return false;
    const int last = int(rule.chain.size()) - 1;
    if (!matchSimple(s, node, rule.chain[std::size_t(last)])) return false; // the common rejection, before anything else
    if (!rule.scoped) return matchChain({s, rule, kNone}, node, rule.chain.data(), last, kNone, SimpleSelector::Combinator::Descendant);
    // @scope: under a scope root (the node or an ancestor) with no scope limit between it and the node
    for (u32 root = node; root != kNone; root = s.parent(root)) {
        const MatchCtx c{s, rule, root};
        if (!matchList(c, root, rule.scopeFirst, rule.scopeCount)) continue;
        bool cut = false;
        if (rule.limitCount)
            for (u32 n = node; n != root && !cut; n = s.parent(n)) cut = matchList(c, n, rule.limitFirst, rule.limitCount);
        if (!cut && matchChain(c, node, rule.chain.data(), last, kNone, SimpleSelector::Combinator::Descendant)) return true;
    }
    return false;
}

bool StyleEngine::resolveVar(std::string_view raw, Prop p, PropValue& out, int depth) const {
    if (depth > 8) return false;
    std::string_view v = trim(raw);
    const auto at = v.find("var(");
    if (at == std::string_view::npos) return parsePropValue(p, v, out);
    // whole-value var(--name[, fallback]); partial substitution only for single var use
    const auto close = v.rfind(')');
    std::string_view inner = v.substr(at + 4, close - at - 4);
    const auto comma = inner.find(',');
    const std::string_view name = trim(inner.substr(0, comma));
    const std::string_view fallback = comma == std::string_view::npos ? std::string_view{} : trim(inner.substr(comma + 1));
    const u32 h = hashId(name);
    std::string_view tok;
    bool found = false;
    for (auto it = m_localTokens.rbegin(); it != m_localTokens.rend() && !found; ++it)
        if (it->first == h) { tok = it->second; found = true; }
    for (auto it = m_sheets.rbegin(); it != m_sheets.rend() && !found; ++it) found = it->token(h, tok);
    if (!found) {
        if (fallback.empty()) return false;
        tok = fallback;
    }
    return resolveVar(tok, p, out, depth + 1);
}

namespace {
u64 hashStyle(const ComputedStyle& cs) {
    // FNV over the bytes of a value-initialised copy is not padding-safe; hash fields that matter most and compare.
    u64 h = 1469598103934665603ull;
    auto mix = [&](f32 f) { u32 b; std::memcpy(&b, &f, 4); h = (h ^ b) * 1099511628211ull; };
    mix(cs.background.r); mix(cs.background.g); mix(cs.background.a); mix(cs.color.r); mix(cs.color.a);
    mix(cs.width.value); mix(cs.height.value); mix(cs.padding.l); mix(cs.padding.t); mix(cs.gap); mix(cs.fontSize);
    mix(cs.opacity); mix(cs.borderWidth); mix(cs.radius[0]); mix(cs.grow);
    // placement and the remaining colour channels: a HUD moves anchored / absolute nodes and fades colours every frame,
    // and styles that differ only in those must not share one probe chain (fuse_ui_bench_hud spent 80% of a frame
    // comparing ComputedStyles in it)
    mix(cs.left.value); mix(cs.top.value); mix(cs.right.value); mix(cs.bottom.value); mix(cs.margin.l); mix(cs.margin.t);
    mix(cs.background.b); mix(cs.color.g); mix(cs.color.b); mix(cs.background2.r); mix(cs.background2.g); mix(cs.background2.b);
    mix(cs.borderColor.r); mix(cs.borderColor.g); mix(cs.borderColor.b); mix(cs.minWidth.value); mix(cs.minHeight.value);
    mix(cs.maxWidth.value); mix(cs.maxHeight.value); mix(cs.basis.value); mix(cs.shadowBlur); mix(cs.letterSpacing);
    mix(cs.transform.m[0]); mix(cs.transform.m[4]); mix(cs.transform.m[5]); mix(f32(cs.zIndex)); mix(f32(cs.animations[0].name));
    h = (h ^ (u64(cs.layout) | u64(cs.direction) << 8 | u64(cs.visible) << 16 | u64(cs.alignItems) << 24 | u64(cs.justify) << 32)) * 1099511628211ull;
    return h;
}
} // namespace

u32 StyleEngine::intern(const ComputedStyle& cs) {
    const u64 h = hashStyle(cs);
    const std::size_t mask = m_hashIndex.size() - 1;
    std::size_t slot = std::size_t(h >> 20) & mask;
    for (;;) {
        const u32 e = m_hashIndex[slot];
        if (e == 0) break;
        if (m_table[e - 1] == cs) return e - 1;
        slot = (slot + 1) & mask;
    }
    if ((std::size_t(m_interned) + 1) * 2 > m_hashIndex.size()) {
        rehash(m_hashIndex.size() * 2);
        return intern(cs);
    }
    m_table.push_back(cs);
    m_isScratch.push_back(0);
    m_hashIndex[slot] = u32(m_table.size());
    ++m_interned;
    return u32(m_table.size() - 1);
}

void StyleEngine::rehash(std::size_t slots) {
    // table indices are node style ids: the table never moves, only the index is rebuilt (scratch entries are not in it)
    m_hashIndex.assign(slots, 0);
    const std::size_t mask = slots - 1;
    for (std::size_t i = 0; i < m_table.size(); ++i) {
        if (m_isScratch[i]) continue;
        std::size_t slot = std::size_t(hashStyle(m_table[i]) >> 20) & mask;
        while (m_hashIndex[slot]) slot = (slot + 1) & mask;
        m_hashIndex[slot] = u32(i + 1);
    }
}

u32 StyleEngine::acquireScratch(const ComputedStyle& init) {
    if (!m_freeScratch.empty()) {
        const u32 i = m_freeScratch.back();
        m_freeScratch.pop_back();
        m_table[i] = init;
        return i;
    }
    const ComputedStyle copy = init; // `init` may live in the table that push_back reallocates
    m_table.push_back(copy);
    m_isScratch.push_back(1);
    return u32(m_table.size() - 1);
}
void StyleEngine::releaseScratch(u32 index) {
    if (index < m_table.size() && m_isScratch[index]) m_freeScratch.push_back(index);
}

u32 StyleEngine::compute(const StyleSubject& s, u32 node, const ComputedStyle* parent, const ComputedStyle* defaults,
                         const std::vector<std::pair<Prop, PropValue>>& inl) {
    ComputedStyle cs = defaults ? *defaults : ComputedStyle{};
    if (parent) inheritFrom(cs, *parent);
    m_scratch.clear();
    m_localTokens.clear();
    for (const Ordered& o : m_ordered) {
        const StyleRule* r = o.rule;
        if (matches(s, node, *r) && (r->cond < 0 || conditionHolds(s, node, o))) {
            m_scratch.push_back(r);
            for (const auto& d : r->decls)
                if (d.prop == Prop::Count) m_localTokens.emplace_back(d.customHash, std::string_view(d.raw));
        }
    }
    for (const StyleRule* r : m_scratch)
        for (const auto& d : r->decls) {
            if (d.prop == Prop::Count) continue;
            if (d.raw.empty()) applyProp(cs, d.prop, d.value);
            else {
                PropValue v;
                if (resolveVar(d.raw, d.prop, v, 0)) applyProp(cs, d.prop, v);
            }
        }
    for (const auto& [p, v] : inl) applyProp(cs, p, v);
    if (cs.paddingStart >= 0.f) (cs.rtl ? cs.padding.r : cs.padding.l) = cs.paddingStart; // logical padding
    if (cs.paddingEnd >= 0.f) (cs.rtl ? cs.padding.l : cs.padding.r) = cs.paddingEnd;
    return intern(cs);
}

} // namespace fuse::ui
