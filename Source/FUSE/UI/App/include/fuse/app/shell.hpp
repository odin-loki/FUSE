// fuse_app shell services (plan §4.13 Shell, UI10): a system tray icon with tooltip, click / double-click callbacks and
// a context menu, and desktop notifications.
//
//   fuse::app::TrayIcon tray;
//   tray.create(iconRgba, "My app");
//   tray.onClick([&] { window.requestRedraw(); });
//   tray.setMenu("Open\n-\nQuit", [&](unsigned index, std::string_view text) { if (text == "Quit") app.quit(); });
//   tray.notify("Build finished", "3 warnings");
//
// The menu uses the popupMenu item format ("A\nB\n-\nC": '-' is a separator, a leading '!' disables an item) and opens
// as an OS popup menu (Win32 TrackPopupMenu) at the pointer on right-click.
// Callbacks run from the thread's message loop: NativePlatform's event pump, or poll() when the app has none.
//
// Win32: Shell_NotifyIconW (NOTIFYICON_VERSION_4, a message-only window per icon; re-added when Explorer restarts);
// notifications are NIF_INFO balloons, shown as toasts on Windows 10 / 11. X11: no tray icon yet (XEmbed system tray
// not done); notify() calls org.freedesktop.Notifications over D-Bus when libdbus-1 loads at runtime, else reports
// Unsupported.
#pragma once

#include "fuse/ui/ui.hpp"

#include <functional>
#include <memory>
#include <string_view>

namespace fuse::app {

enum class NotifyResult : fuse::ui::u8 {
    Shown,       ///< handed to the OS notification service
    Unsupported, ///< no notification service on this platform / session
    Failed,      ///< the service exists but refused the request
};

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon(); ///< removes the icon
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    /// True when this platform / session has a tray (Win32 with a shell; false on X11 for now).
    static bool supported();
    /// Adds the icon (square RGBA raster, e.g. 32x32; resampled by the OS). False when the shell refuses.
    bool create(const fuse::ui::Image& icon, std::string_view tooltip);
    void remove();
    bool shown() const;
    bool setIcon(const fuse::ui::Image& icon);
    bool setTooltip(std::string_view tooltip);
    void onClick(std::function<void()> fn);
    void onDoubleClick(std::function<void()> fn);
    /// Clicks on a notification this icon posted.
    void onNotificationClick(std::function<void()> fn);
    /// Context menu (popupMenu item format). `fn` gets the item index (separators not counted) and its text.
    void setMenu(std::string_view items, std::function<void(unsigned index, std::string_view text)> fn);
    unsigned menuItemCount() const;
    /// Opens the context menu at a screen position (right-click does this); returns after a pick or dismissal.
    void openMenu(fuse::ui::Vec2 screen);
    /// Balloon / toast from this icon.
    NotifyResult notify(std::string_view title, std::string_view body);
    /// The icon's rectangle on screen (Win32 Shell_NotifyIconGetRect). False when hidden / overflowed / not shown.
    bool screenRect(fuse::ui::Rect& out) const;
    /// Dispatches pending messages of this icon's window (when no other message loop runs on the thread).
    void poll();
    /// Native handles for tests: the callback window (HWND), the callback message id and the icon id.
    unsigned long long nativeWindow() const;
    unsigned callbackMessage() const;
    unsigned iconId() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

/// A desktop notification without an app-owned tray icon (Win32 adds one transient tray icon for the balloon,
/// removed at exit; X11 uses D-Bus). `appName` names the sender where the service shows one.
NotifyResult notify(std::string_view title, std::string_view body, std::string_view appName = "FUSE");
/// True when notify() can reach a notification service here.
bool notificationsSupported();

} // namespace fuse::app
