// System tray icon and global (application-level) menu bar.
//   Linux / BSD: StatusNotifierItem + com.canonical.dbusmenu over runtime-loaded libdbus (KDE, GNOME with the
//                AppIndicator extension, XFCE, LXQt, waybar, ...); global menu via com.canonical.AppMenu.Registrar.
//   Windows:     Shell_NotifyIconW + TrackPopupMenu; global menu = the window's native HMENU bar (SetMenu).
//   macOS:       NSStatusItem + NSMenu; global menu = [NSApp setMainMenu:].
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fuse/ui/draw_list.hpp"

namespace fuse::ui {

struct MenuItem {
    std::string label;            ///< '&' marks the mnemonic (stripped / translated per platform)
    std::function<void()> onTriggered;
    bool enabled = true, visible = true, separator = false, checkable = false, checked = false, radio = false;
    std::string shortcut;         ///< e.g. "Ctrl+S" (exported as a hint)
    std::string iconName;         ///< freedesktop icon name (DBus menus)
    std::vector<MenuItem> children;
    static MenuItem sep() { MenuItem m; m.separator = true; return m; }
};

enum class TrayStatus : u8 { Passive, Active, NeedsAttention };

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    /// Registers the icon with the platform tray. False when there is no tray host (no StatusNotifierWatcher on the
    /// session bus, no libdbus, ...): callers then fall back to a window, as Qt's isSystemTrayAvailable() suggests.
    bool show();
    void hide();
    bool visible() const;
    static bool available();          ///< a tray host exists right now
    static const char* backendName(); ///< "sni", "win32", "cocoa" or "none"

    void setId(std::string id);
    void setTitle(std::string title);
    void setToolTip(std::string title, std::string body = {});
    void setIcon(const Image& img);    ///< premultiplied RGBA (fuse::ui::Image); converted per platform
    void setIconName(std::string name); ///< themed icon name (SNI); ignored where unsupported
    void setStatus(TrayStatus s);
    void setMenu(std::vector<MenuItem> menu);
    std::vector<MenuItem>& menu();
    void menuChanged();                ///< after editing menu() in place (labels / checks)

    std::function<void(int x, int y)> onActivated;          ///< primary click
    std::function<void(int x, int y)> onSecondaryActivated; ///< middle click
    std::function<void(int delta, bool horizontal)> onScroll;

    /// Process tray events (call once per frame / from the event loop). Callbacks run inside poll().
    void poll();
    std::string serviceName() const;   ///< SNI bus name (diagnostics / tests)

    struct Impl;
private:
    std::unique_ptr<Impl> m;
};

/// Application menu bar exported to the desktop shell (global menu). nativeWindow: X11 Window id (as u64) on
/// Linux/BSD, HWND on Windows, ignored on macOS (the application menu is process-wide).
class GlobalMenuBar {
public:
    explicit GlobalMenuBar(u64 nativeWindow);
    ~GlobalMenuBar();
    GlobalMenuBar(const GlobalMenuBar&) = delete;
    GlobalMenuBar& operator=(const GlobalMenuBar&) = delete;
    /// Publish the top-level menus (each MenuItem with children is one menu title). False when the shell has no
    /// global menu (no com.canonical.AppMenu.Registrar): keep the in-window menuBar widget visible then.
    bool setMenus(std::vector<MenuItem> menus);
    bool active() const;
    void poll();
    std::string objectPath() const;
    static const char* backendName(); ///< "appmenu", "win32", "cocoa" or "none"

    struct Impl;
private:
    std::unique_ptr<Impl> m;
};

namespace dbusmenu_detail {
/// Flattened DBusMenu ids (depth-first, root = 0) shared by the SNI and AppMenu exporters (exposed for tests).
const MenuItem* itemById(const std::vector<MenuItem>& menu, i32 id);
}

} // namespace fuse::ui
