#include "fuse/ui/style.hpp"

#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <deque>
#include <mutex>
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
    {Prop::Transition, "transition", T::Transition, false, false, false},
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
static bool transitionProps(std::string_view name, u64& mask) {
    auto bit = [](Prop p) { return u64(1) << u8(p); };
    if (name == "all") {
        for (int k = 0; k < kPropCount; ++k)
            if (kProps[k].animatable) mask |= u64(1) << k;
        return true;
    }
    if (name == "background" || name == "background-color") { mask |= bit(Prop::Background) | bit(Prop::Background2); return true; }
    if (name == "padding") { mask |= bit(Prop::PaddingLeft) | bit(Prop::PaddingTop) | bit(Prop::PaddingRight) | bit(Prop::PaddingBottom); return true; }
    if (name == "margin") { mask |= bit(Prop::MarginLeft) | bit(Prop::MarginTop) | bit(Prop::MarginRight) | bit(Prop::MarginBottom); return true; }
    if (name == "border-radius") { mask |= bit(Prop::RadiusTL) | bit(Prop::RadiusTR) | bit(Prop::RadiusBR) | bit(Prop::RadiusBL); return true; }
    if (name == "border") { mask |= bit(Prop::BorderColor) | bit(Prop::BorderWidth); return true; }
    if (name == "box-shadow") { mask |= bit(Prop::ShadowColor) | bit(Prop::ShadowBlur) | bit(Prop::ShadowX) | bit(Prop::ShadowY); return true; }
    if (name == "inset") { mask |= bit(Prop::Left) | bit(Prop::Top) | bit(Prop::Right) | bit(Prop::Bottom); return true; }
    Prop p;
    if (!findProp(name, p)) return false;
    if (propInfo(p).animatable) mask |= bit(p); // discrete properties: accepted, not animated
    return true;
}
static bool parseTime(std::string_view w, f32& out) {
    std::string_view unit;
    if (!parseNumber(w, out, unit)) return false;
    if (unit == "ms") out /= 1000.f;
    else if (unit != "s") return false;
    return true;
}
static bool parseTiming(std::string_view w, TransitionSpec& t) {
    using K = TransitionSpec::Kind;
    using SP = TransitionSpec::StepPos;
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
// "none" | <single-transition> [, <single-transition>]*  with  [<property> | all] || <time> || <easing> || <time>
static bool parseTransition(std::string_view text, PropValue& out) {
    if (text == "none") return true;
    std::size_t s = 0;
    int depth = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && text[i] == '(') ++depth;
        if (i < text.size() && text[i] == ')') --depth;
        if (i < text.size() && !(text[i] == ',' && depth == 0)) continue;
        const std::string_view item = trim(text.substr(s, i - s));
        s = i + 1;
        if (item.empty() || out.trackCount >= kMaxTransitions) return false;
        TransitionSpec t;
        bool haveProp = false, haveEase = false;
        int times = 0;
        std::size_t w0 = 0;
        int d = 0;
        for (std::size_t j = 0; j <= item.size(); ++j) {
            if (j < item.size() && item[j] == '(') ++d;
            if (j < item.size() && item[j] == ')') --d;
            if (j < item.size() && !(std::isspace(static_cast<unsigned char>(item[j])) && d == 0)) continue;
            const std::string_view w = trim(item.substr(w0, j - w0));
            w0 = j + 1;
            if (w.empty()) continue;
            f32 v = 0.f;
            if (times < 2 && parseTime(w, v)) {
                if (times == 0 && v < 0.f) return false; // negative durations are invalid (delays may be negative)
                (times++ == 0 ? t.duration : t.delay) = v;
            } else if (!haveEase && parseTiming(w, t)) haveEase = true;
            else if (!haveProp && w != "none" && transitionProps(w, t.props)) haveProp = true;
            else if (!haveProp && w == "none") haveProp = true; // covers nothing
            else return false;
        }
        if (!haveProp) transitionProps("all", t.props);
        out.transitions[out.trackCount++] = t;
    }
    return depth == 0;
}

f32 transitionEase(const TransitionSpec& s, f32 t) {
    t = std::clamp(t, 0.f, 1.f);
    if (s.kind == TransitionSpec::Kind::Steps) {
        using SP = TransitionSpec::StepPos;
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

PropValue interpolateProp(const PropValue& a, const PropValue& b, f32 k) {
    if (a.type != b.type) return k < 0.5f ? a : b;
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
    default: return false;
    }
}

bool parsePropValue(Prop p, std::string_view text, PropValue& out) {
    text = trim(text);
    const PropInfo& pi = propInfo(p);
    out = {};
    out.type = pi.type;
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
        std::string_view unit;
        if (!parseNumber(text, out.num, unit)) return false;
        if (unit == "%") out.num /= 100.f;
        return unit.empty() || unit == "%";
    }
    case T::Color: return parseColor(text, out.color);
    case T::Transition: return parseTransition(text, out);
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
    case Prop::Filter:
        s.filterCount = v.trackCount;
        s.filterShadows = v.shadows;
        for (u8 k = 0; k < kMaxGridTracks; ++k) {
            s.filterFns[k] = k < v.trackCount ? FilterFn(v.fns[k]) : FilterFn{};
            s.filterAmounts[k] = k < v.trackCount ? v.tracks[k].value : 0.f;
        }
        break;
    case Prop::Transition:
        s.transitionCount = v.trackCount;
        s.transitions = v.transitions;
        break;
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


bool parseSelector(std::string_view text, StyleRule& rule, bool& isRoot) {
    rule.chain.clear();
    isRoot = false;
    std::size_t i = 0;
    auto comb = SimpleSelector::Combinator::None;
    u32 ids = 0, cls = 0, types = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
            if (!rule.chain.empty() && comb == SimpleSelector::Combinator::None) comb = SimpleSelector::Combinator::Descendant;
        }
        if (i >= text.size()) break;
        if (text[i] == '>') {
            comb = SimpleSelector::Combinator::Child;
            ++i;
            continue;
        }
        SimpleSelector sel;
        sel.combinator = rule.chain.empty() ? SimpleSelector::Combinator::None : comb;
        bool any = false;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '>') {
            const char c = text[i];
            auto ident = [&](std::size_t from) {
                std::size_t k = from;
                while (k < text.size() && (std::isalnum(static_cast<unsigned char>(text[k])) || text[k] == '-' || text[k] == '_')) ++k;
                return k;
            };
            if (c == '*') { ++i; any = true; }
            else if (c == '#') { const auto e = ident(i + 1); sel.idHash = hashId(text.substr(i + 1, e - i - 1)); i = e; ++ids; any = true; }
            else if (c == '.') {
                const auto e = ident(i + 1);
                if (sel.classCount >= 4) return false;
                sel.classes[sel.classCount++] = hashId(text.substr(i + 1, e - i - 1));
                i = e; ++cls; any = true;
            } else if (c == ':') {
                const auto e = ident(i + 1);
                const auto name = text.substr(i + 1, e - i - 1);
                i = e; any = true;
                if (name == "root") { isRoot = true; continue; }
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
        rule.chain.push_back(sel);
        comb = SimpleSelector::Combinator::None;
    }
    rule.specificity = (std::min(ids, 255u) << 16) | (std::min(cls, 255u) << 8) | std::min(types, 255u);
    return !rule.chain.empty() || isRoot;
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

std::expected<StyleSheet, StyleDiagnostic> StyleSheet::parse(std::string_view css, std::vector<StyleDiagnostic>* diags) {
    StyleSheet sheet;
    Cursor c{css};
    u32 order = 0;
    for (;;) {
        c.skipWs();
        if (c.i >= css.size()) break;
        const auto brace = css.find('{', c.i);
        if (brace == std::string_view::npos) return std::unexpected(StyleDiagnostic{StyleError::Syntax, c.line, "expected '{'"});
        const std::string_view selectors = trim(css.substr(c.i, brace - c.i));
        const auto close = css.find('}', brace);
        if (close == std::string_view::npos) return std::unexpected(StyleDiagnostic{StyleError::Syntax, c.line, "expected '}'"});
        const u32 line = c.line;
        for (std::size_t k = c.i; k < close; ++k)
            if (css[k] == '\n') ++c.line;
        StyleRule proto;
        std::string_view body = css.substr(brace + 1, close - brace - 1);
        std::vector<std::pair<u32, std::string>> tokens;
        // declarations
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
                        Declaration d;
                        d.customHash = hashId(name);
                        d.raw.assign(value);
                        tokens.emplace_back(d.customHash, d.raw);
                        proto.decls.push_back(std::move(d));
                    } else if (!(name == "background" && value.starts_with("linear-gradient")) && findProp(name, p)) addDecl(proto, p, value, line, diags);
                    else addShorthand(proto, name, value, line, diags);
                }
            }
            if (semi == std::string_view::npos) break;
            body.remove_prefix(semi + 1);
        }
        // one rule per selector in the list
        std::string_view list = selectors;
        while (!list.empty()) {
            const auto comma = list.find(',');
            const auto one = trim(list.substr(0, comma));
            StyleRule rule;
            bool isRoot = false;
            if (!parseSelector(one, rule, isRoot)) {
                if (diags) diags->push_back({StyleError::Syntax, line, "bad selector: " + std::string(one)});
            } else {
                rule.decls = proto.decls;
                rule.order = order++;
                rule.root = isRoot;
                if (isRoot) for (auto& t : tokens) sheet.m_tokens.push_back(t);
                if (!rule.chain.empty() || isRoot) sheet.m_rules.push_back(std::move(rule));
            }
            if (comma == std::string_view::npos) break;
            list.remove_prefix(comma + 1);
        }
        c.i = close + 1;
    }
    return sheet;
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

void StyleEngine::reorder() {
    m_ordered.clear();
    u32 sheetIdx = 0;
    std::vector<std::pair<u64, const StyleRule*>> keyed;
    for (const auto& sh : m_sheets) {
        for (const auto& r : sh.m_rules) keyed.emplace_back((u64(r.specificity) << 40) | (u64(sheetIdx) << 32) | r.order, &r);
        ++sheetIdx;
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& k : keyed) m_ordered.push_back(k.second);
    ++m_version;
}

bool StyleEngine::matchSimple(const StyleSubject& s, u32 node, const SimpleSelector& sel) const {
    if (sel.typeHash && s.typeHash(node) != sel.typeHash) return false;
    if (sel.idHash && s.idHash(node) != sel.idHash) return false;
    for (u8 k = 0; k < sel.classCount; ++k)
        if (!s.hasClass(node, sel.classes[k])) return false;
    if ((s.states(node) & sel.states) != sel.states) return false;
    return true;
}

bool StyleEngine::matches(const StyleSubject& s, u32 node, const StyleRule& rule) const {
    if (rule.root) return s.parent(node) == 0xFFFFFFFFu;
    int k = int(rule.chain.size()) - 1;
    if (!matchSimple(s, node, rule.chain[std::size_t(k)])) return false;
    u32 cur = node;
    while (k > 0) {
        const auto comb = rule.chain[std::size_t(k)].combinator;
        --k;
        const auto& want = rule.chain[std::size_t(k)];
        if (comb == SimpleSelector::Combinator::Child) {
            cur = s.parent(cur);
            if (cur == 0xFFFFFFFFu || !matchSimple(s, cur, want)) return false;
        } else {
            for (;;) {
                cur = s.parent(cur);
                if (cur == 0xFFFFFFFFu) return false;
                if (matchSimple(s, cur, want)) break;
            }
        }
    }
    return true;
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
    for (const StyleRule* r : m_ordered)
        if (matches(s, node, *r)) {
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
