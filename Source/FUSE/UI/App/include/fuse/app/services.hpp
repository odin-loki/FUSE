// fuse_app platform services (plan §4.13): the platform-neutral types behind AppWindow's window management, the system
// theme, displays, menus, file dialogs and the HiDPI scale maths. Everything here works headless (tests, servers); the
// native backends (fuse/app/native.hpp) plug the OS in behind the same API.
#pragma once

#include "fuse/ui/ui.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace fuse::app {

class AppWindow;

// ---- window management ---------------------------------------------------------------------------------------------
enum class WindowMode : fuse::ui::u8 { Normal, Minimized, Maximized, Fullscreen };

/// Light / dark preference and accent colour of the desktop. Unknown when the platform cannot tell (plain X11).
enum class ColorScheme : fuse::ui::u8 { Unknown, Light, Dark };
struct SystemTheme {
    ColorScheme scheme = ColorScheme::Unknown;
    bool hasAccent = false;
    fuse::ui::Color accent{};
    friend bool operator==(const SystemTheme&, const SystemTheme&) = default;
};

/// What happened to a window (AppWindow::setEventHandler). Delivered from the event pump (or from the call that caused
/// it on the headless backend).
struct WindowEvent {
    enum class Type : fuse::ui::u8 {
        FocusGained, FocusLost,   ///< the OS window became / stopped being the active window
        Moved,                    ///< `position`: new top-left of the client area on screen (logical units)
        ModeChanged,              ///< `mode`: minimised / maximised / fullscreen / restored
        Shown, Hidden,
        CloseRequested,           ///< the user asked to close and the close guard allowed it (the window is closing)
        CloseVetoed,              ///< the user asked to close and the close guard refused
        ThemeChanged,             ///< `theme`: the system light / dark preference or accent changed
        ScaleChanged,             ///< `scale`: the window moved to a display with another scale
    };
    Type type = Type::FocusGained;
    fuse::ui::Vec2 position{};
    WindowMode mode = WindowMode::Normal;
    SystemTheme theme{};
    fuse::ui::f32 scale = 1.f;
};
struct WindowEventHandler {
    void* user = nullptr;
    void (*fn)(void* user, AppWindow& w, const WindowEvent& e) = nullptr;
};
/// Close-request veto (unsaved changes): return false to keep the window open.
struct CloseGuard {
    void* user = nullptr;
    bool (*allow)(void* user, AppWindow& w) = nullptr;
};

/// What AppWindow asks its OS backend to do; the backend reads the wanted value from the window (title(),
/// requestedMode(), minimumSize() ...). Returns false when the platform cannot do it (the window state then stays).
enum class WindowRequest : fuse::ui::u8 { Title, SizeLimits, Mode, Visible, Position, Activate, Menu };
struct WindowBackend {
    void* user = nullptr;
    bool (*apply)(void* user, AppWindow& w, WindowRequest what) = nullptr;
};

// ---- displays ------------------------------------------------------------------------------------------------------
struct DisplayInfo {
    std::string name{};
    fuse::ui::Rect bounds{};    ///< physical pixels in the virtual desktop
    fuse::ui::Rect workArea{};  ///< bounds minus task bars / docks (== bounds when unknown)
    fuse::ui::f32 scale = 1.f;
    fuse::ui::f32 refreshHz = 0.f; ///< 0 = unknown
    bool primary = false;
};

// ---- menus (plan UI10) -----------------------------------------------------------------------------------------------
/// One menu entry. A platform-neutral description: AppWindow::setMenu builds the macOS menu bar from it when the
/// native backend is attached and the in-window widgets::menuBar everywhere else.
struct MenuItem {
    std::string title{};
    fuse::ui::u32 id = 0;                 ///< reported on activation (0 = none: submenu parents, separators)
    fuse::ui::Key key = fuse::ui::Key::None; ///< shortcut key (None = no shortcut) with `mods` (kModCtrl = Cmd on macOS)
    fuse::ui::u8 mods = 0;
    bool enabled = true;
    bool checkable = false, checked = false;
    bool separator = false;
    std::vector<MenuItem> items{};        ///< non-empty: a submenu
    static MenuItem sep() { MenuItem m; m.separator = true; return m; }
};
struct MenuDesc {
    std::vector<MenuItem> menus{}; ///< the top-level titles ("File", "Edit" ...), each with `items`
    bool empty() const { return menus.empty(); }
};
struct MenuHandler {
    void* user = nullptr;
    void (*activated)(void* user, AppWindow& w, fuse::ui::u32 id) = nullptr;
};
/// "Ctrl+Shift+S" (Cmd / Option glyph-free names on every platform; empty when key is None).
std::string shortcutText(fuse::ui::Key key, fuse::ui::u8 mods);
/// The widgets::menuBarMenu item string of one top-level menu ('\n' lines, '\t' nesting, "-" separators; checkable items
/// carry a "[x] " / "[ ] " prefix, shortcuts follow the title) and, per line, the id it activates (0 = none).
std::string menuItemsText(const MenuItem& menu, std::vector<fuse::ui::u32>& lineIds);
/// Depth-first search by id (nullptr when absent).
MenuItem* findMenuItem(MenuDesc& d, fuse::ui::u32 id);
/// The enabled item bound to this shortcut (nullptr when none).
const MenuItem* findMenuShortcut(const MenuDesc& d, fuse::ui::Key key, fuse::ui::u8 mods);

// ---- file dialogs ----------------------------------------------------------------------------------------------------
struct FileDialogOptions {
    std::string title = "Open";
    std::string directory{};   ///< start directory (empty = the platform's choice)
    std::string filter{};      ///< "*.png;*.jpg" (empty = all files), as widgets::fileDialog
    std::string filterName{};  ///< label of the filter in native dialogs ("Images")
    std::string defaultName{}; ///< save: the suggested file name
    bool save = false;
    bool pickDirectory = false; ///< choose a folder (native dialogs only)
};
enum class FileDialogStatus : fuse::ui::u8 {
    Accepted,
    Cancelled,
    Unavailable, ///< no native dialog here: fall back to the in-toolkit widgets::fileDialog
};
/// Called once with the chosen path ("" = cancelled).
struct FileDialogCallback {
    void* user = nullptr;
    void (*done)(void* user, AppWindow& w, std::string_view path) = nullptr;
};
/// Opens the in-toolkit widgets::fileDialog in `w` and reports through `cb` (the fallback of
/// NativePlatform::openFileDialog; usable directly on a headless window). Returns the dialog node.
fuse::ui::NodeHandle openToolkitFileDialog(AppWindow& w, const FileDialogOptions& o, FileDialogCallback cb);
/// Command lines of the Linux / BSD desktop dialog helpers (argv[0] = "zenity" / "kdialog").
std::vector<std::string> zenityArgs(const FileDialogOptions& o);
std::vector<std::string> kdialogArgs(const FileDialogOptions& o);

// ---- system theme ----------------------------------------------------------------------------------------------------
/// A `:root { --token: ... }` style sheet that re-colours the default widget theme for `t`: the light palette for
/// ColorScheme::Light, the stock dark tokens otherwise, plus the accent when the system has one. Added after the
/// theme sheet it restyles the whole window (AppWindow::applyTheme does that).
std::string themeTokensCss(const SystemTheme& t);
/// GTK_THEME ("Adwaita:dark") / `gsettings get org.gnome.desktop.interface color-scheme` output ("'prefer-dark'") ->
/// scheme; Unknown when the text says nothing.
ColorScheme schemeFromGtkTheme(std::string_view gtkTheme);
ColorScheme schemeFromGsettings(std::string_view output);

// ---- HiDPI scale maths -----------------------------------------------------------------------------------------------
namespace dpi {
/// dpi / 96 clamped to [0.5, 8]; 0 when dpi is not positive (unknown).
fuse::ui::f32 scaleFromDpi(fuse::ui::f32 dpi);
/// The `Xft.dpi` value of an X RESOURCE_MANAGER string ("Xft.dpi:\t144\n..."); 0 when absent / malformed.
fuse::ui::f32 xftDpi(std::string_view resourceManager);
/// X11 auto scale: $FUSE_UI_SCALE if it parses (> 0), else Xft.dpi / 96, else $GDK_SCALE, else 1.
fuse::ui::f32 x11Scale(std::string_view resourceManager, std::string_view gdkScale, std::string_view fuseScale);
/// NativeOptions::scale semantics: `option` > 0 wins (1 = physical pixels), else the detected scale, else 1.
fuse::ui::f32 resolveScale(fuse::ui::f32 option, fuse::ui::f32 detected);
/// Logical size -> device pixels (rounded, at least 1 x 1) and device position -> logical.
fuse::ui::Size toPixels(fuse::ui::Size logical, fuse::ui::f32 scale);
fuse::ui::Vec2 toLogical(fuse::ui::Vec2 pixels, fuse::ui::f32 scale);
} // namespace dpi

} // namespace fuse::app
