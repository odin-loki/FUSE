#include "fuse/ui/style.hpp"

#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <atomic>
#include <tuple>
#include <string>
#include <cstdio>
#include <vector>

namespace fuse::ui {

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
    {Prop::GradientAngle, "gradient-angle", T::Number, false, false, false},
    {Prop::GradientStops, "gradient-stops", T::Stops, false, false, false},
    {Prop::BackgroundClip, "background-clip", T::Enum, false, false, false},
    {Prop::BackdropFilter, "backdrop-filter", T::Filter, false, false, false},
    {Prop::Filter, "filter", T::Filter, false, false, false},
    {Prop::TransitionProperty, "transition-property", T::List, false, false, false},
    {Prop::TransitionDuration, "transition-duration", T::List, false, false, false},
    {Prop::TransitionTimingFunction, "transition-timing-function", T::List, false, false, false},
    {Prop::TransitionDelay, "transition-delay", T::List, false, false, false},
    {Prop::AnimationName, "animation-name", T::List, false, false, false},
    {Prop::AnimationDuration, "animation-duration", T::List, false, false, false},
    {Prop::AnimationTimingFunction, "animation-timing-function", T::List, false, false, false},
    {Prop::AnimationDelay, "animation-delay", T::List, false, false, false},
    {Prop::AnimationIterationCount, "animation-iteration-count", T::List, false, false, false},
    {Prop::AnimationDirection, "animation-direction", T::List, false, false, false},
    {Prop::AnimationFillMode, "animation-fill-mode", T::List, false, false, false},
    {Prop::AnimationPlayState, "animation-play-state", T::List, false, false, false},
    {Prop::TransitionBehavior, "transition-behavior", T::List, false, false, false},
    {Prop::AnimationComposition, "animation-composition", T::List, false, false, false},
    {Prop::Transform, "transform", T::Transform, false, true, false},
    {Prop::TransformOrigin, "transform-origin", T::List, false, false, false},
    {Prop::AnimationTimeline, "animation-timeline", T::List, false, false, false},
    {Prop::ScrollTimeline, "scroll-timeline", T::List, false, false, false},
    {Prop::ViewTimeline, "view-timeline", T::List, false, false, false},
    {Prop::Perspective, "perspective", T::Length, false, true, false},
    {Prop::PerspectiveOrigin, "perspective-origin", T::List, false, false, false},
    {Prop::BackfaceVisibility, "backface-visibility", T::Bool, false, false, false},
    {Prop::TransformStyle, "transform-style", T::Bool, false, false, false},
    {Prop::ViewTimelineInset, "view-timeline-inset", T::List, false, false, false},
    {Prop::TimelineScope, "timeline-scope", T::List, false, false, false},
    {Prop::AnimationRangeStart, "animation-range-start", T::List, false, false, false},
    {Prop::AnimationRangeEnd, "animation-range-end", T::List, false, false, false},
    {Prop::ContainerType, "container-type", T::Enum, false, false, true},
    {Prop::ContainerName, "container-name", T::List, false, false, false},
    {Prop::ClipPath, "clip-path", T::List, false, false, false},
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
    {Prop::ContainerType, "normal", 0}, {Prop::ContainerType, "size", 1}, {Prop::ContainerType, "inline-size", 2},
    {Prop::Gradient, "none", u8(GradientDir::None)},  {Prop::Gradient, "vertical", u8(GradientDir::Vertical)},
    {Prop::Gradient, "horizontal", u8(GradientDir::Horizontal)}, {Prop::Gradient, "angle", u8(GradientDir::Angle)},
    {Prop::TextAlign, "left", u8(TextAlignment::Start)}, {Prop::TextAlign, "start", u8(TextAlignment::Start)},
    {Prop::TextAlign, "center", u8(TextAlignment::Center)}, {Prop::TextAlign, "right", u8(TextAlignment::End)},
    {Prop::TextAlign, "end", u8(TextAlignment::End)},
    {Prop::BackgroundClip, "border-box", u8(BackgroundClip::BorderBox)},
    {Prop::BackgroundClip, "padding-box", u8(BackgroundClip::PaddingBox)},
    {Prop::BackgroundClip, "content-box", u8(BackgroundClip::ContentBox)},
    {Prop::BackgroundClip, "text", u8(BackgroundClip::Text)},
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool parseNumber(std::string_view s, f32& out, std::string_view& unit) {
    s = trim(s);
    std::size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == '-' || s[i] == '+')) ++i;
    if (i == 0) return false;
    std::string buf(s.substr(0, i));
    char* end = nullptr;
    out = std::strtof(buf.c_str(), &end);
    if (end != buf.c_str() + buf.size()) return false;
    unit = s.substr(i);
    return true;
}

bool parseLength(std::string_view s, Length& out) {
    s = trim(s);
    if (s == "auto") { out = {}; return true; }
    f32 v;
    std::string_view unit;
    if (!parseNumber(s, v, unit)) return false;
    if (unit.empty() || unit == "px" || unit == "dp") out = Length::px(v);
    else if (unit == "%") out = Length::pct(v);
    else if (unit == "fr") out = Length::fr(v);
    else return false;
    return true;
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = char(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    return -1;
}

struct NamedColor { std::string_view name; u32 hex; };
/// CSS Color Module 4 named colours (SVG uses the same set).
constexpr NamedColor kNamed[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4},
    {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000},
    {"blanchedalmond", 0xFFEBCD}, {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E},
    {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C},
    {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9}, {"darkkhaki", 0xBDB76B},
    {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC},
    {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1}, {"darkviolet", 0x9400D3},
    {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700},
    {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xADFF2F},
    {"grey", 0x808080}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA},
    {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6},
    {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3},
    {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A},
    {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66CDAA},
    {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371},
    {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC}, {"mediumvioletred", 0xC71585},
    {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1}, {"moccasin", 0xFFE4B5},
    {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6}, {"olive", 0x808000},
    {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500}, {"orangered", 0xFF4500}, {"orchid", 0xDA70D6},
    {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE}, {"palevioletred", 0xDB7093},
    {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F}, {"pink", 0xFFC0CB},
    {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399},
    {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1}, {"saddlebrown", 0x8B4513},
    {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57}, {"seashell", 0xFFF5EE},
    {"sienna", 0xA0522D}, {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD},
    {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xFFFAFA}, {"springgreen", 0x00FF7F},
    {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"teal", 0x008080}, {"thistle", 0xD8BFD8},
    {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3},
    {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00}, {"yellowgreen", 0x9ACD32}};
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

namespace {
struct FilterNames {
    std::mutex mu;
    std::deque<std::string> names; // deque: stable addresses for the returned views
};
FilterNames& filterNames() {
    static FilterNames n;
    return n;
}
} // namespace
u32 filterNameId(std::string_view id) {
    FilterNames& t = filterNames();
    std::lock_guard lock(t.mu);
    for (std::size_t i = 0; i < t.names.size(); ++i)
        if (t.names[i] == id) return u32(i);
    t.names.emplace_back(id);
    return u32(t.names.size() - 1);
}
std::string_view filterName(u32 id) {
    FilterNames& t = filterNames();
    std::lock_guard lock(t.mu);
    return id < t.names.size() ? std::string_view(t.names[id]) : std::string_view{};
}

// `transition` property names: longhands, plus shorthands expanding to their longhands
static bool transitionProps(std::string_view name, PropMask& m) {
    auto bit = [&](Prop p) { m.set(u8(p)); };
    if (name == "all") { // discrete properties are included; they only transition with allow-discrete
        for (int k = 0; k < kPropCount; ++k)
            if (kProps[k].type != T::List) m.set(u32(k));
        return true;
    }
    if (name == "background" || name == "background-color") { bit(Prop::Background); bit(Prop::Background2); return true; }
    if (name == "padding") { bit(Prop::PaddingLeft); bit(Prop::PaddingTop); bit(Prop::PaddingRight); bit(Prop::PaddingBottom); return true; }
    if (name == "margin") { bit(Prop::MarginLeft); bit(Prop::MarginTop); bit(Prop::MarginRight); bit(Prop::MarginBottom); return true; }
    if (name == "border-radius") { bit(Prop::RadiusTL); bit(Prop::RadiusTR); bit(Prop::RadiusBR); bit(Prop::RadiusBL); return true; }
    if (name == "border") { bit(Prop::BorderColor); bit(Prop::BorderWidth); return true; }
    if (name == "box-shadow") { bit(Prop::ShadowColor); bit(Prop::ShadowBlur); bit(Prop::ShadowX); bit(Prop::ShadowY); return true; }
    if (name == "inset") { bit(Prop::Left); bit(Prop::Top); bit(Prop::Right); bit(Prop::Bottom); return true; }
    Prop p;
    if (!findProp(name, p)) return false;
    if (propInfo(p).type != T::List) bit(p); // discrete properties: animated only with allow-discrete
    return true;
}
static bool parseTime(std::string_view w, f32& out) {
    std::string_view unit;
    if (!parseNumber(w, out, unit)) return false;
    if (unit == "ms") out /= 1000.f;
    else if (unit != "s") return false;
    return true;
}
static bool parseTiming(std::string_view w, Easing& t) {
    using K = Easing::Kind;
    using SP = Easing::StepPos;
    auto bez = [&](f32 a, f32 b, f32 c, f32 d) { t.kind = K::Bezier, t.x1 = a, t.y1 = b, t.x2 = c, t.y2 = d; return true; };
    if (w == "ease") return bez(0.25f, 0.1f, 0.25f, 1.f);
    if (w == "linear") return bez(0.f, 0.f, 1.f, 1.f);
    if (w == "ease-in") return bez(0.42f, 0.f, 1.f, 1.f);
    if (w == "ease-out") return bez(0.f, 0.f, 0.58f, 1.f);
    if (w == "ease-in-out") return bez(0.42f, 0.f, 0.58f, 1.f);
    if (w == "step-start") { t.kind = K::Steps, t.steps = 1, t.stepPos = SP::JumpStart; return true; }
    if (w == "step-end") { t.kind = K::Steps, t.steps = 1, t.stepPos = SP::JumpEnd; return true; }
    const auto open = w.find('(');
    if (open == std::string_view::npos || w.back() != ')') return false;
    const std::string_view fn = w.substr(0, open), args = w.substr(open + 1, w.size() - open - 2);
    std::string_view a[4];
    int n = 0;
    for (std::size_t s = 0, i = 0; i <= args.size(); ++i)
        if (i == args.size() || args[i] == ',') {
            if (n == 4) return false;
            a[n++] = trim(args.substr(s, i - s));
            s = i + 1;
        }
    std::string_view unit;
    if (fn == "cubic-bezier") {
        f32 v[4];
        if (n != 4) return false;
        for (int k = 0; k < 4; ++k)
            if (!parseNumber(a[k], v[k], unit) || !unit.empty()) return false;
        if (v[0] < 0.f || v[0] > 1.f || v[2] < 0.f || v[2] > 1.f) return false;
        return bez(v[0], v[1], v[2], v[3]);
    }
    if (fn == "steps") {
        f32 c = 0.f;
        if (n < 1 || n > 2 || !parseNumber(a[0], c, unit) || !unit.empty() || c < 1.f || c != std::floor(c) || c > 255.f) return false;
        SP pos = SP::JumpEnd;
        if (n == 2) {
            if (a[1] == "jump-start" || a[1] == "start") pos = SP::JumpStart;
            else if (a[1] == "jump-end" || a[1] == "end") pos = SP::JumpEnd;
            else if (a[1] == "jump-none") pos = SP::JumpNone;
            else if (a[1] == "jump-both") pos = SP::JumpBoth;
            else return false;
        }
        if (pos == SP::JumpNone && c < 2.f) return false;
        t.kind = K::Steps, t.steps = u8(c), t.stepPos = pos;
        return true;
    }
    return false;
}
bool parseEasing(std::string_view text, Easing& out) { return parseTiming(trim(text), out); }

// Splits on top-level commas / whitespace (parentheses nest)
static void splitTop(std::string_view text, char sep, std::vector<std::string_view>& out) {
    out.clear();
    int depth = 0;
    std::size_t s = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && text[i] == '(') ++depth;
        if (i < text.size() && text[i] == ')') --depth;
        const bool at = i == text.size() || (depth == 0 && (sep == ' ' ? std::isspace(static_cast<unsigned char>(text[i])) != 0 : text[i] == sep));
        if (!at) continue;
        const std::string_view item = trim(text.substr(s, i - s));
        s = i + 1;
        if (sep != ' ' || !item.empty()) out.push_back(item);
    }
}
static bool isIdent(std::string_view w) {
    if (w.empty() || std::isdigit(static_cast<unsigned char>(w[0]))) return false;
    for (char c : w)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
    return true;
}
static bool parseDirection(std::string_view w, u8& out) {
    if (w == "normal") out = u8(AnimDirection::Normal);
    else if (w == "reverse") out = u8(AnimDirection::Reverse);
    else if (w == "alternate") out = u8(AnimDirection::Alternate);
    else if (w == "alternate-reverse") out = u8(AnimDirection::AlternateReverse);
    else return false;
    return true;
}
static bool parseFill(std::string_view w, u8& out) {
    if (w == "none") out = u8(AnimFill::None);
    else if (w == "forwards") out = u8(AnimFill::Forwards);
    else if (w == "backwards") out = u8(AnimFill::Backwards);
    else if (w == "both") out = u8(AnimFill::Both);
    else return false;
    return true;
}
static bool parsePlay(std::string_view w, u8& out) {
    if (w == "running") out = 0;
    else if (w == "paused") out = 1;
    else return false;
    return true;
}
static bool parseBehavior(std::string_view w, u8& out) {
    if (w == "normal") out = 0;
    else if (w == "allow-discrete") out = 1;
    else return false;
    return true;
}
static bool parseComposite(std::string_view w, u8& out) {
    if (w == "replace") out = u8(AnimComposite::Replace);
    else if (w == "add") out = u8(AnimComposite::Add);
    else if (w == "accumulate") out = u8(AnimComposite::Accumulate);
    else return false;
    return true;
}
static bool parseIterations(std::string_view w, f32& out) {
    if (w == "infinite") { out = std::numeric_limits<f32>::infinity(); return true; }
    std::string_view unit;
    return parseNumber(w, out, unit) && unit.empty() && out >= 0.f;
}
// axis keyword: block / y -> 1, inline / x -> 0
static bool parseAxis(std::string_view w, u8& out) {
    if (w == "block" || w == "y" || w == "vertical") out = 1;
    else if (w == "inline" || w == "x" || w == "horizontal") out = 0;
    else return false;
    return true;
}
// animation-timeline entry: auto | none | <dashed-ident> | scroll([<scroller>] [<axis>]) | view([<axis>])
// (kind in fns[k], name in ids[k], axis | scroller << 1 in tracks[k].value)
static bool parseTimelineItem(std::string_view w, PropValue& out, u8 k) {
    out.ids[k] = 0;
    out.tracks[k].value = 1.f;
    if (w == "auto") { out.fns[k] = 0; return true; }
    if (w == "none") { out.fns[k] = 1; return true; }
    if (w.starts_with("--") && isIdent(w)) { out.fns[k] = 2; out.ids[k] = animationNameId(w); return true; }
    const bool scroll = w.starts_with("scroll("), view = w.starts_with("view(");
    if ((!scroll && !view) || w.back() != ')') return false;
    std::string_view args = trim(w.substr(scroll ? 7 : 5, w.size() - (scroll ? 8 : 6)));
    u8 axis = 1, scroller = 0, insets = 0;
    out.pairs[k] = {};
    while (!args.empty()) {
        const std::size_t sp = args.find(' ');
        const std::string_view tok = args.substr(0, sp);
        args = sp == std::string_view::npos ? std::string_view{} : trim(args.substr(sp + 1));
        if (parseAxis(tok, axis)) continue;
        if (scroll && tok == "nearest") scroller = 0;
        else if (scroll && tok == "root") scroller = 1;
        else if (scroll && tok == "self") scroller = 2;
        else if (view && insets < 2) { // <inset>{1,2}: auto | <length-percentage>
            Length l;
            if (tok != "auto" && (!parseLength(tok, l) || l.unit == Unit::Fr)) return false;
            out.pairs[k][insets++] = l;
        } else return false;
    }
    if (insets == 1) out.pairs[k][1] = out.pairs[k][0];
    out.fns[k] = scroll ? 3 : 4;
    out.tracks[k].value = f32(axis | scroller << 1);
    return true;
}
// animation-range-start / -end: normal | <length-percentage> | <timeline-range-name> <length-percentage>?
// (consumes one or two of `w`; a bare name means 0% for the start, 100% for the end)
static int parseRangeWords(const std::string_view* w, int n, bool isEnd, u8& name, Length& off) {
    static constexpr std::pair<std::string_view, TimelineRange> kNames[] = {
        {"cover", TimelineRange::Cover}, {"contain", TimelineRange::Contain}, {"entry", TimelineRange::Entry},
        {"exit", TimelineRange::Exit}, {"entry-crossing", TimelineRange::EntryCrossing}, {"exit-crossing", TimelineRange::ExitCrossing}};
    name = 0, off = {};
    if (n < 1) return 0;
    if (w[0] == "normal") return 1;
    auto len = [](std::string_view t, Length& l) { return parseLength(t, l) && l.unit != Unit::Auto && l.unit != Unit::Fr; };
    for (const auto& [word, r] : kNames)
        if (w[0] == word) {
            name = u8(r);
            if (n > 1 && len(w[1], off)) return 2;
            off = Length::pct(isEnd ? 100.f : 0.f);
            return 1;
        }
    return len(w[0], off) ? 1 : 0;
}
static bool parseRangeItem(std::string_view text, bool isEnd, u8& name, Length& off) {
    std::vector<std::string_view> words;
    splitTop(text, ' ', words);
    return !words.empty() && words.size() <= 2 &&
           parseRangeWords(words.data(), int(words.size()), isEnd, name, off) == int(words.size());
}
// One entry of a transition-* / animation-* longhand list
static bool parseListItem(Prop p, std::string_view w, PropValue& out, u8 k, bool single) {
    switch (p) {
    case Prop::TransitionProperty:
        if (w == "none") return single;
        return isIdent(w) && w != "initial" && w != "inherit" && w != "unset" && w != "default" &&
               transitionProps(w, out.masks[k]);
    case Prop::TransitionDuration: case Prop::AnimationDuration:
        return parseTime(w, out.tracks[k].value) && out.tracks[k].value >= 0.f;
    case Prop::TransitionDelay: case Prop::AnimationDelay: return parseTime(w, out.tracks[k].value);
    case Prop::TransitionTimingFunction: case Prop::AnimationTimingFunction: return parseTiming(w, out.eases[k]);
    case Prop::AnimationName:
        if (w == "none") { out.ids[k] = 0; return true; }
        if (!isIdent(w) || w == "initial" || w == "inherit" || w == "unset" || w == "default") return false;
        out.ids[k] = animationNameId(w);
        return true;
    case Prop::AnimationIterationCount: return parseIterations(w, out.tracks[k].value);
    case Prop::AnimationDirection: return parseDirection(w, out.fns[k]);
    case Prop::AnimationFillMode: return parseFill(w, out.fns[k]);
    case Prop::AnimationPlayState: return parsePlay(w, out.fns[k]);
    case Prop::TransitionBehavior: return parseBehavior(w, out.fns[k]);
    case Prop::AnimationComposition: return parseComposite(w, out.fns[k]);
    case Prop::AnimationTimeline: return parseTimelineItem(w, out, k);
    case Prop::AnimationRangeStart: case Prop::AnimationRangeEnd:
        return parseRangeItem(w, p == Prop::AnimationRangeEnd, out.fns[k], out.pairs[k][0]);
    case Prop::TimelineScope:
        if (!w.starts_with("--") || !isIdent(w)) return false;
        out.ids[k] = animationNameId(w);
        return true;
    case Prop::ContainerName:
        if (!isIdent(w) || w == "none" || w == "and" || w == "not" || w == "or") return false;
        out.ids[k] = animationNameId(w);
        return true;
    default: return false;
    }
}
static bool parseList(Prop p, std::string_view text, PropValue& out) {
    std::vector<std::string_view> items;
    splitTop(text, ',', items);
    if (items.empty() || items.size() > std::size_t(kMaxTransitions)) return false;
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (items[k].empty() || !parseListItem(p, items[k], out, u8(k), items.size() == 1)) return false;
        ++out.trackCount;
    }
    return true;
}
static PropValue listValue(PropType) { PropValue v; v.type = PropType::List; return v; }

// `transition` shorthand: "none" | <single-transition>#, [<property> | all | none] || <time> || <easing> || <time>
static bool parseTransitionShorthand(std::string_view text, PropValue (&out)[5]) {
    for (auto& v : out) v = listValue(PropType::List);
    std::vector<std::string_view> items, words;
    splitTop(text, ',', items);
    if (items.empty() || items.size() > std::size_t(kMaxTransitions)) return false;
    for (std::size_t k = 0; k < items.size(); ++k) {
        splitTop(items[k], ' ', words);
        if (words.empty()) return false;
        bool haveProp = false, haveEase = false, haveBehavior = false;
        u8 behavior = 0;
        int times = 0;
        Easing e;
        f32 dur = 0.f, delay = 0.f;
        PropMask m;
        for (auto w : words) {
            f32 v = 0.f;
            if (times < 2 && parseTime(w, v)) {
                if (times == 0 && v < 0.f) return false;
                (times++ == 0 ? dur : delay) = v;
            } else if (!haveEase && parseTiming(w, e)) haveEase = true;
            else if (!haveBehavior && parseBehavior(w, behavior)) haveBehavior = true;
            else if (!haveProp && w == "none") { if (items.size() != 1) return false; haveProp = true; }
            else if (!haveProp && isIdent(w) && transitionProps(w, m)) haveProp = true;
            else return false;
        }
        if (!haveProp) transitionProps("all", m);
        out[0].masks[k] = m, out[1].tracks[k].value = dur, out[2].eases[k] = e, out[3].tracks[k].value = delay;
        out[4].fns[k] = behavior;
        for (auto& v : out) ++v.trackCount;
    }
    return true;
}
// `animation` shorthand: <single-animation>#, <time> || <easing> || <time> || <iteration-count> || <direction> ||
// <fill-mode> || <play-state> || [none | <name>]
static bool parseAnimationShorthand(std::string_view text, PropValue (&out)[8]) {
    for (auto& v : out) v = listValue(PropType::List);
    std::vector<std::string_view> items, words;
    splitTop(text, ',', items);
    if (items.empty() || items.size() > std::size_t(kMaxTransitions)) return false;
    for (std::size_t k = 0; k < items.size(); ++k) {
        splitTop(items[k], ' ', words);
        if (words.empty()) return false;
        bool hEase = false, hIter = false, hDir = false, hFill = false, hPlay = false, hName = false;
        int times = 0;
        f32 dur = 0.f, delay = 0.f, iter = 1.f;
        Easing e;
        u8 dir = 0, fill = 0, play = 0;
        u32 name = 0;
        for (auto w : words) {
            f32 v = 0.f;
            if (times < 2 && parseTime(w, v)) {
                if (times == 0 && v < 0.f) return false;
                (times++ == 0 ? dur : delay) = v;
            } else if (!hEase && parseTiming(w, e)) hEase = true;
            else if (!hIter && parseIterations(w, iter)) hIter = true;
            else if (!hDir && parseDirection(w, dir)) hDir = true;
            else if (!hFill && w != "none" && parseFill(w, fill)) hFill = true;
            else if (!hPlay && parsePlay(w, play)) hPlay = true;
            else if (!hName && w == "none") hName = true;
            else if (!hFill && w == "none") hFill = true; // second none: fill-mode
            else if (!hName && isIdent(w) && w != "initial" && w != "inherit" && w != "unset" && w != "default") {
                name = animationNameId(w);
                hName = true;
            } else return false;
        }
        out[0].ids[k] = name, out[1].tracks[k].value = dur, out[2].eases[k] = e, out[3].tracks[k].value = delay;
        out[4].tracks[k].value = iter, out[5].fns[k] = dir, out[6].fns[k] = fill, out[7].fns[k] = play;
        for (auto& v : out) ++v.trackCount;
    }
    return true;
}

f32 easeValue(const Easing& s, f32 t) {
    t = std::clamp(t, 0.f, 1.f);
    if (s.kind == Easing::Kind::Steps) {
        using SP = Easing::StepPos;
        const f32 n = f32(s.steps);
        f32 step = std::floor(t * n);
        if (s.stepPos == SP::JumpStart || s.stepPos == SP::JumpBoth) step += 1.f;
        if (t >= 1.f && s.stepPos != SP::JumpStart && s.stepPos != SP::JumpBoth) step = n; // the end value at 1
        const f32 jumps = s.stepPos == SP::JumpNone ? n - 1.f : s.stepPos == SP::JumpBoth ? n + 1.f : n;
        if (t >= 1.f) return 1.f;
        return std::min(step, jumps) / jumps;
    }
    if (s.x1 == s.y1 && s.x2 == s.y2) return t; // linear
    // x(u) = 3(1-u)^2 u x1 + 3(1-u) u^2 x2 + u^3; solve x(u) = t (Newton, then bisection), return y(u)
    const auto bx = [&](f32 u) { const f32 v = 1.f - u; return 3.f * v * v * u * s.x1 + 3.f * v * u * u * s.x2 + u * u * u; };
    const auto by = [&](f32 u) { const f32 v = 1.f - u; return 3.f * v * v * u * s.y1 + 3.f * v * u * u * s.y2 + u * u * u; };
    const auto dx = [&](f32 u) { const f32 v = 1.f - u; return 3.f * v * v * s.x1 + 6.f * v * u * (s.x2 - s.x1) + 3.f * u * u * (1.f - s.x2); };
    f32 u = t;
    for (int k = 0; k < 8; ++k) {
        const f32 e = bx(u) - t;
        if (std::fabs(e) < 1e-6f) return by(u);
        const f32 g = dx(u);
        if (std::fabs(g) < 1e-6f) break;
        u = std::clamp(u - e / g, 0.f, 1.f);
    }
    f32 lo = 0.f, hi = 1.f;
    u = t;
    for (int k = 0; k < 40; ++k) {
        const f32 x = bx(u);
        if (std::fabs(x - t) < 1e-6f) break;
        (x < t ? lo : hi) = u;
        u = 0.5f * (lo + hi);
    }
    return by(u);
}

Decomposed2 decompose(const Affine2& m) {
    Decomposed2 r;
    r.tx = m.e, r.ty = m.f;
    f32 a = m.a, b = m.b, c = m.c, d = m.d;
    r.sx = std::sqrt(a * a + b * b);
    if (r.sx > 0.f) a /= r.sx, b /= r.sx;
    if (a * d - b * c < 0.f) { // a reflection: carried by a negative x scale
        a = -a, b = -b, r.sx = -r.sx;
    }
    r.shear = a * c + b * d;
    c -= a * r.shear, d -= b * r.shear;
    r.sy = std::sqrt(c * c + d * d);
    if (r.sy > 0.f) r.shear /= r.sy;
    if (r.sx == 0.f) a = 1.f, b = 0.f;
    r.angle = std::atan2(b, a) * 57.29577951308232f;
    return r;
}
Affine2 recompose(const Decomposed2& d) {
    const f32 t = d.angle * 0.017453292519943295f, cs = std::cos(t), sn = std::sin(t);
    const Affine2 rot{cs, sn, -sn, cs, 0.f, 0.f}, sk{1.f, 0.f, d.shear, 1.f, 0.f, 0.f}, sc{d.sx, 0.f, 0.f, d.sy, 0.f, 0.f};
    Affine2 m = rot * sk * sc;
    m.e = d.tx, m.f = d.ty;
    return m;
}

namespace {
constexpr f32 kRad = 0.017453292519943295f;
// matrix() / rotate3d() / matrix3d() entries never match pairwise (they interpolate decomposed)
bool isMatrixFn(u8 fn) {
    const TransformFn f = TransformFn(fn);
    return (f >= TransformFn::MatA && f <= TransformFn::MatF) || (f >= TransformFn::R3X && f <= TransformFn::R3A) ||
           f >= TransformFn::M3_0;
}
bool isScaleFn(u8 fn) {
    const TransformFn f = TransformFn(fn);
    return f == TransformFn::ScaleX || f == TransformFn::ScaleY || f == TransformFn::ScaleZ;
}
f32 transformIdentity(u8 fn) {
    switch (TransformFn(fn)) {
    case TransformFn::ScaleX: case TransformFn::ScaleY: case TransformFn::ScaleZ: case TransformFn::MatA: case TransformFn::MatD:
    case TransformFn::M3_0: case TransformFn::M3_5: case TransformFn::M3_10: case TransformFn::M3_15: return 1.f;
    default: return 0.f;
    }
}
Mat4 rotateAxis(f32 x, f32 y, f32 z, f32 deg) {
    Mat4 r;
    const f32 len = std::sqrt(x * x + y * y + z * z);
    if (len <= 0.f) return r;
    x /= len, y /= len, z /= len;
    const f32 a = deg * kRad, c = std::cos(a), s = std::sin(a), t = 1.f - c;
    r.at(0, 0) = t * x * x + c, r.at(0, 1) = t * x * y - s * z, r.at(0, 2) = t * x * z + s * y;
    r.at(1, 0) = t * x * y + s * z, r.at(1, 1) = t * y * y + c, r.at(1, 2) = t * y * z - s * x;
    r.at(2, 0) = t * x * z - s * y, r.at(2, 1) = t * y * z + s * x, r.at(2, 2) = t * z * z + c;
    return r;
}
// The list as a map about the origin: each function applies to the already transformed space (CSS order).
Mat4 composeTransform(const u8* fns, const f32* args, u8 n) {
    Mat4 m;
    f32 mat[6] = {1.f, 0.f, 0.f, 1.f, 0.f, 0.f}, r3[4] = {}, m3[16] = {};
    for (u8 k = 0; k < n; ++k) {
        const f32 v = args[k];
        const TransformFn f = TransformFn(fns[k]);
        Mat4 e;
        switch (f) {
        case TransformFn::TranslateX: e.at(0, 3) = v; break;
        case TransformFn::TranslateY: e.at(1, 3) = v; break;
        case TransformFn::TranslateZ: e.at(2, 3) = v; break;
        case TransformFn::ScaleX: e.at(0, 0) = v; break;
        case TransformFn::ScaleY: e.at(1, 1) = v; break;
        case TransformFn::ScaleZ: e.at(2, 2) = v; break;
        case TransformFn::Rotate: e = rotateAxis(0.f, 0.f, 1.f, v); break;
        case TransformFn::RotateX: e = rotateAxis(1.f, 0.f, 0.f, v); break;
        case TransformFn::RotateY: e = rotateAxis(0.f, 1.f, 0.f, v); break;
        case TransformFn::SkewX: e.at(0, 1) = std::tan(v * kRad); break;
        case TransformFn::SkewY: e.at(1, 0) = std::tan(v * kRad); break;
        case TransformFn::Perspective: if (v > 0.f) e.at(3, 2) = -1.f / v; break;
        case TransformFn::R3X: case TransformFn::R3Y: case TransformFn::R3Z: case TransformFn::R3A:
            r3[fns[k] - u8(TransformFn::R3X)] = v;
            if (f == TransformFn::R3A) e = rotateAxis(r3[0], r3[1], r3[2], r3[3]);
            break;
        default:
            if (f >= TransformFn::M3_0) {
                const int at = fns[k] - u8(TransformFn::M3_0);
                m3[at] = v;
                if (f == TransformFn::M3_15) for (int q = 0; q < 16; ++q) e.m[std::size_t(q)] = m3[q];
            } else { // matrix(): six consecutive entries, applied at MatF
                mat[fns[k] - u8(TransformFn::MatA)] = v;
                if (f == TransformFn::MatF) e = Mat4::from(Affine2{mat[0], mat[1], mat[2], mat[3], mat[4], mat[5]});
            }
            break;
        }
        m = m * e;
    }
    return m;
}
Mat4 composeTransform(const PropValue& v) { return composeTransform(v.xfns.data(), v.xargs.data(), v.trackCount); }
bool pushFn(PropValue& r, TransformFn f, f32 v) {
    if (r.trackCount >= kMaxTransformArgs) return false;
    r.xfns[r.trackCount] = u8(f), r.xargs[r.trackCount++] = v;
    return true;
}
// Canonical form of a map: translate(tx, ty) rotate(a) skewX(s) scale(sx, sy) from its decomposition (only the
// functions that differ from identity, so axis-aligned maps stay translate + scale).
PropValue transformFromDecomposed(const Decomposed2& d) {
    PropValue r;
    r.type = PropType::Transform;
    pushFn(r, TransformFn::TranslateX, d.tx);
    pushFn(r, TransformFn::TranslateY, d.ty);
    if (d.angle != 0.f) pushFn(r, TransformFn::Rotate, d.angle);
    if (d.shear != 0.f) pushFn(r, TransformFn::SkewX, std::atan(d.shear) * 57.29577951308232f);
    pushFn(r, TransformFn::ScaleX, d.sx);
    pushFn(r, TransformFn::ScaleY, d.sy);
    return r;
}
PropValue transformFromMat4(const Mat4& m) {
    if (m.is2D()) return transformFromDecomposed(decompose(m.affine()));
    PropValue r;
    r.type = PropType::Transform;
    for (int q = 0; q < 16; ++q) pushFn(r, TransformFn(u8(TransformFn::M3_0) + q), m.m[std::size_t(q)]);
    return r;
}
// Lists whose functions match pairwise (the shorter padded with identity functions, CSS Transforms 1 §9).
bool transformsMatch(const PropValue& a, const PropValue& b) {
    const u8 n = std::min(a.trackCount, b.trackCount);
    for (u8 k = 0; k < n; ++k)
        if (a.xfns[k] != b.xfns[k]) return false;
    for (u8 k = 0; k < a.trackCount; ++k) if (isMatrixFn(a.xfns[k])) return false;
    for (u8 k = 0; k < b.trackCount; ++k) if (isMatrixFn(b.xfns[k])) return false;
    return true;
}
PropValue padTransform(const PropValue& v, const PropValue& like) {
    PropValue r = v;
    for (u8 k = v.trackCount; k < like.trackCount; ++k) r.xfns[k] = like.xfns[k], r.xargs[k] = transformIdentity(like.xfns[k]);
    r.trackCount = std::max(v.trackCount, like.trackCount);
    return r;
}
// Mismatched lists: decomposed maps mixed by `mix` (2D when both are 2D, CSS Transforms 2 §decomposition otherwise).
PropValue mixDecomposed(const Mat4& ma, const Mat4& mb, f32 k, bool accumulate) {
    if (ma.is2D() && mb.is2D()) {
        const Decomposed2 da = decompose(ma.affine());
        Decomposed2 db = decompose(mb.affine());
        if (accumulate)
            return transformFromDecomposed({da.tx + db.tx, da.ty + db.ty, da.angle + db.angle, da.shear + db.shear,
                                            da.sx + db.sx - 1.f, da.sy + db.sy - 1.f});
        if (std::fabs(db.angle - da.angle) > 180.f) db.angle += db.angle > da.angle ? -360.f : 360.f; // shortest way
        auto mix = [&](f32 x, f32 y) { return x + (y - x) * k; };
        return transformFromDecomposed({mix(da.tx, db.tx), mix(da.ty, db.ty), mix(da.angle, db.angle), mix(da.shear, db.shear),
                                        mix(da.sx, db.sx), mix(da.sy, db.sy)});
    }
    Decomposed3 a, b, r;
    if (!decompose3(ma, a) || !decompose3(mb, b)) return transformFromMat4(accumulate || k >= 0.5f ? mb : ma); // discrete
    for (int i = 0; i < 3; ++i) {
        r.t[i] = accumulate ? a.t[i] + b.t[i] : a.t[i] + (b.t[i] - a.t[i]) * k;
        r.s[i] = accumulate ? a.s[i] + b.s[i] - 1.f : a.s[i] + (b.s[i] - a.s[i]) * k;
        r.k[i] = accumulate ? a.k[i] + b.k[i] : a.k[i] + (b.k[i] - a.k[i]) * k;
    }
    for (int i = 0; i < 4; ++i) r.p[i] = accumulate ? a.p[i] + b.p[i] - (i == 3 ? 1.f : 0.f) : a.p[i] + (b.p[i] - a.p[i]) * k;
    if (accumulate) { // quaternion product
        const auto& p = a.q;
        const auto& q = b.q;
        r.q = {p[3] * q[0] + p[0] * q[3] + p[1] * q[2] - p[2] * q[1], p[3] * q[1] - p[0] * q[2] + p[1] * q[3] + p[2] * q[0],
               p[3] * q[2] + p[0] * q[1] - p[1] * q[0] + p[2] * q[3], p[3] * q[3] - p[0] * q[0] - p[1] * q[1] - p[2] * q[2]};
    } else { // slerp
        f32 d = a.q[0] * b.q[0] + a.q[1] * b.q[1] + a.q[2] * b.q[2] + a.q[3] * b.q[3];
        d = std::clamp(d, -1.f, 1.f);
        if (std::fabs(d) >= 1.f - 1e-6f) r.q = a.q;
        else {
            const f32 th = std::acos(d), w = std::sin(k * th) / std::sqrt(1.f - d * d), c = std::cos(k * th) - d * w;
            for (int i = 0; i < 4; ++i) r.q[i] = a.q[i] * c + b.q[i] * w;
        }
    }
    return transformFromMat4(recompose3(r));
}
} // namespace

bool Mat4::inverse(Mat4& out) const {
    double a[16], inv[16];
    for (int i = 0; i < 16; ++i) a[i] = m[std::size_t(i)];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (!(std::fabs(det) > 1e-12) || !std::isfinite(det)) return false;
    for (int i = 0; i < 16; ++i) out.m[std::size_t(i)] = f32(inv[i] / det);
    return true;
}
bool invert3(const std::array<f32, 9>& h, std::array<f32, 9>& out) {
    const double a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7], i = h[8];
    const double A = e * i - f * hh, B = -(d * i - f * g), C = d * hh - e * g;
    const double det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1e-12) || !std::isfinite(det)) return false;
    const double r[9] = {A, -(b * i - c * hh), b * f - c * e, B, a * i - c * g, -(a * f - c * d), C, -(a * hh - b * g), a * e - b * d};
    for (int k = 0; k < 9; ++k) out[std::size_t(k)] = f32(r[k] / det);
    return true;
}
bool mapHomography(const std::array<f32, 9>& h, Vec2 p, Vec2& out) {
    const f32 w = h[6] * p.x + h[7] * p.y + h[8];
    if (!(w > 1e-6f)) return false;
    out = {(h[0] * p.x + h[1] * p.y + h[2]) / w, (h[3] * p.x + h[4] * p.y + h[5]) / w};
    return true;
}
Rect mapRectHomography(const std::array<f32, 9>& h, const Rect& r) {
    // clip the quad to W >= eps (in front of the viewer), then take the bounds of the mapped polygon
    const Vec2 in[4] = {{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}};
    constexpr f32 kEps = 1e-4f, kLim = 16384.f;
    f32 x0 = kLim, y0 = kLim, x1 = -kLim, y1 = -kLim;
    bool any = false;
    auto add = [&](Vec2 p) {
        Vec2 q;
        const f32 w = std::max(h[6] * p.x + h[7] * p.y + h[8], kEps);
        q = {(h[0] * p.x + h[1] * p.y + h[2]) / w, (h[3] * p.x + h[4] * p.y + h[5]) / w};
        x0 = std::min(x0, std::clamp(q.x, -kLim, kLim)), y0 = std::min(y0, std::clamp(q.y, -kLim, kLim));
        x1 = std::max(x1, std::clamp(q.x, -kLim, kLim)), y1 = std::max(y1, std::clamp(q.y, -kLim, kLim));
        any = true;
    };
    for (int k = 0; k < 4; ++k) {
        const Vec2 a = in[k], b = in[(k + 1) % 4];
        const f32 wa = h[6] * a.x + h[7] * a.y + h[8] - kEps, wb = h[6] * b.x + h[7] * b.y + h[8] - kEps;
        if (wa >= 0.f) add(a);
        if ((wa >= 0.f) != (wb >= 0.f)) {
            const f32 t = wa / (wa - wb);
            add({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
        }
    }
    if (!any) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}
bool decompose3(const Mat4& in, Decomposed3& out) {
    Mat4 m = in;
    const f32 w = m.at(3, 3);
    if (w == 0.f) return false;
    for (f32& v : m.m) v /= w;
    Mat4 pm = m;
    pm.at(3, 0) = pm.at(3, 1) = pm.at(3, 2) = 0.f, pm.at(3, 3) = 1.f;
    Mat4 pinv;
    if (!pm.inverse(pinv)) return false;
    out = {};
    if (m.at(3, 0) != 0.f || m.at(3, 1) != 0.f || m.at(3, 2) != 0.f) {
        const f32 rhs[4] = {m.at(3, 0), m.at(3, 1), m.at(3, 2), m.at(3, 3)};
        for (int i = 0; i < 4; ++i) {
            f32 s = 0.f;
            for (int j = 0; j < 4; ++j) s += rhs[j] * pinv.at(j, i);
            out.p[std::size_t(i)] = s;
        }
    }
    for (int i = 0; i < 3; ++i) out.t[std::size_t(i)] = m.at(i, 3);
    f32 c[3][3]; // columns (basis images)
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) c[i][j] = m.at(j, i);
    auto len = [](const f32* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    auto dot = [](const f32* a, const f32* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto norm = [&](f32* v, f32 l) { if (l != 0.f) for (int q = 0; q < 3; ++q) v[q] /= l; };
    auto comb = [](f32* a, const f32* b, f32 s) { for (int q = 0; q < 3; ++q) a[q] += b[q] * s; };
    out.s[0] = len(c[0]); norm(c[0], out.s[0]);
    out.k[0] = dot(c[0], c[1]); comb(c[1], c[0], -out.k[0]);
    out.s[1] = len(c[1]); norm(c[1], out.s[1]);
    if (out.s[1] != 0.f) out.k[0] /= out.s[1];
    out.k[1] = dot(c[0], c[2]); comb(c[2], c[0], -out.k[1]);
    out.k[2] = dot(c[1], c[2]); comb(c[2], c[1], -out.k[2]);
    out.s[2] = len(c[2]); norm(c[2], out.s[2]);
    if (out.s[2] != 0.f) out.k[1] /= out.s[2], out.k[2] /= out.s[2];
    const f32 cr[3] = {c[1][1] * c[2][2] - c[1][2] * c[2][1], c[1][2] * c[2][0] - c[1][0] * c[2][2], c[1][0] * c[2][1] - c[1][1] * c[2][0]};
    if (dot(c[0], cr) < 0.f)
        for (int i = 0; i < 3; ++i) { out.s[std::size_t(i)] = -out.s[std::size_t(i)]; for (int q = 0; q < 3; ++q) c[i][q] = -c[i][q]; }
    // R(row, col) = c[col][row]
    const f32 r00 = c[0][0], r11 = c[1][1], r22 = c[2][2];
    out.q[0] = 0.5f * std::sqrt(std::max(1.f + r00 - r11 - r22, 0.f));
    out.q[1] = 0.5f * std::sqrt(std::max(1.f - r00 + r11 - r22, 0.f));
    out.q[2] = 0.5f * std::sqrt(std::max(1.f - r00 - r11 + r22, 0.f));
    out.q[3] = 0.5f * std::sqrt(std::max(1.f + r00 + r11 + r22, 0.f));
    if (c[2][1] < c[1][2]) out.q[0] = -out.q[0]; // R21 - R12
    if (c[0][2] < c[2][0]) out.q[1] = -out.q[1]; // R02 - R20
    if (c[1][0] < c[0][1]) out.q[2] = -out.q[2]; // R10 - R01
    return true;
}
Mat4 recompose3(const Decomposed3& d) {
    Mat4 p, t = Mat4::translate(d.t[0], d.t[1], d.t[2]), r, k, s;
    for (int i = 0; i < 4; ++i) p.at(3, i) = d.p[std::size_t(i)];
    const f32 x = d.q[0], y = d.q[1], z = d.q[2], w = d.q[3];
    r.at(0, 0) = 1.f - 2.f * (y * y + z * z), r.at(0, 1) = 2.f * (x * y - z * w), r.at(0, 2) = 2.f * (x * z + y * w);
    r.at(1, 0) = 2.f * (x * y + z * w), r.at(1, 1) = 1.f - 2.f * (x * x + z * z), r.at(1, 2) = 2.f * (y * z - x * w);
    r.at(2, 0) = 2.f * (x * z - y * w), r.at(2, 1) = 2.f * (y * z + x * w), r.at(2, 2) = 1.f - 2.f * (x * x + y * y);
    k.at(0, 1) = d.k[0], k.at(0, 2) = d.k[1], k.at(1, 2) = d.k[2];
    s.at(0, 0) = d.s[0], s.at(1, 1) = d.s[1], s.at(2, 2) = d.s[2];
    return p * t * r * k * s;
}

Mat4 transform3dOf(const ComputedStyle& s, const Rect& box) {
    u8 fns[kMaxTransformArgs];
    for (u8 k = 0; k < s.transformCount; ++k) fns[k] = u8(s.transformFns[k]);
    const Mat4 m = composeTransform(fns, s.transformArgs.data(), s.transformCount);
    const f32 ox = box.x + s.transformOrigin[0].resolve(box.w, box.w * 0.5f);
    const f32 oy = box.y + s.transformOrigin[1].resolve(box.h, box.h * 0.5f);
    return Mat4::translate(ox, oy, s.transformOriginZ) * m * Mat4::translate(-ox, -oy, -s.transformOriginZ); // O + M(p - O)
}
Affine2 transformOf(const ComputedStyle& s, const Rect& box) { return transform3dOf(s, box).affine(); }
Mat4 perspectiveOf(const ComputedStyle& parent, const Rect& box) {
    if (!(parent.perspective > 0.f)) return {};
    const f32 ox = box.x + parent.perspectiveOrigin[0].resolve(box.w, box.w * 0.5f);
    const f32 oy = box.y + parent.perspectiveOrigin[1].resolve(box.h, box.h * 0.5f);
    Mat4 p;
    p.at(3, 2) = -1.f / std::max(parent.perspective, 1.f);
    return Mat4::translate(ox, oy, 0.f) * p * Mat4::translate(-ox, -oy, 0.f);
}

PropValue interpolateProp(const PropValue& a, const PropValue& b, f32 k) {
    if (a.type != b.type) return k < 0.5f ? a : b;
    if (a.type == T::Transform) {
        if (transformsMatch(a, b)) {
            const PropValue pa = padTransform(a, b), pb = padTransform(b, a);
            PropValue r = pb;
            for (u8 i = 0; i < r.trackCount; ++i) r.xargs[i] = pa.xargs[i] + (pb.xargs[i] - pa.xargs[i]) * k;
            return r;
        }
        // mismatched lists: the decomposed maps (CSS Transforms 1 §9 / 2 §interpolation of matrices)
        return mixDecomposed(composeTransform(a), composeTransform(b), k, false);
    }
    PropValue r = b;
    switch (a.type) {
    case T::Number: r.num = a.num + (b.num - a.num) * k; break;
    case T::Length:
        if (a.len.unit != b.len.unit) return k < 0.5f ? a : b;
        r.len.value = a.len.value + (b.len.value - a.len.value) * k;
        break;
    case T::Color: { // premultiplied
        const f32 al = a.color.a + (b.color.a - a.color.a) * k;
        r.color.a = std::clamp(al, 0.f, 1.f);
        if (r.color.a <= 0.f) { r.color.r = r.color.g = r.color.b = 0.f; break; }
        const f32 ca[3] = {a.color.r, a.color.g, a.color.b}, cb[3] = {b.color.r, b.color.g, b.color.b};
        f32 o[3];
        for (int c = 0; c < 3; ++c) o[c] = std::clamp((ca[c] * a.color.a + (cb[c] * b.color.a - ca[c] * a.color.a) * k) / r.color.a, 0.f, 1.f);
        r.color.r = o[0], r.color.g = o[1], r.color.b = o[2];
        break;
    }
    default: return k < 0.5f ? a : b;
    }
    return r;
}
bool samePropValue(const PropValue& a, const PropValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
    case T::Number: return a.num == b.num;
    case T::Length: return a.len == b.len;
    case T::Color: return a.color.r == b.color.r && a.color.g == b.color.g && a.color.b == b.color.b && a.color.a == b.color.a;
    case T::Bool: return a.b == b.b;
    case T::Enum: return a.e == b.e;
    case T::Transform:
        if (a.trackCount != b.trackCount) return false;
        for (u8 k = 0; k < a.trackCount; ++k)
            if (a.xfns[k] != b.xfns[k] || a.xargs[k] != b.xargs[k]) return false;
        return true;
    default: return false;
    }
}

PropValue interpolateDiscrete(Prop p, const PropValue& a, const PropValue& b, f32 k) {
    if (k <= 0.f) return a;
    if (k >= 1.f) return b;
    if (p == Prop::Display && a.type == T::Bool && b.type == T::Bool) return a.b ? a : b; // shown throughout
    return k < 0.5f ? a : b;
}

PropValue compositeProp(const PropValue& u, const PropValue& v, AnimComposite c) {
    if (c == AnimComposite::Replace || u.type != v.type) return v;
    PropValue r = v;
    if (v.type == T::Transform) {
        if (c == AnimComposite::Add) { // CSS Transforms: the lists concatenate (underlying first)
            if (u.trackCount + v.trackCount > kMaxTransformArgs) return transformFromMat4(composeTransform(u) * composeTransform(v));
            r = u;
            for (u8 k = 0; k < v.trackCount; ++k) pushFn(r, TransformFn(v.xfns[k]), v.xargs[k]);
            return r;
        }
        if (transformsMatch(u, v)) { // accumulate: matching functions combine their arguments
            const PropValue pu = padTransform(u, v);
            r = padTransform(v, u);
            for (u8 k = 0; k < r.trackCount; ++k)
                r.xargs[k] = isScaleFn(r.xfns[k]) ? pu.xargs[k] + r.xargs[k] - 1.f : pu.xargs[k] + r.xargs[k];
            return r;
        }
        return mixDecomposed(composeTransform(u), composeTransform(v), 1.f, true); // decomposed sums
    }
    switch (v.type) {
    case T::Number: r.num = u.num + v.num; break;
    case T::Length:
        if (u.len.unit != v.len.unit || v.len.unit == Unit::Auto) return v;
        r.len.value = u.len.value + v.len.value;
        break;
    case T::Color:
        r.color.r = std::clamp(u.color.r + v.color.r, 0.f, 1.f);
        r.color.g = std::clamp(u.color.g + v.color.g, 0.f, 1.f);
        r.color.b = std::clamp(u.color.b + v.color.b, 0.f, 1.f);
        r.color.a = std::clamp(u.color.a + v.color.a, 0.f, 1.f);
        break;
    default: return v;
    }
    return r;
}

namespace {
void splitWords(std::string_view v, std::string_view* out, int& n, int max);
}
// transform-origin: [left | center | right | top | bottom | <length> | <percentage>]{1,2} (z ignored) -> tracks[0..1]
static bool parseTransformOrigin(std::string_view text, PropValue& out) {
    std::string_view w[3];
    int n = 0;
    splitWords(text, w, n, 3);
    if (n < 1) return false;
    Length x = Length::pct(50.f), y = Length::pct(50.f);
    bool hasX = false, hasY = false;
    for (int i = 0; i < std::min(n, 2); ++i) {
        const std::string_view t = w[i];
        if (t == "left" || t == "right") { if (hasX) return false; x = Length::pct(t == "left" ? 0.f : 100.f); hasX = true; continue; }
        if (t == "top" || t == "bottom") { if (hasY) return false; y = Length::pct(t == "top" ? 0.f : 100.f); hasY = true; continue; }
        if (t == "center") continue;
        Length l;
        if (!parseLength(t, l) || l.unit == Unit::Auto || l.unit == Unit::Fr) return false;
        if (i == 0 && !hasX) x = l, hasX = true;
        else if (!hasY) y = l, hasY = true;
        else return false;
    }
    out.trackCount = 2;
    out.tracks[0] = x, out.tracks[1] = y;
    if (n == 3) { // <length> z
        Length z;
        if (!parseLength(w[2], z) || z.unit != Unit::Px) return false;
        out.trackCount = 3, out.tracks[2] = z;
    }
    return true;
}

// clip-path: none | inset() | circle() | ellipse() | polygon() [<geometry-box>]? (the box is always the border box)
// Encoding: e = shape; inset / circle / ellipse arguments in tracks[], polygon points in pairs[] (count: trackCount)
static bool parseClipPath(std::string_view text, PropValue& out) {
    out.type = PropType::List;
    if (text == "none") return true;
    for (std::string_view box : {"border-box", "padding-box", "content-box", "margin-box", "fill-box", "stroke-box", "view-box"}) {
        if (text.ends_with(box)) { text = trim(text.substr(0, text.size() - box.size())); break; }
    }
    const auto open = text.find('(');
    if (open == std::string_view::npos || text.back() != ')') return false;
    const std::string_view fn = trim(text.substr(0, open)), args = trim(text.substr(open + 1, text.size() - open - 2));
    std::vector<std::string_view> w;
    auto len = [](std::string_view s, Length& l) { return parseLength(s, l) && l.unit != Unit::Auto && l.unit != Unit::Fr; };
    auto position = [&](std::size_t from, Length& x, Length& y) { // "at x y" (keywords or lengths); default centre
        x = y = Length::pct(50.f);
        if (from >= w.size()) return true;
        if (w[from] != "at" || w.size() - from > 3) return false;
        auto kw = [](std::string_view s, Length& l, bool horiz) {
            if (s == "center") l = Length::pct(50.f);
            else if (s == (horiz ? "left" : "top")) l = Length::pct(0.f);
            else if (s == (horiz ? "right" : "bottom")) l = Length::pct(100.f);
            else return parseLength(s, l) && l.unit != Unit::Auto;
            return true;
        };
        if (w.size() - from == 2) return kw(w[from + 1], x, true) || (kw(w[from + 1], y, false) && (x = Length::pct(50.f), true));
        return kw(w[from + 1], x, true) && kw(w[from + 2], y, false);
    };
    auto radius = [&](std::string_view s, Length& l) {
        if (s == "closest-side") { l = Length{}; return true; }
        if (s == "farthest-side") { l = Length{-1.f, Unit::Auto}; return true; }
        return len(s, l) && l.value >= 0.f;
    };
    splitTop(args, ' ', w);
    if (fn == "inset") {
        out.e = 1;
        std::size_t n = 0;
        while (n < w.size() && w[n] != "round") ++n;
        if (n == 0 || n > 4) return false;
        Length v[4];
        for (std::size_t k = 0; k < n; ++k) if (!len(w[k], v[k])) return false;
        const Length t = v[0], r = n > 1 ? v[1] : t, b = n > 2 ? v[2] : t, l = n > 3 ? v[3] : r;
        out.tracks[0] = t, out.tracks[1] = r, out.tracks[2] = b, out.tracks[3] = l;
        out.tracks[4] = Length::px(0.f);
        if (n < w.size()) {
            if (w.size() != n + 2 || !len(w[n + 1], out.tracks[4])) return false;
        }
        out.trackCount = 5;
        return true;
    }
    if (fn == "circle") {
        out.e = 2;
        std::size_t k = 0;
        out.tracks[0] = Length{};
        if (!w.empty() && w[0] != "at") { if (!radius(w[0], out.tracks[0])) return false; k = 1; }
        out.trackCount = 3;
        return position(k, out.tracks[1], out.tracks[2]);
    }
    if (fn == "ellipse") {
        out.e = 3;
        std::size_t k = 0;
        out.tracks[0] = out.tracks[1] = Length{};
        if (!w.empty() && w[0] != "at") {
            if (w.size() < 2 || !radius(w[0], out.tracks[0]) || !radius(w[1], out.tracks[1])) return false;
            k = 2;
        }
        out.trackCount = 4;
        return position(k, out.tracks[2], out.tracks[3]);
    }
    if (fn == "polygon") {
        out.e = 4;
        std::vector<std::string_view> pts;
        splitTop(args, ',', pts);
        if (!pts.empty() && (trim(pts[0]) == "evenodd" || trim(pts[0]) == "nonzero")) {
            out.b = trim(pts[0]) == "evenodd";
            pts.erase(pts.begin());
        }
        if (pts.size() < 3 || pts.size() > std::size_t(kMaxTransitions)) return false;
        for (std::size_t k = 0; k < pts.size(); ++k) {
            std::vector<std::string_view> xy;
            splitTop(trim(pts[k]), ' ', xy);
            if (xy.size() != 2 || !len(xy[0], out.pairs[k][0]) || !len(xy[1], out.pairs[k][1])) return false;
        }
        out.trackCount = u8(pts.size());
        return true;
    }
    return false;
}

bool parsePropValue(Prop p, std::string_view text, PropValue& out) {
    text = trim(text);
    const PropInfo& pi = propInfo(p);
    out = {};
    out.type = pi.type;
    if (p == Prop::ClipPath) return parseClipPath(text, out);
    switch (pi.type) {
    case T::Bool:
        if (p == Prop::Display) { out.b = text != "none"; return true; }
        if (p == Prop::Wrap) { out.b = text == "wrap" || text == "true"; return text == "wrap" || text == "nowrap" || text == "true" || text == "false"; }
        if (p == Prop::PointerEvents) { out.b = text != "none"; return true; }
        if (p == Prop::BackfaceVisibility) { out.b = text == "hidden"; return text == "hidden" || text == "visible"; }
        if (p == Prop::TransformStyle) { out.b = text == "preserve-3d"; return text == "preserve-3d" || text == "flat"; }
        out.b = text == "true" || text == "1" || text == "yes";
        return text == "true" || text == "false" || text == "1" || text == "0" || text == "yes" || text == "no";
    case T::Enum:
        for (const auto& e : kEnums)
            if (e.prop == p && e.word == text) { out.e = e.value; return true; }
        return false;
    case T::Length:
        if (p == Prop::Perspective) { // none | <length> (>= 0)
            if (text == "none") return true;
            return parseLength(text, out.len) && out.len.unit == Unit::Px && out.len.value >= 0.f;
        }
        return parseLength(text, out.len);
    case T::Number: {
        std::string_view unit;
        if (!parseNumber(text, out.num, unit)) return false;
        if (unit == "%") out.num /= 100.f;
        return unit.empty() || unit == "%";
    }
    case T::Color: return parseColor(text, out.color);
    case T::List:
        if (p == Prop::TransformOrigin) return parseTransformOrigin(text, out);
        if (p == Prop::PerspectiveOrigin) return parseTransformOrigin(text, out) && out.trackCount == 2;
        if (p == Prop::ViewTimelineInset) { // [auto | <length-percentage>]{1,2}
            std::string_view w[3];
            int n = 0;
            splitWords(text, w, n, 3);
            if (n < 1 || n > 2) return false;
            for (int i = 0; i < n; ++i) {
                Length l;
                if (w[i] != "auto" && (!parseLength(w[i], l) || l.unit == Unit::Fr)) return false;
                out.pairs[0][std::size_t(i)] = l;
            }
            if (n == 1) out.pairs[0][1] = out.pairs[0][0];
            out.trackCount = 1;
            return true;
        }
        if (p == Prop::ContainerName) { // none | <custom-ident>+ (space separated)
            if (text == "none") return true;
            std::vector<std::string_view> names;
            splitTop(text, ' ', names);
            if (names.empty() || names.size() > std::size_t(kMaxTransitions)) return false;
            for (std::size_t k = 0; k < names.size(); ++k)
                if (!parseListItem(p, names[k], out, u8(k), false)) return false;
            out.trackCount = u8(names.size());
            return true;
        }
        if (p == Prop::TimelineScope) { // none | all | <dashed-ident>#
            if (text == "none") return true;
            if (text == "all") { out.b = true; return true; }
        }
        if (p == Prop::ScrollTimeline || p == Prop::ViewTimeline) { // none | <dashed-ident> [<axis>]
            out.trackCount = 1;
            out.fns[0] = 1;
            if (text == "none") return true;
            std::string_view w[3];
            int n = 0;
            splitWords(text, w, n, 3);
            if (n < 1 || n > 2 || !w[0].starts_with("--") || !isIdent(w[0])) return false;
            out.ids[0] = animationNameId(w[0]);
            return n == 1 || parseAxis(w[1], out.fns[0]);
        }
        return parseList(p, text, out);
    case T::Transform: { // "none" | translate(x[, y]) translateX/Y scale(sx[, sy]) scaleX/Y rotate(a) skew(ax[, ay]) skewX/Y
                         // matrix(a, b, c, d, e, f)
        if (text == "none") return true;
        std::size_t i = 0;
        auto push = [&](TransformFn f, f32 a) { return pushFn(out, f, a); };
        while (i < text.size()) {
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            if (i >= text.size()) break;
            const std::size_t open = text.find('(', i), close = text.find(')', i);
            if (open == std::string_view::npos || close == std::string_view::npos || close < open) return false;
            const std::string_view name = trim(text.substr(i, open - i));
            std::string_view arg = trim(text.substr(open + 1, close - open - 1));
            i = close + 1;
            f32 a[16];
            int n = 0;
            const bool scale = name.starts_with("scale"), anyAngle = name.starts_with("rotate") || name.starts_with("skew"),
                       matrix = name == "matrix" || name == "matrix3d", r3d = name == "rotate3d";
            if (name == "perspective" && arg == "none") { if (!push(TransformFn::Perspective, 0.f)) return false; continue; }
            while (!arg.empty() && n < 16) {
                const bool angle = anyAngle && !(r3d && n < 3);
                std::size_t sep = arg.find_first_of(", ");
                const std::string_view tok = trim(arg.substr(0, sep));
                arg = sep == std::string_view::npos ? std::string_view{} : trim(arg.substr(sep + 1));
                if (!arg.empty() && arg.front() == ',') arg = trim(arg.substr(1));
                std::string_view unit;
                if (!parseNumber(tok, a[n], unit)) return false;
                if (angle) { // <angle> (unitless 0 allowed) -> degrees
                    if (unit == "rad") a[n] *= 57.29577951308232f;
                    else if (unit == "turn") a[n] *= 360.f;
                    else if (unit == "grad") a[n] *= 0.9f;
                    else if (unit != "deg" && !(unit.empty() && a[n] == 0.f)) return false;
                } else if (matrix || r3d) {
                    if (!unit.empty()) return false;
                } else if (scale ? !(unit.empty() || unit == "%") : !(unit.empty() || unit == "px")) return false;
                if (scale && unit == "%") a[n] /= 100.f;
                ++n;
            }
            if (n == 0 || !arg.empty()) return false;
            auto pushMatrix = [&](const f32* m6) {
                for (int q = 0; q < 6; ++q)
                    if (!push(TransformFn(u8(TransformFn::MatA) + q), m6[q])) return false;
                return true;
            };
            bool ok;
            if (name == "translate" && n <= 2) ok = push(TransformFn::TranslateX, a[0]) && push(TransformFn::TranslateY, n > 1 ? a[1] : 0.f);
            else if (name == "translateX" && n == 1) ok = push(TransformFn::TranslateX, a[0]);
            else if (name == "translateY" && n == 1) ok = push(TransformFn::TranslateY, a[0]);
            else if (name == "translateZ" && n == 1) ok = push(TransformFn::TranslateZ, a[0]);
            else if (name == "translate3d" && n == 3)
                ok = push(TransformFn::TranslateX, a[0]) && push(TransformFn::TranslateY, a[1]) && push(TransformFn::TranslateZ, a[2]);
            else if (name == "scaleZ" && n == 1) ok = push(TransformFn::ScaleZ, a[0]);
            else if (name == "scale3d" && n == 3)
                ok = push(TransformFn::ScaleX, a[0]) && push(TransformFn::ScaleY, a[1]) && push(TransformFn::ScaleZ, a[2]);
            else if (name == "rotateX" && n == 1) ok = push(TransformFn::RotateX, a[0]);
            else if (name == "rotateY" && n == 1) ok = push(TransformFn::RotateY, a[0]);
            else if (name == "rotateZ" && n == 1) ok = push(TransformFn::Rotate, a[0]);
            else if (r3d && n == 4)
                ok = push(TransformFn::R3X, a[0]) && push(TransformFn::R3Y, a[1]) && push(TransformFn::R3Z, a[2]) && push(TransformFn::R3A, a[3]);
            else if (name == "perspective" && n == 1 && a[0] >= 0.f) ok = push(TransformFn::Perspective, a[0]);
            else if (name == "matrix3d" && n == 16) {
                ok = true;
                for (int q = 0; q < 16 && ok; ++q) ok = push(TransformFn(u8(TransformFn::M3_0) + q), a[q]);
            }
            else if (name == "scale" && n <= 2) ok = push(TransformFn::ScaleX, a[0]) && push(TransformFn::ScaleY, n > 1 ? a[1] : a[0]);
            else if (name == "scaleX" && n == 1) ok = push(TransformFn::ScaleX, a[0]);
            else if (name == "scaleY" && n == 1) ok = push(TransformFn::ScaleY, a[0]);
            else if (name == "rotate" && n == 1) ok = push(TransformFn::Rotate, a[0]);
            else if (name == "skewX" && n == 1) ok = push(TransformFn::SkewX, a[0]);
            else if (name == "skewY" && n == 1) ok = push(TransformFn::SkewY, a[0]);
            else if (name == "skew" && n <= 2) {
                if (n == 1 || a[1] == 0.f) ok = push(TransformFn::SkewX, a[0]);
                else { // skew(ax, ay) is not skewX(ax) skewY(ay): its own matrix
                    const f32 m6[6] = {1.f, std::tan(a[1] * 0.017453292519943295f), std::tan(a[0] * 0.017453292519943295f), 1.f, 0.f, 0.f};
                    ok = pushMatrix(m6);
                }
            } else if (name == "matrix" && n == 6) ok = pushMatrix(a);
            else ok = false;
            if (!ok) return false;
        }
        return true;
    }
    case T::Filter: { // "none" | fn(<arg>) ... : blur(<len>), brightness / contrast / saturate / grayscale / invert / sepia /
                      // opacity(<number> | <pct>), hue-rotate(<angle>), drop-shadow(<dx> <dy> [<blur>] [<color>]), url(#id)
        if (text == "none") return true;
        static constexpr std::pair<std::string_view, FilterFn> kFns[] = {
            {"blur", FilterFn::Blur}, {"brightness", FilterFn::Brightness}, {"contrast", FilterFn::Contrast},
            {"saturate", FilterFn::Saturate}, {"grayscale", FilterFn::Grayscale}, {"hue-rotate", FilterFn::HueRotate},
            {"invert", FilterFn::Invert}, {"sepia", FilterFn::Sepia}, {"opacity", FilterFn::Opacity},
            {"drop-shadow", FilterFn::DropShadow}, {"url", FilterFn::Url}};
        std::size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            if (i >= text.size()) break;
            const std::size_t open = text.find('(', i);
            if (open == std::string_view::npos || out.trackCount >= kMaxGridTracks) return false;
            std::size_t close = open; // the matching ')' (drop-shadow colours nest: rgba(...))
            for (int depth = 0; close < text.size(); ++close) {
                if (text[close] == '(') ++depth;
                else if (text[close] == ')' && --depth == 0) break;
            }
            if (close >= text.size()) return false;
            const std::string_view name = trim(text.substr(i, open - i)), arg = trim(text.substr(open + 1, close - open - 1));
            i = close + 1;
            bool known = false;
            FilterFn fn{};
            for (const auto& [n, f] : kFns)
                if (n == name) known = true, fn = f;
            if (!known) return false;
            f32 v = 0.f;
            std::string_view unit;
            if (fn == FilterFn::Blur) {
                Length l;
                if (!arg.empty() && !parseLength(arg, l)) return false;
                v = arg.empty() ? 0.f : l.value;
            } else if (fn == FilterFn::HueRotate) {
                if (!arg.empty() && !parseNumber(arg, v, unit)) return false;
                if (unit == "turn") v *= 360.f;
                else if (unit == "rad") v *= 180.f / 3.14159265f;
                else if (unit == "grad") v *= 0.9f;
                else if (!unit.empty() && unit != "deg" && !(v == 0.f)) return false;
            } else if (fn == FilterFn::Url) {
                std::string_view id = arg;
                if (id.size() >= 2 && (id.front() == '"' || id.front() == '\'') && id.back() == id.front()) id = id.substr(1, id.size() - 2);
                if (id.empty() || id.front() != '#') return false;
                const u32 k = filterNameId(id.substr(1));
                std::memcpy(&v, &k, 4);
            } else if (fn == FilterFn::DropShadow) {
                // lengths (2 or 3) and an optional colour, in either order (default: black)
                FilterShadow sh{};
                sh.color = Color{0.f, 0.f, 0.f, 1.f};
                f32 lens[3] = {};
                int nl = 0;
                std::size_t j = 0;
                while (j < arg.size()) {
                    while (j < arg.size() && std::isspace(static_cast<unsigned char>(arg[j]))) ++j;
                    if (j >= arg.size()) break;
                    std::size_t e = j;
                    for (int depth = 0; e < arg.size(); ++e) {
                        if (arg[e] == '(') ++depth;
                        else if (arg[e] == ')') --depth;
                        else if (depth == 0 && std::isspace(static_cast<unsigned char>(arg[e]))) break;
                    }
                    const std::string_view tok = arg.substr(j, e - j);
                    j = e;
                    Length l;
                    if (nl < 3 && parseLength(tok, l)) lens[nl++] = l.value;
                    else if (!parseColor(tok, sh.color)) return false;
                }
                if (nl < 2) return false;
                sh.x = lens[0], sh.y = lens[1], sh.blur = std::max(0.f, lens[2]);
                if (out.shadowCount >= kMaxFilterShadows) return false;
                v = f32(out.shadowCount);
                out.shadows[out.shadowCount++] = sh;
            } else {
                v = 1.f; // CSS default amount
                if (!arg.empty()) {
                    if (!parseNumber(arg, v, unit)) return false;
                    if (unit == "%") v /= 100.f;
                    else if (!unit.empty()) return false;
                }
            }
            out.fns[out.trackCount] = u8(fn);
            out.tracks[out.trackCount].value = v;
            ++out.trackCount;
        }
        return true;
    }
    case T::Stops: { // "<color> [<offset>%], ..." (CSS colour-stop list; missing offsets spread evenly)
        std::vector<std::string_view> items;
        int depth = 0;
        std::size_t s0 = 0;
        for (std::size_t i = 0; i <= text.size(); ++i) {
            if (i < text.size() && text[i] == '(') ++depth;
            if (i < text.size() && text[i] == ')') --depth;
            if (i == text.size() || (text[i] == ',' && depth == 0)) items.push_back(trim(text.substr(s0, i - s0))), s0 = i + 1;
        }
        if (items.size() < 2 || items.size() > std::size_t(kMaxGridTracks)) return false;
        f32 off[kMaxGridTracks];
        bool has[kMaxGridTracks] = {};
        for (std::size_t i = 0; i < items.size(); ++i) {
            std::string_view it = items[i];
            // colour, then an optional trailing offset outside any parentheses
            const std::size_t close = it.rfind(')');
            const std::size_t sp = it.find_last_of(" \t");
            std::string_view col = it;
            if (sp != std::string_view::npos && (close == std::string_view::npos || sp > close)) {
                std::string_view unit;
                f32 v = 0.f;
                if (!parseNumber(trim(it.substr(sp + 1)), v, unit) || (unit != "%" && !unit.empty())) return false;
                off[i] = unit == "%" ? v / 100.f : v, has[i] = true;
                col = trim(it.substr(0, sp));
            }
            if (!parseColor(col, out.stopColors[i])) return false;
        }
        const std::size_t n = items.size();
        if (!has[0]) off[0] = 0.f, has[0] = true;
        if (!has[n - 1]) off[n - 1] = 1.f, has[n - 1] = true;
        for (std::size_t i = 1; i < n; ++i) {
            if (has[i]) continue;
            std::size_t j = i;
            while (!has[j]) ++j;
            for (std::size_t k = i; k < j; ++k) off[k] = off[i - 1] + (off[j] - off[i - 1]) * f32(k - i + 1) / f32(j - i + 1);
            for (std::size_t k = i; k < j; ++k) has[k] = true;
        }
        f32 lo = 0.f;
        for (std::size_t i = 0; i < n; ++i) lo = std::max(lo, off[i]), out.tracks[i] = Length::px(lo); // non-decreasing
        out.trackCount = u8(n);
        return true;
    }
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
    case Prop::GridColumns: s.gridColumns = v.tracks; s.gridColumnCount = v.trackCount; break;
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
    case Prop::GradientAngle: s.gradientAngle = v.num; break;
    case Prop::GradientStops:
        s.gradientStopCount = v.trackCount;
        for (u8 i = 0; i < kMaxGridTracks; ++i) {
            s.gradientOffsets[i] = i < v.trackCount ? v.tracks[i].value : 0.f;
            s.gradientColors[i] = i < v.trackCount ? v.stopColors[i] : Color{};
        }
        break;
    case Prop::ShadowX: s.shadowX = v.len.value; break;
    case Prop::ShadowY: s.shadowY = v.len.value; break;
    case Prop::PointerEvents: s.pointerEvents = v.b; break;
    case Prop::BackgroundClip: s.backgroundClip = fuse::ui::BackgroundClip(v.e); break;
    case Prop::BackdropFilter:
        s.backdropFilterCount = v.trackCount;
        s.backdropBlur = 0.f; // a blur() is an ordered entry of the list
        s.backdropShadows = v.shadows;
        for (u8 k = 0; k < kMaxGridTracks; ++k) {
            s.backdropFilterFns[k] = k < v.trackCount ? FilterFn(v.fns[k]) : FilterFn{};
            s.backdropFilterAmounts[k] = k < v.trackCount ? v.tracks[k].value : 0.f;
        }
        break;
    case Prop::Transform:
        s.transformCount = v.trackCount;
        for (u8 k = 0; k < kMaxTransformArgs; ++k) {
            s.transformFns[k] = k < v.trackCount ? TransformFn(v.xfns[k]) : TransformFn{};
            s.transformArgs[k] = k < v.trackCount ? v.xargs[k] : 0.f;
        }
        break;
    case Prop::Filter:
        s.filterCount = v.trackCount;
        s.filterShadows = v.shadows;
        for (u8 k = 0; k < kMaxGridTracks; ++k) {
            s.filterFns[k] = k < v.trackCount ? FilterFn(v.fns[k]) : FilterFn{};
            s.filterAmounts[k] = k < v.trackCount ? v.tracks[k].value : 0.f;
        }
        break;
    case Prop::TransitionProperty:
        s.transitionPropertyCount = v.trackCount ? v.trackCount : 1; // none: one empty entry
        s.transitionProperty = v.masks;
        break;
    case Prop::TransitionDuration:
        s.transitionDurationCount = v.trackCount;
        for (u8 k = 0; k < kMaxTransitions; ++k) s.transitionDuration[k] = v.tracks[k].value;
        break;
    case Prop::TransitionTimingFunction: s.transitionEaseCount = v.trackCount; s.transitionEase = v.eases; break;
    case Prop::TransitionDelay:
        s.transitionDelayCount = v.trackCount;
        for (u8 k = 0; k < kMaxTransitions; ++k) s.transitionDelay[k] = v.tracks[k].value;
        break;
    case Prop::AnimationName: s.animationNameCount = v.trackCount; s.animationName = v.ids; break;
    case Prop::AnimationDuration:
        s.animationDurationCount = v.trackCount;
        for (u8 k = 0; k < kMaxTransitions; ++k) s.animationDuration[k] = v.tracks[k].value;
        break;
    case Prop::AnimationTimingFunction: s.animationEaseCount = v.trackCount; s.animationEase = v.eases; break;
    case Prop::AnimationDelay:
        s.animationDelayCount = v.trackCount;
        for (u8 k = 0; k < kMaxTransitions; ++k) s.animationDelay[k] = v.tracks[k].value;
        break;
    case Prop::AnimationIterationCount:
        s.animationIterationCount = v.trackCount;
        for (u8 k = 0; k < kMaxTransitions; ++k) s.animationIterations[k] = v.tracks[k].value;
        break;
    case Prop::AnimationDirection:
        s.animationDirectionCount = v.trackCount;
        for (u8 k = 0; k < kMaxTransitions; ++k) s.animationDirection[k] = AnimDirection(v.fns[k]);
        break;
    case Prop::AnimationFillMode:
        s.animationFillCount = v.trackCount;
        for (u8 k = 0; k < kMaxTransitions; ++k) s.animationFill[k] = AnimFill(v.fns[k]);
        break;
    case Prop::AnimationPlayState:
        s.animationPlayCount = v.trackCount;
        for (u8 k = 0; k < kMaxTransitions; ++k) s.animationPaused[k] = v.fns[k];
        break;
    case Prop::TransitionBehavior:
        s.transitionBehaviorCount = v.trackCount ? v.trackCount : 1;
        for (u8 k = 0; k < kMaxTransitions; ++k) s.transitionDiscrete[k] = v.fns[k];
        break;
    case Prop::AnimationComposition:
        s.animationCompositeCount = v.trackCount ? v.trackCount : 1;
        for (u8 k = 0; k < kMaxTransitions; ++k) s.animationComposite[k] = AnimComposite(v.fns[k] <= 2 ? v.fns[k] : 0);
        break;
    case Prop::TransformOrigin:
        if (v.trackCount >= 2) s.transformOrigin = {v.tracks[0], v.tracks[1]};
        else s.transformOrigin = {Length::pct(50.f), Length::pct(50.f)};
        s.transformOriginZ = v.trackCount == 3 ? v.tracks[2].value : 0.f;
        break;
    case Prop::AnimationTimeline:
        s.animationTimelineCount = v.trackCount ? v.trackCount : 1;
        for (u8 k = 0; k < kMaxTransitions; ++k) {
            s.animationTimelineKind[k] = k < v.trackCount ? v.fns[k] : 0;
            s.animationTimelineName[k] = k < v.trackCount ? v.ids[k] : 0;
            s.animationTimelineArg[k] = k < v.trackCount ? u8(v.tracks[k].value) : 1;
            s.animationTimelineInset[k] = k < v.trackCount ? v.pairs[k] : std::array<Length, 2>{};
        }
        break;
    case Prop::ScrollTimeline:
        s.scrollTimelineName = v.trackCount ? v.ids[0] : 0;
        s.scrollTimelineAxis = v.trackCount ? v.fns[0] : 1;
        break;
    case Prop::ViewTimeline:
        s.viewTimelineName = v.trackCount ? v.ids[0] : 0;
        s.viewTimelineAxis = v.trackCount ? v.fns[0] : 1;
        break;
    case Prop::Perspective: s.perspective = v.len.unit == Unit::Px ? std::max(v.len.value, 1.f) : 0.f; break;
    case Prop::PerspectiveOrigin:
        if (v.trackCount == 2) s.perspectiveOrigin = {v.tracks[0], v.tracks[1]};
        else s.perspectiveOrigin = {Length::pct(50.f), Length::pct(50.f)};
        break;
    case Prop::BackfaceVisibility: s.backfaceHidden = v.b; break;
    case Prop::TransformStyle: s.preserve3d = v.b; break;
    case Prop::ViewTimelineInset: s.viewTimelineInset = v.trackCount ? v.pairs[0] : std::array<Length, 2>{}; break;
    case Prop::ContainerType: s.containerType = v.e; break;
    case Prop::ClipPath:
        s.clipShape = v.e;
        s.clipEvenOdd = v.b;
        s.clipCount = v.trackCount;
        s.clipArgs = {};
        if (v.e == 4) for (u8 k = 0; k < v.trackCount && k < kMaxTransitions; ++k) s.clipArgs[2 * k] = v.pairs[k][0], s.clipArgs[2 * k + 1] = v.pairs[k][1];
        else for (u8 k = 0; k < v.trackCount && k < kMaxGridTracks; ++k) s.clipArgs[k] = v.tracks[k];
        break;
    case Prop::ContainerName:
        s.containerNameCount = v.trackCount;
        s.containerName = v.ids;
        break;
    case Prop::TimelineScope:
        s.timelineScopeAll = v.b;
        s.timelineScopeCount = v.trackCount;
        s.timelineScope = v.ids;
        break;
    case Prop::AnimationRangeStart: case Prop::AnimationRangeEnd: {
        const bool end = p == Prop::AnimationRangeEnd;
        (end ? s.animationRangeEndCount : s.animationRangeStartCount) = v.trackCount ? v.trackCount : 1;
        for (u8 k = 0; k < kMaxTransitions; ++k) {
            (end ? s.animationRangeEndName : s.animationRangeStartName)[k] = k < v.trackCount ? v.fns[k] : 0;
            (end ? s.animationRangeEndOffset : s.animationRangeStartOffset)[k] = k < v.trackCount ? v.pairs[k][0] : Length{};
        }
        break;
    }
    case Prop::Count: break;
    }
}

PropValue readProp(const ComputedStyle& s, Prop p) {
    switch (p) {
    case Prop::Display: return PropValue::ofBool(s.visible);
    case Prop::Opacity: return PropValue::ofNumber(s.opacity);
    case Prop::Color: return PropValue::ofColor(s.color);
    case Prop::Background: return PropValue::ofColor(s.background);
    case Prop::BorderColor: return PropValue::ofColor(s.borderColor);
    case Prop::FontSize: return PropValue::ofLength(Length::px(s.fontSize));
    case Prop::Width: return PropValue::ofLength(s.width);
    case Prop::Height: return PropValue::ofLength(s.height);
    case Prop::Grow: return PropValue::ofNumber(s.grow);
    case Prop::Gap: return PropValue::ofLength(Length::px(s.gap));
    case Prop::Layout: return PropValue::ofEnum(u8(s.layout));
    case Prop::Direction: return PropValue::ofEnum(u8(s.direction));
    case Prop::TextAlign: return PropValue::ofEnum(u8(s.textAlign));
    case Prop::BackgroundClip: return PropValue::ofEnum(u8(s.backgroundClip));
    case Prop::MinWidth: return PropValue::ofLength(s.minWidth);
    case Prop::MinHeight: return PropValue::ofLength(s.minHeight);
    case Prop::MaxWidth: return PropValue::ofLength(s.maxWidth);
    case Prop::MaxHeight: return PropValue::ofLength(s.maxHeight);
    case Prop::Basis: return PropValue::ofLength(s.basis);
    case Prop::Shrink: return PropValue::ofNumber(s.shrink);
    case Prop::PaddingLeft: return PropValue::ofLength(Length::px(s.padding.l));
    case Prop::PaddingTop: return PropValue::ofLength(Length::px(s.padding.t));
    case Prop::PaddingRight: return PropValue::ofLength(Length::px(s.padding.r));
    case Prop::PaddingBottom: return PropValue::ofLength(Length::px(s.padding.b));
    case Prop::MarginLeft: return PropValue::ofLength(Length::px(s.margin.l));
    case Prop::MarginTop: return PropValue::ofLength(Length::px(s.margin.t));
    case Prop::MarginRight: return PropValue::ofLength(Length::px(s.margin.r));
    case Prop::MarginBottom: return PropValue::ofLength(Length::px(s.margin.b));
    case Prop::Left: return PropValue::ofLength(s.left);
    case Prop::Top: return PropValue::ofLength(s.top);
    case Prop::Right: return PropValue::ofLength(s.right);
    case Prop::Bottom: return PropValue::ofLength(s.bottom);
    case Prop::Background2: return PropValue::ofColor(s.background2);
    case Prop::BorderWidth: return PropValue::ofLength(Length::px(s.borderWidth));
    case Prop::RadiusTL: return PropValue::ofLength(Length::px(s.radius[0]));
    case Prop::RadiusTR: return PropValue::ofLength(Length::px(s.radius[1]));
    case Prop::RadiusBR: return PropValue::ofLength(Length::px(s.radius[2]));
    case Prop::RadiusBL: return PropValue::ofLength(Length::px(s.radius[3]));
    case Prop::ShadowColor: return PropValue::ofColor(s.shadowColor);
    case Prop::ShadowBlur: return PropValue::ofLength(Length::px(s.shadowBlur));
    case Prop::ShadowX: return PropValue::ofLength(Length::px(s.shadowX));
    case Prop::ShadowY: return PropValue::ofLength(Length::px(s.shadowY));
    case Prop::BackdropBlur: return PropValue::ofLength(Length::px(s.backdropBlur));
    case Prop::Perspective: return PropValue::ofLength(s.perspective > 0.f ? Length::px(s.perspective) : Length{});
    case Prop::Transform: {
        PropValue v;
        v.type = T::Transform;
        v.trackCount = s.transformCount;
        for (u8 k = 0; k < s.transformCount; ++k) v.xfns[k] = u8(s.transformFns[k]), v.xargs[k] = s.transformArgs[k];
        return v;
    }
    default: break;
    }
    PropValue v;
    v.type = propInfo(p).type;
    return v;
}

void inheritFrom(ComputedStyle& s, const ComputedStyle& parent) {
    s.color = parent.color;
    s.fontSize = parent.fontSize;
    s.textAlign = parent.textAlign;
}

// ---- Parser --------------------------------------------------------------------------------------------------------
namespace {

struct Cursor {
    std::string_view s;
    std::size_t i = 0;
    u32 line = 1;
    void skipWs() {
        for (;;) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
                if (s[i] == '\n') ++line;
                ++i;
            }
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*') {
                const auto e = s.find("*/", i + 2);
                for (std::size_t k = i; k < (e == std::string_view::npos ? s.size() : e); ++k)
                    if (s[k] == '\n') ++line;
                i = e == std::string_view::npos ? s.size() : e + 2;
                continue;
            }
            break;
        }
    }
};


// An+B (CSS Syntax 3 §6): odd, even, <int>, [+-]?<int>?n[ [+-] <int>]
bool parseNthImpl(std::string_view t, i32& a, i32& b) {
    std::string w;
    for (char c : t)
        if (!std::isspace(static_cast<unsigned char>(c))) w += char(std::tolower(static_cast<unsigned char>(c)));
    if (w == "odd") { a = 2, b = 1; return true; }
    if (w == "even") { a = 2, b = 0; return true; }
    auto num = [](std::string_view s, i32& out) {
        if (s.empty()) return false;
        std::size_t i = 0;
        bool neg = false;
        if (s[0] == '+' || s[0] == '-') neg = s[0] == '-', ++i;
        if (i >= s.size()) return false;
        i64 v = 0;
        for (; i < s.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
            v = v * 10 + (s[i] - '0');
            if (v > 1000000) return false;
        }
        out = i32(neg ? -v : v);
        return true;
    };
    const auto n = w.find('n');
    if (n == std::string::npos) { a = 0; return num(w, b); }
    const std::string_view as = std::string_view(w).substr(0, n), bs = std::string_view(w).substr(n + 1);
    if (as.empty() || as == "+") a = 1;
    else if (as == "-") a = -1;
    else if (!num(as, a)) return false;
    if (bs.empty()) { b = 0; return true; }
    if (bs[0] != '+' && bs[0] != '-') return false;
    return num(bs, b);
}

void splitSelectors(std::string_view list, std::vector<std::string_view>& out);

// One complex selector into `chain`. isRoot == nullptr: inside :not() / :is() / :where() / :has() (`:root` becomes a
// flag); relative: a :has() argument, which may start with a combinator (the relation to the subject).
bool parseComplex(std::string_view text, std::vector<SimpleSelector>& chainOut, u32& ids, u32& cls, u32& types,
                  bool* isRoot, bool relative, int depth) {
    using C = SimpleSelector::Combinator;
    if (depth > 8) return false;
    struct { std::vector<SimpleSelector>& chain; } rule{chainOut};
    rule.chain.clear();
    if (isRoot) *isRoot = false;
    std::size_t i = 0;
    auto comb = C::None;
    auto isComb = [](char c) { return c == '>' || c == '+' || c == '~'; };
    auto ident = [&](std::size_t from) {
        std::size_t k = from;
        while (k < text.size() && (std::isalnum(static_cast<unsigned char>(text[k])) || text[k] == '-' || text[k] == '_')) ++k;
        return k;
    };
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
            if (!rule.chain.empty() && comb == C::None) comb = C::Descendant;
        }
        if (i >= text.size()) break;
        if (isComb(text[i])) {
            if (rule.chain.empty() && (!relative || comb != C::None)) return false;
            comb = text[i] == '>' ? C::Child : text[i] == '+' ? C::Adjacent : C::Sibling;
            ++i;
            continue;
        }
        SimpleSelector sel;
        sel.combinator = rule.chain.empty() ? (relative ? (comb == C::None ? C::Descendant : comb) : C::None) : comb;
        bool any = false;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && !isComb(text[i])) {
            const char c = text[i];
            if (c == '*') { ++i; any = true; }
            else if (c == '#') { const auto e = ident(i + 1); sel.idHash = hashId(text.substr(i + 1, e - i - 1)); i = e; ++ids; any = true; }
            else if (c == '.') {
                const auto e = ident(i + 1);
                if (sel.classCount >= 4) return false;
                sel.classes[sel.classCount++] = hashId(text.substr(i + 1, e - i - 1));
                i = e; ++cls; any = true;
            } else if (c == '[') {
                // [ name ( op value )? ( i | s )? ]
                std::size_t k = i + 1;
                auto ws = [&] { while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k; };
                ws();
                const std::size_t ne = ident(k);
                if (ne == k) return false;
                SimpleSelector::Attr at;
                at.nameHash = hashId(text.substr(k, ne - k));
                k = ne;
                ws();
                if (k >= text.size()) return false;
                using Op = SimpleSelector::Attr::Op;
                if (text[k] != ']') {
                    static constexpr std::pair<char, Op> kOps[] = {{'~', Op::Word}, {'|', Op::DashPrefix}, {'^', Op::Prefix},
                                                                  {'$', Op::Suffix}, {'*', Op::Contains}};
                    if (text[k] == '=') { at.op = Op::Equals; ++k; }
                    else {
                        bool found = false;
                        for (auto [ch, op] : kOps)
                            if (text[k] == ch && k + 1 < text.size() && text[k + 1] == '=') { at.op = op; k += 2; found = true; }
                        if (!found) return false;
                    }
                    ws();
                    if (k >= text.size()) return false;
                    if (text[k] == '"' || text[k] == '\'') {
                        const char q = text[k];
                        const auto e = text.find(q, k + 1);
                        if (e == std::string_view::npos) return false;
                        at.value.assign(text.substr(k + 1, e - k - 1));
                        k = e + 1;
                    } else {
                        const auto e = ident(k);
                        if (e == k) return false;
                        at.value.assign(text.substr(k, e - k));
                        k = e;
                    }
                    ws();
                    if (k < text.size() && (text[k] == 'i' || text[k] == 'I' || text[k] == 's' || text[k] == 'S')) {
                        at.ci = text[k] == 'i' || text[k] == 'I';
                        ++k;
                        ws();
                    }
                }
                if (k >= text.size() || text[k] != ']') return false;
                if (at.ci) for (char& ch : at.value) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                sel.attrs.push_back(std::move(at));
                i = k + 1; ++cls; any = true;
            } else if (c == ':') {
                const auto e = ident(i + 1);
                const auto name = text.substr(i + 1, e - i - 1);
                i = e; any = true;
                if (name == "root") {
                    if (isRoot) *isRoot = true;
                    else sel.flags |= SimpleSelector::kRoot, ++cls;
                    continue;
                }
                if (name == "empty") { sel.flags |= SimpleSelector::kEmpty; ++cls; continue; }
                if (name == "not" || name == "is" || name == "where" || name == "has" || name == "matches") {
                    using LK = SimpleSelector::Logical::Kind;
                    if (i >= text.size() || text[i] != '(') return false;
                    int d = 0;
                    std::size_t close = i;
                    char quote = 0;
                    for (; close < text.size(); ++close) {
                        const char ch = text[close];
                        if (quote) { if (ch == quote) quote = 0; continue; }
                        if (ch == '"' || ch == '\'') quote = ch;
                        else if (ch == '(') ++d;
                        else if (ch == ')' && --d == 0) break;
                    }
                    if (close >= text.size()) return false;
                    SimpleSelector::Logical lg;
                    lg.kind = name == "not" ? LK::Not : name == "where" ? LK::Where : name == "has" ? LK::Has : LK::Is;
                    std::vector<std::string_view> args;
                    splitSelectors(text.substr(i + 1, close - i - 1), args);
                    u32 best = 0;
                    for (auto a : args) {
                        std::vector<SimpleSelector> ch;
                        u32 ai = 0, ac = 0, at = 0;
                        if (a.empty() || !parseComplex(a, ch, ai, ac, at, nullptr, lg.kind == LK::Has, depth + 1)) {
                            if (lg.kind == LK::Is || lg.kind == LK::Where) continue; // forgiving selector list
                            return false;
                        }
                        best = std::max(best, (std::min(ai, 255u) << 16) | (std::min(ac, 255u) << 8) | std::min(at, 255u));
                        lg.args.push_back(std::move(ch));
                    }
                    if (lg.args.empty() && (lg.kind == LK::Not || lg.kind == LK::Has)) return false;
                    if (lg.kind != LK::Where) ids += best >> 16, cls += (best >> 8) & 255u, types += best & 255u;
                    sel.logicals.push_back(std::move(lg));
                    i = close + 1;
                    continue;
                }
                using NK = SimpleSelector::Nth::Kind;
                auto nth = [&](NK k, i32 a, i32 b) { sel.nths.push_back({k, a, b}); ++cls; };
                if (name == "first-child") { nth(NK::Child, 0, 1); continue; }
                if (name == "last-child") { nth(NK::LastChild, 0, 1); continue; }
                if (name == "only-child") { nth(NK::Child, 0, 1); sel.nths.push_back({NK::LastChild, 0, 1}); continue; }
                if (name == "first-of-type") { nth(NK::OfType, 0, 1); continue; }
                if (name == "last-of-type") { nth(NK::LastOfType, 0, 1); continue; }
                if (name == "only-of-type") { nth(NK::OfType, 0, 1); sel.nths.push_back({NK::LastOfType, 0, 1}); continue; }
                if (name == "nth-child" || name == "nth-last-child" || name == "nth-of-type" || name == "nth-last-of-type") {
                    if (i >= text.size() || text[i] != '(') return false;
                    const auto close = text.find(')', i);
                    if (close == std::string_view::npos) return false;
                    i32 a = 0, b = 0;
                    if (!parseNthImpl(text.substr(i + 1, close - i - 1), a, b)) return false;
                    i = close + 1;
                    nth(name == "nth-child" ? NK::Child : name == "nth-last-child" ? NK::LastChild
                        : name == "nth-of-type" ? NK::OfType : NK::LastOfType, a, b);
                    continue;
                }
                u32 bit = 0;
                if (name == "hover") bit = kStateHover;
                else if (name == "pressed" || name == "active") bit = kStatePressed;
                else if (name == "focus") bit = kStateFocus;
                else if (name == "focus-visible") bit = kStateFocusVisible;
                else if (name == "disabled") bit = kStateDisabled;
                else if (name == "checked") bit = kStateChecked;
                else if (name == "selected") bit = kStateSelected;
                else return false;
                sel.states |= bit;
                ++cls;
            } else if (std::isalpha(static_cast<unsigned char>(c))) {
                const auto e = ident(i);
                sel.typeHash = hashId(text.substr(i, e - i));
                i = e; ++types; any = true;
            } else return false;
        }
        if (!any) return false;
        rule.chain.push_back(std::move(sel));
        comb = C::None;
    }
    if (comb != C::None && comb != C::Descendant) return false; // dangling combinator
    return !rule.chain.empty() || (isRoot && *isRoot);
}

bool parseSelector(std::string_view text, StyleRule& rule, bool& isRoot) {
    u32 ids = 0, cls = 0, types = 0;
    if (!parseComplex(text, rule.chain, ids, cls, types, &isRoot, false, 0)) return false;
    rule.specificity = (std::min(ids, 255u) << 16) | (std::min(cls, 255u) << 8) | std::min(types, 255u);
    return true;
}

void addDecl(StyleRule& rule, Prop p, std::string_view value, u32 line, std::vector<StyleDiagnostic>* diags) {
    Declaration d;
    d.prop = p;
    if (value.find("var(") != std::string_view::npos) {
        d.raw.assign(value);
        rule.decls.push_back(std::move(d));
        return;
    }
    if (!parsePropValue(p, value, d.value)) {
        if (diags) diags->push_back({StyleError::BadValue, line, std::string(propInfo(p).cssName) + ": " + std::string(value)});
        return;
    }
    rule.decls.push_back(std::move(d));
}

void splitWords(std::string_view v, std::string_view* out, int& n, int max) {
    n = 0;
    std::size_t i = 0;
    while (i < v.size() && n < max) {
        while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
        std::size_t s = i;
        int depth = 0;
        while (i < v.size() && (depth > 0 || !std::isspace(static_cast<unsigned char>(v[i])))) {
            if (v[i] == '(') ++depth;
            if (v[i] == ')') --depth;
            ++i;
        }
        if (i > s) out[n++] = v.substr(s, i - s);
    }
}

void addShorthand(StyleRule& rule, std::string_view name, std::string_view value, u32 line,
                  std::vector<StyleDiagnostic>* diags) {
    if (name == "transition" || name == "animation") {
        static constexpr Prop kT[] = {Prop::TransitionProperty, Prop::TransitionDuration, Prop::TransitionTimingFunction,
                                      Prop::TransitionDelay, Prop::TransitionBehavior};
        static constexpr Prop kA[] = {Prop::AnimationName, Prop::AnimationDuration, Prop::AnimationTimingFunction,
                                      Prop::AnimationDelay, Prop::AnimationIterationCount, Prop::AnimationDirection,
                                      Prop::AnimationFillMode, Prop::AnimationPlayState};
        PropValue t[5], an[8];
        const bool isT = name == "transition";
        if (isT ? !parseTransitionShorthand(value, t) : !parseAnimationShorthand(value, an)) {
            if (diags) diags->push_back({StyleError::BadValue, line, std::string(name) + ": " + std::string(value)});
            return;
        }
        for (int k = 0; k < (isT ? 5 : 8); ++k) {
            Declaration d;
            d.prop = isT ? kT[k] : kA[k];
            d.value = isT ? t[k] : an[k];
            rule.decls.push_back(std::move(d));
        }
        return;
    }
    if (name == "container") { // <container-name> [/ <container-type>]?
        const auto slash = value.find('/');
        const std::string_view nm = trim(value.substr(0, slash));
        const std::string_view ty = slash == std::string_view::npos ? std::string_view("normal") : trim(value.substr(slash + 1));
        addDecl(rule, Prop::ContainerName, nm, line, diags);
        addDecl(rule, Prop::ContainerType, ty, line, diags);
        return;
    }
    if (name == "animation-range") { // [<start> <end>?]#
        PropValue st = listValue(PropType::List), en = listValue(PropType::List);
        std::vector<std::string_view> items, words;
        splitTop(value, ',', items);
        bool ok = !items.empty() && items.size() <= std::size_t(kMaxTransitions);
        for (std::size_t k = 0; ok && k < items.size(); ++k) {
            splitTop(items[k], ' ', words);
            const int n = int(words.size());
            const int a = parseRangeWords(words.data(), n, false, st.fns[k], st.pairs[k][0]);
            if (a == 0) { ok = false; break; }
            if (a == n) { // end omitted: the same name at 100% (a name), else normal
                en.fns[k] = st.fns[k];
                en.pairs[k][0] = st.fns[k] ? Length::pct(100.f) : Length{};
            } else if (parseRangeWords(words.data() + a, n - a, true, en.fns[k], en.pairs[k][0]) != n - a) ok = false;
            ++st.trackCount, ++en.trackCount;
        }
        if (!ok) {
            if (diags) diags->push_back({StyleError::BadValue, line, std::string(name) + ": " + std::string(value)});
            return;
        }
        Declaration d;
        d.prop = Prop::AnimationRangeStart, d.value = st;
        rule.decls.push_back(d);
        d.prop = Prop::AnimationRangeEnd, d.value = en;
        rule.decls.push_back(std::move(d));
        return;
    }
    std::string_view w[4];
    int n = 0;
    auto four = [&](Prop t, Prop r, Prop b, Prop l) {
        splitWords(value, w, n, 4);
        if (n == 0) return;
        std::string_view top = w[0], right = n > 1 ? w[1] : w[0], bottom = n > 2 ? w[2] : w[0],
                         left = n > 3 ? w[3] : (n > 1 ? w[1] : w[0]);
        addDecl(rule, t, top, line, diags);
        addDecl(rule, r, right, line, diags);
        addDecl(rule, b, bottom, line, diags);
        addDecl(rule, l, left, line, diags);
    };
    if (name == "padding") four(Prop::PaddingTop, Prop::PaddingRight, Prop::PaddingBottom, Prop::PaddingLeft);
    else if (name == "margin") four(Prop::MarginTop, Prop::MarginRight, Prop::MarginBottom, Prop::MarginLeft);
    else if (name == "border-radius") four(Prop::RadiusTL, Prop::RadiusTR, Prop::RadiusBR, Prop::RadiusBL);
    else if (name == "border") {
        splitWords(value, w, n, 4);
        for (int k = 0; k < n; ++k) {
            PropValue tmp;
            if (parsePropValue(Prop::BorderWidth, w[k], tmp)) addDecl(rule, Prop::BorderWidth, w[k], line, diags);
            else if (w[k] == "solid" || w[k] == "none") continue;
            else addDecl(rule, Prop::BorderColor, w[k], line, diags);
        }
    } else if (name == "flex") {
        splitWords(value, w, n, 3);
        if (n > 0) addDecl(rule, Prop::Grow, w[0], line, diags);
        if (n > 1) addDecl(rule, Prop::Shrink, w[1], line, diags);
        if (n > 2) addDecl(rule, Prop::Basis, w[2], line, diags);
    } else if (name == "box-shadow") { // <x> <y> <blur> <color>
        splitWords(value, w, n, 4);
        if (n == 4) {
            addDecl(rule, Prop::ShadowX, w[0], line, diags);
            addDecl(rule, Prop::ShadowY, w[1], line, diags);
            addDecl(rule, Prop::ShadowBlur, w[2], line, diags);
            addDecl(rule, Prop::ShadowColor, w[3], line, diags);
        } else if (diags) diags->push_back({StyleError::BadValue, line, "box-shadow"});
    } else if (name == "background" && value.starts_with("linear-gradient")) {
        // linear-gradient(vertical|horizontal, c0, c1)
        const auto open = value.find('('), close = value.rfind(')');
        std::string_view args = value.substr(open + 1, close - open - 1);
        std::string_view parts[3];
        int k = 0;
        int depth = 0;
        std::size_t s = 0;
        for (std::size_t i = 0; i <= args.size() && k < 3; ++i) {
            if (i < args.size() && args[i] == '(') ++depth;
            if (i < args.size() && args[i] == ')') --depth;
            if (i == args.size() || (args[i] == ',' && depth == 0)) {
                parts[k++] = trim(args.substr(s, i - s));
                s = i + 1;
            }
        }
        const bool legacy = k == 3 && s > args.size() && (parts[0] == "vertical" || parts[0] == "horizontal" || parts[0] == "to right" || parts[0] == "to bottom");
        // CSS form: linear-gradient([<angle> | to <side-or-corner>,] <color-stop>, <color-stop>, ...)
        f32 angle = 180.f;
        std::string_view stops = args;
        bool angleOk = !legacy;
        if (!legacy) {
            const std::size_t comma = [&] {
                int d = 0;
                for (std::size_t i = 0; i < args.size(); ++i) {
                    if (args[i] == '(') ++d;
                    else if (args[i] == ')') --d;
                    else if (args[i] == ',' && d == 0) return i;
                }
                return args.size();
            }();
            const std::string_view first = trim(args.substr(0, comma));
            std::string_view unit;
            f32 v = 0.f;
            if (first.starts_with("to ")) {
                const bool t = first.find("top") != std::string_view::npos, b = first.find("bottom") != std::string_view::npos;
                const bool l = first.find("left") != std::string_view::npos, r = first.find("right") != std::string_view::npos;
                if (t && r) angle = 45.f; else if (b && r) angle = 135.f; else if (b && l) angle = 225.f; else if (t && l) angle = 315.f;
                else if (t) angle = 0.f; else if (r) angle = 90.f; else if (b) angle = 180.f; else if (l) angle = 270.f;
                else angleOk = false;
                stops = comma < args.size() ? args.substr(comma + 1) : std::string_view{};
            } else if (parseNumber(first, v, unit) && (unit == "deg" || unit == "rad" || unit == "turn" || unit == "grad")) {
                angle = unit == "deg" ? v : unit == "rad" ? v * 57.29577951f : unit == "turn" ? v * 360.f : v * 0.9f;
                stops = comma < args.size() ? args.substr(comma + 1) : std::string_view{};
            }
        }
        if (angleOk) {
            PropValue pv;
            if (parsePropValue(Prop::GradientStops, stops, pv)) {
                char ab[32];
                std::snprintf(ab, sizeof ab, "%g", double(angle));
                addDecl(rule, Prop::Gradient, "angle", line, diags);
                addDecl(rule, Prop::GradientAngle, ab, line, diags);
                addDecl(rule, Prop::GradientStops, stops, line, diags);
                // first / last colours keep a 2-stop fallback for code that reads background / background-end
                Declaration d0;
                d0.prop = Prop::Background, d0.value = PropValue::ofColor(pv.stopColors[0]);
                rule.decls.push_back(d0);
                d0.prop = Prop::Background2, d0.value = PropValue::ofColor(pv.stopColors[pv.trackCount - 1]);
                rule.decls.push_back(d0);
            } else if (diags) diags->push_back({StyleError::BadValue, line, "linear-gradient"});
        } else if (k == 3) {
            addDecl(rule, Prop::Gradient, parts[0] == "to right" ? "horizontal" : (parts[0] == "to bottom" ? "vertical" : parts[0]), line, diags);
            addDecl(rule, Prop::Background, parts[1], line, diags);
            addDecl(rule, Prop::Background2, parts[2], line, diags);
        } else if (diags) diags->push_back({StyleError::BadValue, line, "linear-gradient"});
    } else if (diags) diags->push_back({StyleError::UnknownProperty, line, std::string(name)});
}

} // namespace

bool parseNth(std::string_view t, i32& a, i32& b) { return parseNthImpl(t, a, b); }

namespace {
void parseDecls(std::string_view body, u32 line, StyleRule& proto, std::vector<std::pair<u32, std::string>>* tokens,
                std::vector<StyleDiagnostic>* diags) {
    while (!body.empty()) {
        const auto semi = body.find(';');
        std::string_view decl = trim(body.substr(0, semi));
        if (!decl.empty()) {
            const auto colon = decl.find(':');
            if (colon == std::string_view::npos) {
                if (diags) diags->push_back({StyleError::Syntax, line, std::string(decl)});
            } else {
                const auto name = trim(decl.substr(0, colon));
                const auto value = trim(decl.substr(colon + 1));
                Prop p;
                if (name.starts_with("--")) {
                    if (tokens) {
                        Declaration d;
                        d.customHash = hashId(name);
                        d.raw.assign(value);
                        tokens->emplace_back(d.customHash, d.raw);
                        proto.decls.push_back(std::move(d));
                    }
                } else if (!(name == "background" && value.starts_with("linear-gradient")) && findProp(name, p)) addDecl(proto, p, value, line, diags);
                else addShorthand(proto, name, value, line, diags);
            }
        }
        if (semi == std::string_view::npos) break;
        body.remove_prefix(semi + 1);
    }
}
// Top-level comma split that respects (), [] and quotes
void splitSelectors(std::string_view list, std::vector<std::string_view>& out) {
    out.clear();
    int depth = 0;
    char quote = 0;
    std::size_t s = 0;
    for (std::size_t i = 0; i <= list.size(); ++i) {
        if (i < list.size()) {
            const char c = list[i];
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '"' || c == '\'') { quote = c; continue; }
            if (c == '(' || c == '[') ++depth;
            if (c == ')' || c == ']') --depth;
            if (c != ',' || depth > 0) continue;
        }
        out.push_back(trim(list.substr(s, i - s)));
        s = i + 1;
    }
}
} // namespace

namespace {
std::atomic<u32> g_anonLayer{0};
}
// ---- sheet parser: style rules with CSS nesting, @layer, @media, @container, @supports, @keyframes -----------------
struct SheetParser {
    std::string text; // comments blanked (offsets kept)
    std::vector<std::size_t> nl;
    StyleSheet& sheet;
    std::vector<StyleDiagnostic>* diags;
    u32 order = 0;
    std::vector<std::string_view> list;
    u32 lineAt(std::size_t pos) const { return 1 + u32(std::upper_bound(nl.begin(), nl.end(), pos) - nl.begin()) - (pos < text.size() && text[pos] == '\n' ? 1 : 0); }
    void diag(StyleError e, std::size_t pos, std::string msg) { if (diags) diags->push_back({e, lineAt(pos), std::move(msg)}); }
    // the matching '}' of the '{' at open (quotes / parens respected); npos when unbalanced
    std::size_t matchBrace(std::size_t open) const {
        int depth = 0;
        char quote = 0;
        for (std::size_t k = open; k < text.size(); ++k) {
            const char c = text[k];
            if (quote) { if (c == '\\') ++k; else if (c == quote) quote = 0; continue; }
            if (c == '"' || c == '\'') quote = c;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) return k;
        }
        return std::string::npos;
    }
    // next ';', '{' or '}' at paren depth 0 from i (end: e)
    std::size_t nextStop(std::size_t i, std::size_t e) const {
        int paren = 0;
        char quote = 0;
        for (std::size_t k = i; k < e; ++k) {
            const char c = text[k];
            if (quote) { if (c == '\\') ++k; else if (c == quote) quote = 0; continue; }
            if (c == '"' || c == '\'') quote = c;
            else if (c == '(') ++paren;
            else if (c == ')') --paren;
            else if (paren <= 0 && (c == ';' || c == '{' || c == '}')) return k;
        }
        return e;
    }
    u16 layerIndex(const std::string& full) {
        // declaring "a.b" declares "a" first
        for (std::size_t dot = full.find('.'); dot != std::string::npos; dot = full.find('.', dot + 1)) layerIndex(full.substr(0, dot));
        auto& L = sheet.m_layers;
        for (std::size_t k = 0; k < L.size(); ++k)
            if (L[k] == full) return u16(k);
        L.push_back(full);
        return u16(L.size() - 1);
    }
    static std::string joinLayer(const std::string& prefix, std::string_view name) { return prefix.empty() ? std::string(name) : prefix + "." + std::string(name); }
    bool parseKeyframes(std::string_view prelude, std::string_view inner, u32 line);
    bool parseQuery(std::string_view q, StyleCondition& c, bool container);
    bool supports(std::string_view cond);
    // nested selectors: `&` is the parent (textually; each parent alternative); no `&` = descendant of the parent
    void resolve(std::string_view prelude, const std::vector<std::string>& parents, std::vector<std::string>& out) {
        std::vector<std::string_view> parts;
        splitSelectors(prelude, parts);
        out.clear();
        for (auto part : parts) {
            if (part.empty()) continue;
            if (parents.empty()) { out.emplace_back(part); continue; }
            for (const std::string& par : parents) {
                if (part.find('&') == std::string_view::npos) { out.push_back(par + " " + std::string(part)); continue; }
                std::string r;
                for (char c : part) c == '&' ? r += par : r += c;
                out.push_back(std::move(r));
            }
        }
    }
    bool block(std::size_t b, std::size_t e, const std::vector<std::string>& sels, u16 layer, u16 cond, const std::string& layerName, int depth);
    void emit(const std::vector<std::string>& sels, std::string_view decls, u32 line, u16 layer, u16 cond) {
        StyleRule proto;
        std::vector<std::pair<u32, std::string>> tokens;
        parseDecls(decls, line, proto, &tokens, diags);
        for (const std::string& one : sels) {
            StyleRule rule;
            bool isRoot = false;
            if (!parseSelector(one, rule, isRoot)) {
                if (diags) diags->push_back({StyleError::Syntax, line, "bad selector: " + one});
                continue;
            }
            rule.decls = proto.decls;
            rule.order = order++;
            rule.root = isRoot;
            rule.layer = layer;
            rule.cond = cond;
            if (isRoot) for (auto& t : tokens) sheet.m_tokens.push_back(t);
            if (!rule.chain.empty() || isRoot) sheet.m_rules.push_back(std::move(rule));
        }
    }
};
bool SheetParser::parseKeyframes(std::string_view selectors, std::string_view innerIn, u32 line) {
    StyleSheet& sheet = this->sheet;
    std::string_view name = trim(selectors.substr(selectors.find(' ') == std::string_view::npos ? selectors.size() : selectors.find(' ')));
    if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') && name.back() == name.front()) name = name.substr(1, name.size() - 2);
    if (name.empty() || name == "none") {
        if (diags) diags->push_back({StyleError::Syntax, line, "bad @keyframes name"});
        return false;
    }
    KeyframesRule kr;
    kr.name = animationNameId(name);
    std::string_view inner = innerIn;
    while (!trim(inner).empty()) {
        const auto ob = inner.find('{'), cb = inner.find('}');
        if (ob == std::string_view::npos || cb == std::string_view::npos || cb < ob) {
            if (diags) diags->push_back({StyleError::Syntax, line, "bad keyframe block"});
            break;
        }
        StyleRule proto;
        parseDecls(inner.substr(ob + 1, cb - ob - 1), line, proto, nullptr, diags);
        Keyframe kf;
        for (auto& d : proto.decls) {
            if (d.prop == Prop::AnimationTimingFunction) {
                if (d.value.trackCount == 1) kf.hasEase = true, kf.ease = d.value.eases[0];
            } else if (d.prop != Prop::Count && propInfo(d.prop).animatable) kf.decls.push_back(std::move(d)); // others ignored (CSS Animations 1 §3)
        }
        splitSelectors(trim(inner.substr(0, ob)), list);
        for (auto sel : list) {
            f32 off = -1.f;
            std::string_view unit;
            if (sel == "from") off = 0.f;
            else if (sel == "to") off = 1.f;
            else if (parseNumber(sel, off, unit) && unit == "%" && off >= 0.f && off <= 100.f) off /= 100.f;
            else off = -1.f;
            if (off < 0.f) {
                if (diags) diags->push_back({StyleError::Syntax, line, "bad keyframe selector: " + std::string(sel)});
                continue;
            }
            kf.offset = off;
            kr.frames.push_back(kf);
        }
        inner.remove_prefix(cb + 1);
    }
    std::stable_sort(kr.frames.begin(), kr.frames.end(), [](const Keyframe& x, const Keyframe& y) { return x.offset < y.offset; });
    sheet.m_keyframes.push_back(std::move(kr));
    return true;
}
// one query (no top-level commas): [not|only] [all|screen|print] [and] (feature) [and (feature)]...
bool SheetParser::parseQuery(std::string_view q, StyleCondition& c, bool container) {
    using F = StyleCondition::Feature;
    using O = StyleCondition::Op;
    StyleCondition::Query qu;
    q = trim(q);
    auto word = [&](std::string_view w) {
        if (q.size() >= w.size() && q.substr(0, w.size()) == w && (q.size() == w.size() || q[w.size()] == ' ' || q[w.size()] == '(')) {
            q = trim(q.substr(w.size()));
            return true;
        }
        return false;
    };
    if (word("not")) qu.negate = true;
    else word("only");
    if (!container) {
        if (word("print")) qu.all.push_back({F::Never});
        else if (!word("all")) word("screen");
        word("and");
    }
    auto length = [&](std::string_view v, f32& out) {
        f32 n = 0.f;
        std::string_view u;
        if (!parseNumber(trim(v), n, u)) return false;
        if (u == "px" || (u.empty() && n == 0.f)) out = n;
        else if (u == "em" || u == "rem") out = n * 16.f;
        else return false;
        return true;
    };
    while (!q.empty()) {
        if (q.front() != '(') return false;
        int d = 0;
        std::size_t close = 0;
        for (; close < q.size(); ++close) {
            if (q[close] == '(') ++d;
            else if (q[close] == ')' && --d == 0) break;
        }
        if (close >= q.size()) return false;
        std::string_view f = trim(q.substr(1, close - 1));
        q = trim(q.substr(close + 1));
        if (!q.empty() && !word("and")) return false;
        // name: value | range syntax (width >= 600px, 400px <= width <= 700px)
        if (const auto colon = f.find(':'); colon != std::string_view::npos) {
            std::string_view name = trim(f.substr(0, colon)), val = trim(f.substr(colon + 1));
            O op = O::Eq;
            if (name.starts_with("min-")) op = O::Ge, name.remove_prefix(4);
            else if (name.starts_with("max-")) op = O::Le, name.remove_prefix(4);
            if (name == "inline-size") name = "width";
            if (name == "block-size") name = "height";
            if (name == "width" || name == "height") {
                StyleCondition::Test t{name == "width" ? F::Width : F::Height, op, 0.f};
                if (!length(val, t.v)) return false;
                qu.all.push_back(t);
            } else if (name == "aspect-ratio") {
                const auto sl = val.find('/');
                f32 a = 0.f, b = 1.f;
                std::string_view u;
                if (!parseNumber(trim(val.substr(0, sl)), a, u) || (sl != std::string_view::npos && !parseNumber(trim(val.substr(sl + 1)), b, u)) || b <= 0.f) return false;
                qu.all.push_back({F::AspectRatio, op, a / b});
            } else if (name == "orientation" && op == O::Eq) {
                if (val != "landscape" && val != "portrait") return false;
                qu.all.push_back({F::Landscape, O::Eq, val == "landscape" ? 1.f : 0.f});
            } else if (!container && name == "prefers-color-scheme" && op == O::Eq) {
                if (val != "dark" && val != "light") return false;
                qu.all.push_back({F::Dark, O::Eq, val == "dark" ? 1.f : 0.f});
            } else if (!container && name == "prefers-reduced-motion" && op == O::Eq) {
                if (val != "reduce" && val != "no-preference") return false;
                qu.all.push_back({F::ReducedMotion, O::Eq, val == "reduce" ? 1.f : 0.f});
            } else return false;
            continue;
        }
        // range: a op name [op b]
        std::string_view toks[5];
        int n = 0;
        for (std::size_t i = 0; i < f.size() && n < 5;) {
            while (i < f.size() && f[i] == ' ') ++i;
            if (i >= f.size()) break;
            std::size_t j = i;
            if (f[i] == '<' || f[i] == '>' || f[i] == '=') { j = i + 1; if (j < f.size() && f[j] == '=') ++j; }
            else while (j < f.size() && f[j] != ' ' && f[j] != '<' && f[j] != '>' && f[j] != '=') ++j;
            toks[n++] = f.substr(i, j - i);
            i = j;
        }
        auto opOf = [](std::string_view o, bool flip, O& out) {
            if (o == "=") { out = O::Eq; return true; }
            const bool lt = o[0] == '<', eq = o.size() == 2;
            const bool less = flip ? !lt : lt; // name on the right flips the sense
            out = less ? (eq ? O::Le : O::Lt) : (eq ? O::Ge : O::Gt);
            return true;
        };
        auto feat = [&](std::string_view nm, F& out) {
            if (nm == "width" || nm == "inline-size") out = F::Width;
            else if (nm == "height" || nm == "block-size") out = F::Height;
            else return false;
            return true;
        };
        F ft;
        StyleCondition::Test t;
        if (n == 3 && feat(toks[0], ft)) { // width >= 600px
            t.f = ft;
            if (!opOf(toks[1], false, t.op) || !length(toks[2], t.v)) return false;
            qu.all.push_back(t);
        } else if ((n == 3 || n == 5) && feat(toks[2], ft)) { // 600px <= width [<= 900px]
            t.f = ft;
            if (!opOf(toks[1], true, t.op) || !length(toks[0], t.v)) return false;
            qu.all.push_back(t);
            if (n == 5) {
                StyleCondition::Test t2{ft, O::Eq, 0.f};
                if (!opOf(toks[3], false, t2.op) || !length(toks[4], t2.v)) return false;
                qu.all.push_back(t2);
            }
        } else return false;
    }
    c.any.push_back(std::move(qu));
    return true;
}
// @supports: (prop: value), not / and / or, selector(...)
bool SheetParser::supports(std::string_view cond) {
    cond = trim(cond);
    if (cond.starts_with("not ")) return !supports(cond.substr(4));
    // split at top-level " and " / " or "
    int d = 0;
    for (std::size_t k = 0; k < cond.size(); ++k) {
        if (cond[k] == '(') ++d;
        else if (cond[k] == ')') --d;
        else if (d == 0 && cond[k] == ' ') {
            const std::string_view rest = cond.substr(k + 1);
            if (rest.starts_with("and ")) return supports(cond.substr(0, k)) && supports(rest.substr(4));
            if (rest.starts_with("or ")) return supports(cond.substr(0, k)) || supports(rest.substr(3));
        }
    }
    if (cond.starts_with("selector(") && cond.back() == ')') {
        StyleRule r;
        bool root = false;
        return parseSelector(trim(cond.substr(9, cond.size() - 10)), r, root);
    }
    if (cond.size() < 2 || cond.front() != '(' || cond.back() != ')') return false;
    const std::string_view in = trim(cond.substr(1, cond.size() - 2));
    if (in.starts_with("(") || in.starts_with("not ") || in.starts_with("selector(")) return supports(in);
    const auto colon = in.find(':');
    if (colon == std::string_view::npos) return false;
    const std::string_view name = trim(in.substr(0, colon)), val = trim(in.substr(colon + 1));
    if (name.starts_with("--")) return true;
    Prop pr;
    if (findProp(name, pr)) {
        PropValue v;
        return parsePropValue(pr, val, v);
    }
    StyleRule probe; // shorthands: supported when they expand to something
    addShorthand(probe, name, val, 0, nullptr);
    return !probe.decls.empty();
}
bool SheetParser::block(std::size_t b, std::size_t e, const std::vector<std::string>& sels, u16 layer, u16 cond, const std::string& layerName, int depth) {
    if (depth > 32) { diag(StyleError::Syntax, b, "nesting too deep"); return true; }
    struct Nested { std::size_t pre, open, close; };
    std::vector<Nested> nested;
    std::string decls;
    std::size_t i = b, firstDecl = std::string::npos;
    while (i < e) {
        while (i < e && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= e) break;
        const std::size_t j = nextStop(i, e);
        if (j >= e || text[j] != '{') {
            const std::string_view stmt = trim(std::string_view(text).substr(i, j - i));
            if (stmt.starts_with("@layer")) { // statement: declares the order
                std::vector<std::string_view> names;
                splitSelectors(trim(stmt.substr(6)), names);
                for (auto nm : names) if (!nm.empty()) layerIndex(joinLayer(layerName, nm));
            } else if (stmt.starts_with("@")) {
                if (!stmt.starts_with("@import") && !stmt.starts_with("@charset") && !stmt.starts_with("@namespace"))
                    diag(StyleError::Syntax, i, "unsupported at-rule: " + std::string(stmt));
            } else if (!stmt.empty()) {
                if (sels.empty()) diag(StyleError::Syntax, i, "declaration outside a rule: " + std::string(stmt));
                else {
                    if (firstDecl == std::string::npos) firstDecl = i;
                    decls.append(stmt).push_back(';');
                }
            }
            if (j < e && text[j] == '}') { diag(StyleError::Syntax, j, "unexpected '}'"); }
            i = j + 1;
            continue;
        }
        const std::size_t close = matchBrace(j);
        if (close == std::string::npos || close > e) return false;
        nested.push_back({i, j, close});
        i = close + 1;
    }
    // the rule's own declarations come before its nested rules
    if (!sels.empty()) emit(sels, decls, lineAt(firstDecl == std::string::npos ? b : firstDecl), layer, cond);
    std::vector<std::string> sub;
    for (const Nested& n : nested) {
        const std::string_view pre = trim(std::string_view(text).substr(n.pre, n.open - n.pre));
        const std::string_view inner = std::string_view(text).substr(n.open + 1, n.close - n.open - 1);
        const u32 line = lineAt(n.pre);
        auto at = [&](std::string_view kw) { return pre.size() >= kw.size() && pre.substr(0, kw.size()) == kw && (pre.size() == kw.size() || !std::isalnum(static_cast<unsigned char>(pre[kw.size()])) && pre[kw.size()] != '-'); };
        if (at("@keyframes") || at("@-webkit-keyframes")) {
            parseKeyframes(pre, inner, line);
        } else if (at("@media") || at("@container")) {
            const bool ctr = at("@container");
            StyleCondition c;
            c.kind = ctr ? StyleCondition::Kind::Container : StyleCondition::Kind::Media;
            c.parent = cond;
            std::string_view q = trim(pre.substr(ctr ? 10 : 6));
            if (ctr && !q.empty() && q.front() != '(' && !q.starts_with("not ")) { // a container name
                const auto sp = q.find_first_of(" (");
                c.containerName = animationNameId(q.substr(0, sp));
                q = sp == std::string_view::npos ? std::string_view{} : trim(q.substr(sp));
            }
            std::vector<std::string_view> qs;
            splitSelectors(q, qs);
            bool ok = !qs.empty() || !ctr;
            if (qs.empty() && !ctr) c.any.push_back({});
            for (auto one : qs) ok = ok && parseQuery(one, c, ctr);
            if (!ok) { // an unknown query is false (Media Queries 4 §3.2): drop the block
                diag(StyleError::BadValue, n.pre, "unsupported query: " + std::string(pre));
                continue;
            }
            sheet.m_conds.push_back(std::move(c));
            if (!block(n.open + 1, n.close, sels, layer, u16(sheet.m_conds.size() - 1), layerName, depth + 1)) return false;
        } else if (at("@supports")) {
            if (supports(pre.substr(9)) && !block(n.open + 1, n.close, sels, layer, cond, layerName, depth + 1)) return false;
        } else if (at("@layer")) {
            std::string_view nm = trim(pre.substr(6));
            const std::string full = joinLayer(layerName, nm.empty() ? "\x01anon" + std::to_string(g_anonLayer++) : std::string(nm));
            if (!block(n.open + 1, n.close, sels, layerIndex(full), cond, full, depth + 1)) return false;
        } else if (pre.starts_with("@")) {
            diag(StyleError::Syntax, n.pre, "unsupported at-rule: " + std::string(pre));
        } else {
            resolve(pre, sels, sub);
            const std::vector<std::string> mine = sub;
            if (!block(n.open + 1, n.close, mine, layer, cond, layerName, depth + 1)) return false;
        }
    }
    return true;
}

std::expected<StyleSheet, StyleDiagnostic> StyleSheet::parse(std::string_view css, std::vector<StyleDiagnostic>* diags) {
    StyleSheet sheet;
    SheetParser p{std::string(css), {}, sheet, diags};
    for (std::size_t k = 0; k < p.text.size(); ++k) {
        if (p.text[k] == '\n') p.nl.push_back(k);
        if (p.text[k] == '/' && k + 1 < p.text.size() && p.text[k + 1] == '*') { // blank comments
            const std::size_t end = p.text.find("*/", k + 2);
            const std::size_t stop = end == std::string::npos ? p.text.size() : end + 2;
            for (std::size_t m = k; m < stop; ++m) if (p.text[m] == '\n') p.nl.push_back(m); else p.text[m] = ' ';
            k = stop - 1;
        }
    }
    // unbalanced braces: the old parser's errors
    int depth = 0;
    for (std::size_t k = 0; k < p.text.size(); ++k) {
        if (p.text[k] == '{') ++depth;
        else if (p.text[k] == '}') --depth;
    }
    if (depth > 0) return std::unexpected(StyleDiagnostic{StyleError::Syntax, p.lineAt(p.text.size()), "expected '}'"});
    // top level: anything not ending in a block is a syntax error ("expected '{'")
    {
        std::size_t i = 0;
        for (;;) {
            while (i < p.text.size() && std::isspace(static_cast<unsigned char>(p.text[i]))) ++i;
            if (i >= p.text.size()) break;
            const std::size_t j = p.nextStop(i, p.text.size());
            if (j >= p.text.size() || p.text[j] != '{') {
                const std::string_view stmt = trim(std::string_view(p.text).substr(i, j - i));
                if (!stmt.starts_with("@")) return std::unexpected(StyleDiagnostic{StyleError::Syntax, p.lineAt(i), "expected '{'"});
                i = j + 1;
                continue;
            }
            const std::size_t close = p.matchBrace(j);
            if (close == std::string::npos) return std::unexpected(StyleDiagnostic{StyleError::Syntax, p.lineAt(j), "expected '}'"});
            i = close + 1;
        }
    }
    if (!p.block(0, p.text.size(), {}, StyleRule::kNone, StyleRule::kNone, {}, 0))
        return std::unexpected(StyleDiagnostic{StyleError::Syntax, 1, "expected '}'"});
    return sheet;
}

namespace {
void scanChain(const std::vector<SimpleSelector>& chain, bool& structural, bool& relational) {
    for (const auto& sel : chain) {
        if (!sel.nths.empty() || (sel.flags & SimpleSelector::kEmpty) || sel.combinator == SimpleSelector::Combinator::Adjacent ||
            sel.combinator == SimpleSelector::Combinator::Sibling)
            structural = true;
        for (const auto& lg : sel.logicals) {
            if (lg.kind == SimpleSelector::Logical::Kind::Has) relational = structural = true;
            for (const auto& a : lg.args) scanChain(a, structural, relational);
        }
    }
}
} // namespace
bool StyleSheet::structural() const {
    bool st = false, rel = false;
    for (const auto& r : m_rules) scanChain(r.chain, st, rel);
    return st;
}
bool StyleSheet::relational() const {
    bool st = false, rel = false;
    for (const auto& r : m_rules) scanChain(r.chain, st, rel);
    return rel;
}

bool StyleSheet::token(u32 h, std::string_view& out) const {
    for (auto it = m_tokens.rbegin(); it != m_tokens.rend(); ++it)
        if (it->first == h) { out = it->second; return true; }
    return false;
}

// ---- Engine --------------------------------------------------------------------------------------------------------
StyleEngine::StyleEngine() {
    m_table.reserve(256);
    m_hashIndex.assign(1024, 0);
    m_scratch.reserve(64);
    m_localTokens.reserve(16);
    intern(ComputedStyle{});
}

void StyleEngine::clearSheets() {
    m_sheets.clear();
    m_ordered.clear();
    m_orderedCond.clear();
    m_orderedSheet.clear();
    m_mediaOk.clear();
    m_condBase.clear();
    m_hasContainer = false;
    m_structural = m_relational = false;
    buildInvalidation();
    ++m_version;
}

void StyleEngine::addSheet(StyleSheet sheet) {
    m_sheets.push_back(std::move(sheet));
    reorder();
}

bool StyleEngine::replaceSheet(u32 index, StyleSheet sheet) {
    if (index >= m_sheets.size()) return false;
    m_sheets[index] = std::move(sheet);
    reorder();
    return true;
}

bool evalCondition(const StyleCondition& c, f32 w, f32 h, const MediaEnv& env) {
    using F = StyleCondition::Feature;
    using O = StyleCondition::Op;
    for (const auto& q : c.any) {
        bool ok = true;
        for (const auto& t : q.all) {
            f32 v = 0.f;
            switch (t.f) {
            case F::Always: v = t.v; break;
            case F::Never: ok = false; break;
            case F::Width: v = w; break;
            case F::Height: v = h; break;
            case F::AspectRatio: v = h > 0.f ? w / h : 0.f; break;
            case F::Landscape: v = w > h ? 1.f : 0.f; break; // portrait when height >= width
            case F::Dark: v = env.dark ? 1.f : 0.f; break;
            case F::ReducedMotion: v = env.reducedMotion ? 1.f : 0.f; break;
            }
            if (!ok) break;
            switch (t.op) {
            case O::Ge: ok = v >= t.v; break;
            case O::Le: ok = v <= t.v; break;
            case O::Gt: ok = v > t.v; break;
            case O::Lt: ok = v < t.v; break;
            case O::Eq: ok = t.f == F::AspectRatio ? std::fabs(v - t.v) < 1e-3f : v == t.v; break;
            }
            if (!ok) break;
        }
        if (ok != q.negate) return true;
    }
    return false;
}

void StyleEngine::evalMedia() {
    m_mediaOk.clear();
    m_condBase.clear();
    for (const auto& sh : m_sheets) {
        m_condBase.push_back(u32(m_mediaOk.size()));
        for (const auto& c : sh.m_conds)
            m_mediaOk.push_back(c.kind == StyleCondition::Kind::Media ? u8(evalCondition(c, m_env.width, m_env.height, m_env)) : u8(1));
    }
}

bool StyleEngine::setMediaEnv(const MediaEnv& env) {
    if (env == m_env) return false;
    m_env = env;
    const std::vector<u8> before = m_mediaOk;
    evalMedia();
    if (before == m_mediaOk) return false;
    ++m_version;
    return true;
}

bool StyleEngine::condOk(const StyleSubject& s, u32 node, u32 idx) const {
    const StyleCondition* c = m_orderedCond[idx];
    if (!c) return true;
    const StyleSheet* sh = m_orderedSheet[idx];
    const u32 base = m_condBase[u32(sh - m_sheets.data())];
    for (;;) {
        const u32 ci = u32(c - sh->m_conds.data());
        if (c->kind == StyleCondition::Kind::Media) {
            if (!m_mediaOk[base + ci]) return false;
        } else {
            bool block = false;
            for (const auto& q : c->any)
                for (const auto& t : q.all)
                    block = block || t.f == StyleCondition::Feature::Height || t.f == StyleCondition::Feature::AspectRatio ||
                            t.f == StyleCondition::Feature::Landscape;
            f32 w = 0.f, h = 0.f;
            if (!s.containerSize(node, c->containerName, block, w, h)) return false;
            if (!evalCondition(*c, w, h, m_env)) return false;
        }
        if (c->parent == StyleRule::kNone) return true;
        c = &sh->m_conds[c->parent];
    }
}

void StyleEngine::reorder() {
    m_ordered.clear();
    m_orderedCond.clear();
    m_orderedSheet.clear();
    // @layer ranks: names merged over sheets in first-declaration order; a layer's sublayers precede its own rules
    // (post-order); unlayered rules last
    std::vector<std::string> names;
    for (const auto& sh : m_sheets)
        for (const auto& n : sh.m_layers)
            if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    std::vector<u32> rank(names.size(), 0);
    {
        u32 next = 0;
        auto parentOf = [](const std::string& n) { const auto d = n.rfind('.'); return d == std::string::npos ? std::string() : n.substr(0, d); };
        auto visit = [&](auto&& self, const std::string& parent) -> void {
            for (std::size_t k = 0; k < names.size(); ++k)
                if (parentOf(names[k]) == parent) {
                    self(self, names[k]);
                    rank[k] = next++;
                }
        };
        visit(visit, std::string());
    }
    const u32 unlayered = u32(names.size());
    struct Key { u32 rank; u32 spec; u32 sheet; u32 order; const StyleRule* r; const StyleSheet* sh; };
    std::vector<Key> keyed;
    m_hasContainer = false;
    u32 sheetIdx = 0;
    for (const auto& sh : m_sheets) {
        std::vector<u32> local(sh.m_layers.size());
        for (std::size_t k = 0; k < local.size(); ++k)
            local[k] = rank[std::size_t(std::find(names.begin(), names.end(), sh.m_layers[k]) - names.begin())];
        for (const auto& r : sh.m_rules)
            keyed.push_back({r.layer < local.size() ? local[r.layer] : unlayered, r.specificity, sheetIdx, r.order, &r, &sh});
        for (const auto& c : sh.m_conds) m_hasContainer = m_hasContainer || c.kind == StyleCondition::Kind::Container;
        ++sheetIdx;
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const Key& a, const Key& b) {
        return std::tie(a.rank, a.spec, a.sheet, a.order) < std::tie(b.rank, b.spec, b.sheet, b.order);
    });
    for (auto& k : keyed) {
        m_ordered.push_back(k.r);
        m_orderedSheet.push_back(k.sh);
        m_orderedCond.push_back(k.r->cond < k.sh->m_conds.size() ? &k.sh->m_conds[k.r->cond] : nullptr);
    }
    evalMedia();
    m_structural = m_relational = false;
    for (const auto& sh : m_sheets) {
        m_structural = m_structural || sh.structural();
        m_relational = m_relational || sh.relational();
    }
    buildInvalidation();
    ++m_version;
}

// ---- invalidation sets -----------------------------------------------------------------------------------------------
void StyleEngine::Invalidation::merge(const Invalidation& o) {
    down = std::max(down, o.down);
    sib = std::max(sib, o.sib);
    hasUp = std::max(hasUp, o.hasUp);
    hasPrev = hasPrev || o.hasPrev;
    has = has || o.has;
    hasDown = std::max(hasDown, o.hasDown);
    hasSib = hasSib || o.hasSib;
}
const StyleEngine::Invalidation& StyleEngine::classInvalidation(u32 h) const {
    auto it = m_classInv.find(h);
    return it != m_classInv.end() ? it->second : m_noInv;
}
const StyleEngine::Invalidation& StyleEngine::attrInvalidation(u32 h) const {
    auto it = m_attrInv.find(h);
    return it != m_attrInv.end() ? it->second : m_noInv;
}
namespace {
using Inv = StyleEngine::Invalidation;
using Comb = SimpleSelector::Combinator;
u8 addLevels(u8 a, u8 b) { return u16(a) + u16(b) >= Inv::kDeep ? Inv::kDeep : u8(a + b); }
struct InvCtx {
    u8 down = 0;
    u8 sib = 0;
    bool inHas = false;
    u8 hasDown = 0;
    bool hasSib = false;
};
struct InvBuilder {
    std::unordered_map<u32, Inv>& cls;
    std::unordered_map<u32, Inv>& attr;
    Inv& state;
    Inv& tree;
    void feature(const SimpleSelector& sel, const Inv& v) {
        for (u8 i = 0; i < sel.classCount; ++i) cls[sel.classes[i]].merge(v);
        for (const auto& a : sel.attrs) attr[a.nameHash].merge(v);
        if (sel.idHash) attr[hashId("id")].merge(v);
        if (sel.states) state.merge(v);
        tree.merge(v); // a node can start / stop matching by moving
    }
    void chain(const std::vector<SimpleSelector>& c, const InvCtx& ctx, int depth) {
        const std::size_t n = c.size();
        for (std::size_t k = 0; k < n; ++k) {
            Inv v;
            // reach to the chain's subject (compounds right of k)
            u8 down = 0;
            u8 sib = 0;
            for (std::size_t j = k + 1; j < n; ++j) {
                switch (c[j].combinator) {
                case Comb::Child: down = addLevels(down, 1); break;
                case Comb::Descendant: down = Inv::kDeep; break;
                case Comb::Adjacent: sib = addLevels(sib, 1); break;
                case Comb::Sibling: sib = Inv::kDeep; break;
                case Comb::None: break;
                }
            }
            if (ctx.inHas) {
                // anchor relation: compounds 0..k (the first combinator is the relation to the anchor)
                u8 up = 0;
                bool prev = false;
                for (std::size_t j = 0; j <= k; ++j) {
                    const Comb cb = j == 0 && c[0].combinator == Comb::None ? Comb::Descendant : c[j].combinator;
                    if (cb == Comb::Child) up = addLevels(up, 1);
                    else if (cb == Comb::Descendant) up = Inv::kDeep;
                    else if (cb == Comb::Adjacent || cb == Comb::Sibling) prev = true;
                }
                v.has = true;
                v.hasUp = up;
                v.hasPrev = prev;
                v.hasDown = ctx.hasDown;
                v.hasSib = ctx.hasSib;
                // inside the argument: other argument compounds are re-matched from the anchor, nothing else
            } else {
                v.down = addLevels(ctx.down, down);
                v.sib = addLevels(ctx.sib, sib);
            }
            feature(c[k], v);
            for (const auto& lg : c[k].logicals) {
                InvCtx sub;
                if (depth > 16) continue;
                if (lg.kind == SimpleSelector::Logical::Kind::Has) {
                    if (ctx.inHas) { // nested :has(): any ancestor / earlier sibling
                        sub = ctx;
                        sub.hasDown = Inv::kDeep;
                        sub.hasSib = true;
                    } else {
                        sub.inHas = true;
                        sub.hasDown = v.down;
                        sub.hasSib = v.sib != 0;
                    }
                    for (const auto& a : lg.args) chain(a, sub, depth + 1);
                    if (ctx.inHas) { // conservatively widen: the nested anchor is anywhere around the outer one
                        Inv w = v;
                        w.hasUp = Inv::kDeep;
                        w.hasPrev = true;
                        w.hasDown = Inv::kDeep;
                        w.hasSib = true;
                        for (const auto& a : lg.args)
                            for (const auto& x : a) feature(x, w);
                    }
                } else if (ctx.inHas) {
                    // :is() / :not() / :where() inside :has(): their compounds relate to compound k in either direction
                    Inv w = v;
                    w.hasUp = Inv::kDeep;
                    w.hasPrev = true;
                    for (const auto& a : lg.args)
                        for (const auto& x : a) {
                            feature(x, w);
                            for (const auto& l2 : x.logicals) {
                                InvCtx s2 = ctx;
                                s2.hasDown = Inv::kDeep;
                                s2.hasSib = true;
                                for (const auto& a2 : l2.args) chain(a2, s2, depth + 2);
                            }
                        }
                } else {
                    sub.down = v.down;
                    sub.sib = v.sib;
                    for (const auto& a : lg.args) chain(a, sub, depth + 1);
                }
            }
        }
    }
};
} // namespace
void StyleEngine::buildInvalidation() {
    m_classInv.clear();
    m_attrInv.clear();
    m_stateInv = m_treeInv = m_noInv = {};
    InvBuilder b{m_classInv, m_attrInv, m_stateInv, m_treeInv};
    for (const auto& sh : m_sheets)
        for (const auto& r : sh.m_rules) b.chain(r.chain, {}, 0);
}

namespace {
bool attrMatch(const SimpleSelector::Attr& at, std::string_view v) {
    using Op = SimpleSelector::Attr::Op;
    std::string low;
    if (at.ci) {
        low.reserve(v.size());
        for (char ch : v) low += char(std::tolower(static_cast<unsigned char>(ch)));
        v = low;
    }
    const std::string_view w = at.value;
    switch (at.op) {
    case Op::Exists: return true;
    case Op::Equals: return v == w;
    case Op::Word: {
        if (w.empty() || w.find_first_of(" \t\n") != std::string_view::npos) return false;
        std::size_t i = 0;
        while (i < v.size()) {
            while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
            std::size_t e = i;
            while (e < v.size() && !std::isspace(static_cast<unsigned char>(v[e]))) ++e;
            if (e > i && v.substr(i, e - i) == w) return true;
            i = e;
        }
        return false;
    }
    case Op::DashPrefix: return v == w || (v.size() > w.size() && v.starts_with(w) && v[w.size()] == '-');
    case Op::Prefix: return !w.empty() && v.starts_with(w);
    case Op::Suffix: return !w.empty() && v.ends_with(w);
    case Op::Contains: return !w.empty() && v.find(w) != std::string_view::npos;
    }
    return false;
}
// 1-based position among siblings (from the end for Last*; same type only for *OfType)
bool nthMatch(const StyleSubject& s, u32 node, const SimpleSelector::Nth& n) {
    using K = SimpleSelector::Nth::Kind;
    const bool last = n.kind == K::LastChild || n.kind == K::LastOfType;
    const bool typed = n.kind == K::OfType || n.kind == K::LastOfType;
    const u32 t = typed ? s.typeHash(node) : 0;
    i64 pos = 1;
    for (u32 c = last ? s.nextSibling(node) : s.prevSibling(node); c != 0xFFFFFFFFu; c = last ? s.nextSibling(c) : s.prevSibling(c))
        if (!typed || s.typeHash(c) == t) ++pos;
    // pos = a*k + b for some integer k >= 0
    if (n.a == 0) return pos == n.b;
    const i64 d = pos - n.b;
    return d % n.a == 0 && d / n.a >= 0;
}
} // namespace

namespace {
constexpr u32 kNoNode = 0xFFFFFFFFu;
bool simpleMatch(const StyleSubject& s, u32 node, const SimpleSelector& sel);
// Right-to-left with backtracking over descendant / general-sibling combinators. anchor != kNoNode: a :has()
// argument, whose leftmost compound must stand in its combinator's relation to the anchor.
bool chainFrom(const StyleSubject& s, u32 node, const std::vector<SimpleSelector>& chain, int k, u32 anchor) {
    using C = SimpleSelector::Combinator;
    const SimpleSelector& sel = chain[std::size_t(k)];
    if (!simpleMatch(s, node, sel)) return false;
    if (k == 0) {
        if (anchor == kNoNode) return true;
        switch (sel.combinator) {
        case C::Child: return s.parent(node) == anchor;
        case C::Adjacent: return s.prevSibling(node) == anchor;
        case C::Sibling:
            for (u32 p = s.prevSibling(node); p != kNoNode; p = s.prevSibling(p))
                if (p == anchor) return true;
            return false;
        default:
            for (u32 p = s.parent(node); p != kNoNode; p = s.parent(p))
                if (p == anchor) return true;
            return false;
        }
    }
    switch (sel.combinator) {
    case C::Child: { const u32 p = s.parent(node); return p != kNoNode && chainFrom(s, p, chain, k - 1, anchor); }
    case C::Adjacent: { const u32 p = s.prevSibling(node); return p != kNoNode && chainFrom(s, p, chain, k - 1, anchor); }
    case C::Sibling:
        for (u32 p = s.prevSibling(node); p != kNoNode; p = s.prevSibling(p))
            if (chainFrom(s, p, chain, k - 1, anchor)) return true;
        return false;
    default:
        for (u32 p = s.parent(node); p != kNoNode; p = s.parent(p)) {
            if (p == anchor) return false; // a relative selector stays below / after its anchor
            if (chainFrom(s, p, chain, k - 1, anchor)) return true;
        }
        return false;
    }
}
// Pre-order walk of root's descendants (root excluded); stops when fn returns true.
template <class F> bool anyDescendant(const StyleSubject& s, u32 root, F&& fn) {
    u32 c = s.firstChild(root);
    while (c != kNoNode) {
        if (fn(c)) return true;
        if (const u32 f = s.firstChild(c); f != kNoNode) { c = f; continue; }
        while (c != kNoNode && c != root) {
            if (const u32 n = s.nextSibling(c); n != kNoNode) { c = n; break; }
            c = s.parent(c);
        }
        if (c == root) break;
    }
    return false;
}
bool hasMatch(const StyleSubject& s, u32 anchor, const std::vector<SimpleSelector>& rel) {
    using C = SimpleSelector::Combinator;
    if (rel.empty()) return false;
    const int last = int(rel.size()) - 1;
    auto test = [&](u32 c) { return chainFrom(s, c, rel, last, anchor); };
    const C first = rel[0].combinator;
    if (first == C::Adjacent || first == C::Sibling) { // subjects: later siblings and their subtrees
        for (u32 n = s.nextSibling(anchor); n != kNoNode; n = s.nextSibling(n)) {
            if (test(n) || anyDescendant(s, n, test)) return true;
            if (first == C::Adjacent && last == 0) break;
        }
        return false;
    }
    return anyDescendant(s, anchor, test);
}
} // namespace

namespace {
bool simpleMatch(const StyleSubject& s, u32 node, const SimpleSelector& sel) {
    if (sel.typeHash && s.typeHash(node) != sel.typeHash) return false;
    if (sel.idHash && s.idHash(node) != sel.idHash) return false;
    for (u8 k = 0; k < sel.classCount; ++k)
        if (!s.hasClass(node, sel.classes[k])) return false;
    if ((s.states(node) & sel.states) != sel.states) return false;
    for (const auto& at : sel.attrs) {
        std::string_view v;
        if (!s.attribute(node, at.nameHash, v) || !attrMatch(at, v)) return false;
    }
    for (const auto& n : sel.nths)
        if (!nthMatch(s, node, n)) return false;
    if ((sel.flags & SimpleSelector::kEmpty) && !s.isEmpty(node)) return false;
    if ((sel.flags & SimpleSelector::kRoot) && s.parent(node) != kNoNode) return false;
    using LK = SimpleSelector::Logical::Kind;
    for (const auto& lg : sel.logicals) {
        bool any = false;
        for (const auto& a : lg.args) {
            if (a.empty()) continue;
            if (lg.kind == LK::Has ? hasMatch(s, node, a) : chainFrom(s, node, a, int(a.size()) - 1, kNoNode)) { any = true; break; }
        }
        if (lg.kind == LK::Not ? any : !any) return false;
    }
    return true;
}
} // namespace

bool StyleEngine::matchSimple(const StyleSubject& s, u32 node, const SimpleSelector& sel) {
    return simpleMatch(s, node, sel);
}

bool StyleEngine::matchChain(const StyleSubject& s, u32 node, const std::vector<SimpleSelector>& chain) {
    return !chain.empty() && chainFrom(s, node, chain, int(chain.size()) - 1, kNoNode);
}

bool StyleEngine::matches(const StyleSubject& s, u32 node, const StyleRule& rule) const {
    if (rule.root) return s.parent(node) == 0xFFFFFFFFu;
    return matchChain(s, node, rule.chain);
}

bool StyleEngine::matchFrom(const StyleSubject& s, u32 node, const StyleRule& rule, int k) const {
    return k >= 0 && chainFrom(s, node, rule.chain, k, kNoNode);
}

const KeyframesRule* StyleEngine::keyframes(u32 name) const {
    for (auto it = m_sheets.rbegin(); it != m_sheets.rend(); ++it)
        for (auto k = it->keyframes().rbegin(); k != it->keyframes().rend(); ++k)
            if (k->name == name) return &*k;
    return nullptr;
}

bool StyleEngine::resolveDecl(const Declaration& d, PropValue& out) const {
    if (d.raw.empty()) { out = d.value; return true; }
    return resolveVar(d.raw, d.prop, out, 0);
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

u32 StyleEngine::intern(const ComputedStyle& cs) {
    // FNV over the bytes of a value-initialised copy is not padding-safe; hash fields that matter most and compare.
    u64 h = 1469598103934665603ull;
    auto mix = [&](f32 f) { u32 b; std::memcpy(&b, &f, 4); h = (h ^ b) * 1099511628211ull; };
    mix(cs.background.r); mix(cs.background.g); mix(cs.background.a); mix(cs.color.r); mix(cs.color.a);
    mix(cs.width.value); mix(cs.height.value); mix(cs.padding.l); mix(cs.padding.t); mix(cs.gap); mix(cs.fontSize);
    mix(cs.opacity); mix(cs.borderWidth); mix(cs.radius[0]); mix(cs.grow);
    h = (h ^ (u64(cs.layout) | u64(cs.direction) << 8 | u64(cs.visible) << 16 | u64(cs.alignItems) << 24 | u64(cs.justify) << 32)) * 1099511628211ull;
    const std::size_t mask = m_hashIndex.size() - 1;
    std::size_t slot = std::size_t(h >> 20) & mask;
    for (;;) {
        const u32 e = m_hashIndex[slot];
        if (e == 0) break;
        if (m_table[e - 1] == cs) return e - 1;
        slot = (slot + 1) & mask;
    }
    if ((m_table.size() + 1) * 2 > m_hashIndex.size()) {
        // grow and rehash in place (indices are handed out: never reorder; scratch slots stay unhashed)
        std::vector<u32> bigger(m_hashIndex.size() * 2, 0);
        m_hashIndex.swap(bigger);
        const auto keep = std::move(m_table);
        const auto flags = std::move(m_isScratch);
        m_table.clear();
        m_isScratch.clear();
        m_table.reserve(keep.size() * 2);
        for (std::size_t k = 0; k < keep.size(); ++k) {
            if (flags[k]) { m_table.push_back(keep[k]); m_isScratch.push_back(1); continue; }
            [[maybe_unused]] const u32 at = intern(keep[k]);
        }
        return intern(cs);
    }
    m_table.push_back(cs);
    m_isScratch.push_back(0);
    m_hashIndex[slot] = u32(m_table.size());
    return u32(m_table.size() - 1);
}
u32 StyleEngine::allocScratch(const ComputedStyle& cs) {
    if (!m_freeScratch.empty()) {
        const u32 i = m_freeScratch.back();
        m_freeScratch.pop_back();
        m_table[i] = cs;
        return i;
    }
    if ((m_table.size() + 1) * 2 > m_hashIndex.size()) { // keep the load factor: grow via a rehash first
        std::vector<u32> bigger(m_hashIndex.size() * 2, 0);
        m_hashIndex.swap(bigger);
        const auto keep = std::move(m_table);
        const auto flags = std::move(m_isScratch);
        m_table.clear();
        m_isScratch.clear();
        for (std::size_t k = 0; k < keep.size(); ++k) {
            if (flags[k]) { m_table.push_back(keep[k]); m_isScratch.push_back(1); continue; }
            [[maybe_unused]] const u32 at = intern(keep[k]);
        }
    }
    m_table.push_back(cs);
    m_isScratch.push_back(1);
    return u32(m_table.size() - 1);
}

ComputedStyle StyleEngine::computeValue(const StyleSubject& s, u32 node, const ComputedStyle* parent, const ComputedStyle* defaults,
                         const std::vector<std::pair<Prop, PropValue>>& inl) {
    ComputedStyle cs = defaults ? *defaults : ComputedStyle{};
    if (parent) inheritFrom(cs, *parent);
    m_scratch.clear();
    m_localTokens.clear();
    for (u32 oi = 0; oi < u32(m_ordered.size()); ++oi)
        if (const StyleRule* r = m_ordered[oi]; matches(s, node, *r) && condOk(s, node, oi)) {
            m_scratch.push_back(r);
            for (const auto& d : r->decls)
                if (d.prop == Prop::Count) m_localTokens.emplace_back(d.customHash, std::string_view(d.raw));
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
    return cs;
}
u32 StyleEngine::compute(const StyleSubject& s, u32 node, const ComputedStyle* parent, const ComputedStyle* defaults,
                         const std::vector<std::pair<Prop, PropValue>>& inl) {
    return intern(computeValue(s, node, parent, defaults, inl));
}

} // namespace fuse::ui
