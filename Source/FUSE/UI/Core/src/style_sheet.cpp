// FUSE UI (UI2): QSS-like style sheet parser and the built-in dark theme. See context.hpp.
#include <fuse/ui/context.hpp>

#include <cctype>
#include <charconv>

namespace fuse::ui {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool parseColor(std::string_view v, Color& out) {
    if (v.size() != 7 && v.size() != 9) return false;
    if (v[0] != '#') return false;
    u32 hex = 0;
    const auto r = std::from_chars(v.data() + 1, v.data() + v.size(), hex, 16);
    if (r.ec != std::errc{} || r.ptr != v.data() + v.size()) return false;
    out = v.size() == 7 ? Color::rgb(hex) : Color::rgb(hex >> 8, u8(hex & 0xff));
    return true;
}

bool parseNumber(std::string_view v, f32& out) {
    if (v.ends_with("px")) v.remove_suffix(2);
    const auto r = std::from_chars(v.data(), v.data() + v.size(), out);
    return r.ec == std::errc{} && r.ptr == v.data() + v.size();
}

bool stateFromName(std::string_view s, u32& out) {
    if (s == "hover") out = kStateHover;
    else if (s == "pressed") out = kStatePressed;
    else if (s == "focus") out = kStateFocus;
    else if (s == "disabled") out = kStateDisabled;
    else if (s == "checked") out = kStateChecked;
    else return false;
    return true;
}

bool parseSelector(std::string_view sel, StyleRule& rule) {
    usize i = 0;
    auto ident = [&]() {
        const usize start = i;
        while (i < sel.size() && (std::isalnum(static_cast<unsigned char>(sel[i])) || sel[i] == '_' || sel[i] == '-')) ++i;
        return sel.substr(start, i - start);
    };
    if (sel == "*") return true;
    if (i < sel.size() && std::isalpha(static_cast<unsigned char>(sel[i]))) rule.type = std::string(ident());
    while (i < sel.size()) {
        const char c = sel[i++];
        const std::string_view id = ident();
        if (id.empty()) return false;
        if (c == '.') rule.cls = std::string(id);
        else if (c == '#') rule.name = std::string(id);
        else if (c == ':') {
            u32 s = 0;
            if (!stateFromName(id, s)) return false;
            rule.states |= s;
        } else return false;
    }
    return true;
}

bool parseDeclaration(std::string_view prop, std::string_view value, StyleRule& rule) {
    Style& s = rule.values;
    f32 n = 0.f;
    if (prop == "background") { if (!parseColor(value, s.background)) return false; rule.mask |= kStyleBackground; }
    else if (prop == "color") { if (!parseColor(value, s.foreground)) return false; rule.mask |= kStyleForeground; }
    else if (prop == "border-color") { if (!parseColor(value, s.border)) return false; rule.mask |= kStyleBorder; }
    else if (prop == "accent") { if (!parseColor(value, s.accent)) return false; rule.mask |= kStyleAccent; }
    else if (prop == "border-width") { if (!parseNumber(value, n)) return false; s.borderWidth = n; rule.mask |= kStyleBorderWidth; }
    else if (prop == "radius") { if (!parseNumber(value, n)) return false; s.radius = n; rule.mask |= kStyleRadius; }
    else if (prop == "text-scale") { if (!parseNumber(value, n) || n < 1.f) return false; s.textScale = u32(n); rule.mask |= kStyleTextScale; }
    else if (prop == "opacity") { if (!parseNumber(value, n)) return false; s.opacity = n; rule.mask |= kStyleOpacity; }
    else return false;
    return true;
}

} // namespace

bool StyleSheet::parse(std::string_view src, std::string* error) {
    std::string clean;
    clean.reserve(src.size());
    for (usize i = 0; i < src.size(); ++i) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            const usize end = src.find("*/", i + 2);
            if (end == std::string_view::npos) break;
            i = end + 1;
            continue;
        }
        clean.push_back(src[i]);
    }
    std::string_view s = clean;
    auto fail = [&](std::string msg) {
        if (error) *error = std::move(msg);
        return false;
    };
    while (true) {
        s = trim(s);
        if (s.empty()) return true;
        const usize open = s.find('{');
        const usize close = s.find('}');
        if (open == std::string_view::npos || close == std::string_view::npos || close < open) return fail("expected '{ ... }'");
        const std::string_view selectors = s.substr(0, open);
        const std::string_view body = s.substr(open + 1, close - open - 1);
        s = s.substr(close + 1);
        std::vector<StyleRule> group;
        usize from = 0;
        while (from <= selectors.size()) {
            usize comma = selectors.find(',', from);
            if (comma == std::string_view::npos) comma = selectors.size();
            const std::string_view one = trim(selectors.substr(from, comma - from));
            StyleRule rule;
            if (one.empty() || !parseSelector(one, rule)) return fail("bad selector '" + std::string(one) + "'");
            group.push_back(std::move(rule));
            from = comma + 1;
        }
        usize at = 0;
        while (at < body.size()) {
            usize semi = body.find(';', at);
            if (semi == std::string_view::npos) semi = body.size();
            const std::string_view decl = trim(body.substr(at, semi - at));
            at = semi + 1;
            if (decl.empty()) continue;
            const usize colon = decl.find(':');
            if (colon == std::string_view::npos) return fail("expected 'property: value' in '" + std::string(decl) + "'");
            const std::string_view prop = trim(decl.substr(0, colon));
            const std::string_view value = trim(decl.substr(colon + 1));
            for (StyleRule& r : group) {
                if (!parseDeclaration(prop, value, r)) return fail("bad declaration '" + std::string(decl) + "'");
            }
        }
        for (StyleRule& r : group) m_rules.push_back(std::move(r));
    }
}

std::string_view darkThemeSource() {
    return R"(
/* FUSE UI default dark theme */
Panel { background: #00000000; }
Panel.window { background: #1e1f22; border-color: #3c3f45; border-width: 1; radius: 6; }
Label { color: #e6e6e6; }
Label.muted { color: #9aa0a6; }
Button { background: #2b2d31; color: #e6e6e6; border-color: #4a4d55; border-width: 1; radius: 4; }
Button:hover { background: #34373d; }
Button:pressed { background: #3d8bfd; color: #ffffff; }
Button:focus { border-color: #3d8bfd; }
Button:disabled { background: #232427; color: #6b6f76; }
CheckBox { color: #e6e6e6; accent: #3d8bfd; }
CheckBox:focus { border-color: #3d8bfd; }
CheckBox:disabled { color: #6b6f76; }
Slider { background: #2b2d31; accent: #3d8bfd; radius: 3; }
Slider:focus { border-color: #3d8bfd; border-width: 1; }
LineEdit { background: #141518; color: #e6e6e6; border-color: #4a4d55; border-width: 1; radius: 3; }
LineEdit:focus { border-color: #3d8bfd; }
ScrollView { background: #18191c; }
ProgressBar { background: #2b2d31; accent: #3ddc84; radius: 3; }
)";
}

} // namespace fuse::ui
