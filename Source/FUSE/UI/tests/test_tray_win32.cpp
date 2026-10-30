// Windows tray + global menu bar under Wine: Shell_NotifyIcon registration, tray callback messages routed to
// TrayIcon callbacks, HMENU bar built from MenuItems and WM_COMMAND routed back (check state toggles).
#include <windows.h>

#include <cstdio>

#include "fuse/ui/extras/tray.hpp"

using namespace fuse::ui;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CHECK(std::string(TrayIcon::backendName()) == "win32" && std::string(GlobalMenuBar::backendName()) == "win32");
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FuseTrayTestWnd";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, L"FuseTrayTestWnd", L"t", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(w != nullptr);

    int newCount = 0;
    {
        GlobalMenuBar bar(u64(reinterpret_cast<uintptr_t>(w)));
        CHECK(bar.setMenus({{.label = "&File", .children = {{.label = "&New", .onTriggered = [&] { ++newCount; }, .shortcut = "Ctrl+N"},
                                                            MenuItem::sep(), {.label = "&Wrap", .checkable = true}}},
                            {.label = "&Help", .children = {{.label = "&About"}}}}));
        HMENU m = GetMenu(w);
        CHECK(m && GetMenuItemCount(m) == 2);
        HMENU file = GetSubMenu(m, 0);
        CHECK(file && GetMenuItemCount(file) == 3);
        const UINT newId = GetMenuItemID(file, 0), wrapId = GetMenuItemID(file, 2);
        SendMessageW(w, WM_COMMAND, MAKEWPARAM(newId, 0), 0);
        CHECK(newCount == 1);
        SendMessageW(w, WM_COMMAND, MAKEWPARAM(wrapId, 0), 0);
        file = GetSubMenu(GetMenu(w), 0);
        CHECK((GetMenuState(file, 2, MF_BYPOSITION) & MF_CHECKED) != 0);
        CHECK(bar.active());
    }

    std::printf("menu bar ok\n");
    TrayIcon tray;
    Image icon; icon.resize(16, 16, 0xFF3366CCu);
    tray.setIcon(icon);
    tray.setToolTip("FUSE", "tray test");
    int act = -1;
    tray.onActivated = [&](int x, int y) { act = x * 1000 + y; };
    const bool shown = tray.show();
    std::printf("tray: Shell_NotifyIcon add -> %d\n", int(shown));
    HWND tw = FindWindowExW(HWND_MESSAGE, nullptr, L"FuseTrayWindow", nullptr);
    CHECK(tw != nullptr);
    if (tw) {
        PostMessageW(tw, WM_APP + 0x31, MAKEWPARAM(12, 34), MAKELPARAM(WM_LBUTTONUP, 1));
        tray.poll();
        CHECK(act == 12034);
    }
    tray.hide();
    CHECK(FindWindowExW(HWND_MESSAGE, nullptr, L"FuseTrayWindow", nullptr) == nullptr);
    DestroyWindow(w);
    std::printf("fuse_ui_tray_win32: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
