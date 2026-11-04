// Tray icon + notification gate (plan UI10, §4.13 Shell). Needs no human:
//   * adds a tray icon from an RGBA raster; the shell accepts it (Shell_NotifyIconGetRect finds it, or NIM_MODIFY of
//     the tooltip / icon succeeds when Windows 11 keeps it in the overflow)
//   * a simulated shell callback message (NIN_SELECT, WM_LBUTTONDBLCLK) fires the click / double-click handlers
//   * the context menu built from popupMenu-format items opens as a real OS popup menu (TrackPopupMenu) on a
//     simulated WM_CONTEXTMENU; keyboard input posted to it picks the second item and the menu handler gets it
//   * a notification (NIF_INFO balloon / toast) is accepted, then the icon is removed
// Exit 77 where there is no tray (X11 for now, or a session without a shell).
#include "fuse/app/shell.hpp"

#include <cstdio>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

using namespace fuse::ui;
namespace app = fuse::app;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

#if defined(_WIN32)
namespace {
bool g_menuSeen = false;
// Runs inside TrackPopupMenu's modal loop (a thread timer): the menu window (#32768) is up; pick item 2 by keyboard.
void CALLBACK menuTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    HWND menu = FindWindowW(L"#32768", nullptr);
    g_menuSeen = menu && IsWindowVisible(menu);
    if (!menu) { EndMenu(); return; }
    PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0);
    PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0);
    PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 0);
}
void CALLBACK menuWatchdog(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    EndMenu(); // never leave a menu open if the keys did not land
}
} // namespace
#endif

int main() {
    if (!app::TrayIcon::supported()) {
        std::printf("fuse_app_tray: skipped (no system tray on this platform / session)\n");
        return 77;
    }
#if defined(_WIN32)
    Image icon;
    icon.resize(32, 32, 0xFF000000u);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) icon.pixels[std::size_t(y * 32 + x)] = (x - 16) * (x - 16) + (y - 16) * (y - 16) < 196 ? 0xFF30A0FFu : 0x00000000u;

    app::TrayIcon tray;
    int clicks = 0, doubles = 0;
    unsigned picked = ~0u;
    std::string pickedText;
    tray.onClick([&] { ++clicks; });
    tray.onDoubleClick([&] { ++doubles; });
    tray.setMenu("Open\nSettings\n-\n!Disabled\nQuit", [&](unsigned i, std::string_view t) { picked = i; pickedText = t; });
    CHECK(tray.menuItemCount() == 4);
    if (!tray.create(icon, "fuse_app_tray gate")) {
        std::printf("fuse_app_tray: skipped (the shell refused Shell_NotifyIcon NIM_ADD)\n");
        return 77;
    }
    CHECK(tray.shown());
    Rect r;
    const bool hasRect = tray.screenRect(r);
    const bool modTip = tray.setTooltip("fuse_app_tray gate (modified)");
    const bool modIcon = tray.setIcon(icon);
    std::printf("  icon added: GetRect %s (%.0f,%.0f %.0fx%.0f), NIM_MODIFY tooltip %d icon %d\n", hasRect ? "ok" : "none",
                r.x, r.y, r.w, r.h, modTip, modIcon);
    CHECK(hasRect || (modTip && modIcon));
    CHECK(modTip && modIcon);

    // simulated shell callbacks (NOTIFYICON_VERSION_4 layout: LOWORD(lParam) = event, HIWORD = icon id)
    HWND hwnd = reinterpret_cast<HWND>(tray.nativeWindow());
    const UINT cb = tray.callbackMessage();
    const LPARAM sel = MAKELPARAM(NIN_SELECT, tray.iconId()), dbl = MAKELPARAM(WM_LBUTTONDBLCLK, tray.iconId());
    const WPARAM at = MAKEWPARAM(100, 100);
    SendMessageW(hwnd, cb, at, sel);
    CHECK(clicks == 1);
    PostMessageW(hwnd, cb, at, sel); // posted: delivered by poll()
    PostMessageW(hwnd, cb, at, dbl);
    tray.poll();
    CHECK(clicks == 2 && doubles == 1);
    std::printf("  callbacks: %d clicks, %d double clicks\n", clicks, doubles);

    // context menu: a real TrackPopupMenu, driven by posted keys from a thread timer inside its modal loop
    POINT pt;
    GetCursorPos(&pt);
    SetTimer(nullptr, 0, 150, &menuTimer);
    const UINT_PTR watchdog = SetTimer(nullptr, 0, 3000, &menuWatchdog);
    SendMessageW(hwnd, cb, MAKEWPARAM(pt.x, pt.y), MAKELPARAM(WM_CONTEXTMENU, tray.iconId()));
    KillTimer(nullptr, watchdog);
    std::printf("  context menu: shown %d, picked %d \"%s\"\n", g_menuSeen, int(picked), pickedText.c_str());
    CHECK(g_menuSeen);
    CHECK(picked == 1 && pickedText == "Settings");

    const app::NotifyResult n = tray.notify("FUSE UI", "fuse_app_tray gate notification");
    std::printf("  notification: %s\n", n == app::NotifyResult::Shown ? "shown" : n == app::NotifyResult::Failed ? "failed" : "unsupported");
    CHECK(n == app::NotifyResult::Shown);
    tray.remove();
    CHECK(!tray.shown());
    CHECK(!tray.screenRect(r));
#endif
    if (g_fail) return 1;
    std::printf("fuse_app_tray: OK\n");
    return 0;
}
