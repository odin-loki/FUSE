#pragma once

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QString>

#include <array>
#include <string_view>

class QApplication;

namespace fuse::editor::qt {

/// One entry of the editor dark-theme colour specification (B6.1 "Qt Editor Initialisation",
/// source: docs/sources/P6.md `set_dark_theme`). Linear RGBA in [0, 1], exactly as specified.
struct ThemeColorSpec {
    const char* role;
    float r;
    float g;
    float b;
    float a;
};

/// The specification, verbatim (role names keep the P6 spelling so the table can be diffed
/// against the source). Every entry is mapped onto the Qt palette and/or the editor stylesheet.
inline constexpr std::array<ThemeColorSpec, 26> kDarkThemeSpec{{
    {"WindowBg", 0.08f, 0.09f, 0.10f, 1.f},
    {"ChildBg", 0.06f, 0.07f, 0.08f, 1.f},
    {"PopupBg", 0.08f, 0.09f, 0.10f, 0.98f},
    {"Border", 0.18f, 0.20f, 0.22f, 1.f},
    {"FrameBg", 0.12f, 0.13f, 0.15f, 1.f},
    {"FrameBgHovered", 0.18f, 0.20f, 0.22f, 1.f},
    {"FrameBgActive", 0.22f, 0.24f, 0.27f, 1.f},
    {"TitleBg", 0.06f, 0.07f, 0.08f, 1.f},
    {"TitleBgActive", 0.10f, 0.11f, 0.13f, 1.f},
    {"MenuBarBg", 0.06f, 0.07f, 0.08f, 1.f},
    {"Header", 0.18f, 0.20f, 0.22f, 1.f},
    {"HeaderHovered", 0.24f, 0.27f, 0.30f, 1.f},
    {"HeaderActive", 0.28f, 0.32f, 0.36f, 1.f},
    {"Button", 0.16f, 0.18f, 0.20f, 1.f},
    {"ButtonHovered", 0.22f, 0.26f, 0.30f, 1.f},
    {"ButtonActive", 0.28f, 0.34f, 0.40f, 1.f},
    {"Tab", 0.10f, 0.11f, 0.13f, 1.f},
    {"TabHovered", 0.22f, 0.26f, 0.30f, 1.f},
    {"TabActive", 0.16f, 0.20f, 0.24f, 1.f},
    {"CheckMark", 0.28f, 0.60f, 0.95f, 1.f},
    {"SliderGrab", 0.28f, 0.60f, 0.95f, 1.f},
    {"SliderGrabActive", 0.36f, 0.70f, 1.00f, 1.f},
    {"DockingPreview", 0.28f, 0.60f, 0.95f, 0.7f},
    {"Text", 0.86f, 0.88f, 0.90f, 1.f},
    {"TextDisabled", 0.40f, 0.43f, 0.46f, 1.f},
    {"Separator", 0.18f, 0.20f, 0.22f, 1.f},
}};

/// Spec colour for `role` (8-bit per channel, the precision a QPalette / stylesheet carries).
/// Unknown roles return an invalid QColor.
[[nodiscard]] QColor themeColor(std::string_view role);

/// Palette carrying the spec colours (Active/Inactive; Disabled uses TextDisabled).
[[nodiscard]] QPalette darkPalette();

/// Stylesheet for the states a QPalette cannot express (hover / pressed / focus / selected,
/// dock titles, tabs, menus, sliders, separators).
[[nodiscard]] QString darkStyleSheet();

/// Fusion style + dark palette + stylesheet + editor font on `app`.
void applyEditorTheme(QApplication& app);

/// Registers the bundled editor font (Open Sans, Qt resource) once; returns its family name, or
/// an empty string if the resource could not be loaded.
QString loadEditorFont();

/// Editor UI font: bundled family at `kEditorFontPixelSize` logical px (DPI scaling is applied
/// by Qt through the device pixel ratio).
[[nodiscard]] QFont editorFont();

inline constexpr int kEditorFontPixelSize = 13;

} // namespace fuse::editor::qt
