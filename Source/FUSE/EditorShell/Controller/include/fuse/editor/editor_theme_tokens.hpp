#pragma once
// Q8 theme tokens: the editor dark theme (P6 `set_dark_theme`) as toolkit-neutral data. Same values as the Qt shell's
// kDarkThemeSpec (src/qt/editor_theme.hpp, which stays the Qt source until Q8 moves it here); the twin gates
// fuse_editor_ui_theme / fuse_editor_qt_theme check both shells against these numbers.
#include <array>
#include <string>
#include <string_view>

namespace fuse::editor {

struct ThemeToken {
    const char* role;
    float r, g, b, a;
};

inline constexpr std::array<ThemeToken, 26> kEditorDarkTheme{{
    {"WindowBg", 0.08f, 0.09f, 0.10f, 1.f},       {"ChildBg", 0.06f, 0.07f, 0.08f, 1.f},
    {"PopupBg", 0.08f, 0.09f, 0.10f, 0.98f},      {"Border", 0.18f, 0.20f, 0.22f, 1.f},
    {"FrameBg", 0.12f, 0.13f, 0.15f, 1.f},        {"FrameBgHovered", 0.18f, 0.20f, 0.22f, 1.f},
    {"FrameBgActive", 0.22f, 0.24f, 0.27f, 1.f},  {"TitleBg", 0.06f, 0.07f, 0.08f, 1.f},
    {"TitleBgActive", 0.10f, 0.11f, 0.13f, 1.f},  {"MenuBarBg", 0.06f, 0.07f, 0.08f, 1.f},
    {"Header", 0.18f, 0.20f, 0.22f, 1.f},         {"HeaderHovered", 0.24f, 0.27f, 0.30f, 1.f},
    {"HeaderActive", 0.28f, 0.32f, 0.36f, 1.f},   {"Button", 0.16f, 0.18f, 0.20f, 1.f},
    {"ButtonHovered", 0.22f, 0.26f, 0.30f, 1.f},  {"ButtonActive", 0.28f, 0.34f, 0.40f, 1.f},
    {"Tab", 0.10f, 0.11f, 0.13f, 1.f},            {"TabHovered", 0.22f, 0.26f, 0.30f, 1.f},
    {"TabActive", 0.16f, 0.20f, 0.24f, 1.f},      {"CheckMark", 0.28f, 0.60f, 0.95f, 1.f},
    {"SliderGrab", 0.28f, 0.60f, 0.95f, 1.f},     {"SliderGrabActive", 0.36f, 0.70f, 1.00f, 1.f},
    {"DockingPreview", 0.28f, 0.60f, 0.95f, 0.7f}, {"Text", 0.86f, 0.88f, 0.90f, 1.f},
    {"TextDisabled", 0.40f, 0.43f, 0.46f, 1.f},   {"Separator", 0.18f, 0.20f, 0.22f, 1.f},
}};

/// 8-bit "#rrggbb" / "#rrggbbaa" for a role (round to nearest, the Qt shell's quantisation). Empty when unknown.
inline std::string themeHex(std::string_view role) {
    for (const ThemeToken& t : kEditorDarkTheme) {
        if (role != t.role) continue;
        const auto q = [](float v) { return static_cast<int>(v * 255.f + 0.5f); };
        static constexpr char kHex[] = "0123456789abcdef";
        std::string s = "#";
        for (const float c : {t.r, t.g, t.b, t.a}) {
            const int v = q(c);
            s += kHex[v >> 4];
            s += kHex[v & 15];
        }
        if (t.a >= 1.f) s.resize(7);
        return s;
    }
    return {};
}

/// The tokens as JSON ({"WindowBg": "#141719", ...}): the Q8 theme file both shells read.
inline std::string themeTokensJson() {
    std::string out = "{";
    for (const ThemeToken& t : kEditorDarkTheme) {
        if (out.size() > 1) out += ", ";
        out += '"';
        out += t.role;
        out += "\": \"" + themeHex(t.role) + '"';
    }
    return out + "}";
}

} // namespace fuse::editor
