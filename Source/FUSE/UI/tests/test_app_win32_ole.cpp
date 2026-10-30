// fuse_app_win32_ole (Windows / Wine): OLE drag and drop and the IFileDialog native dialog.
//   1. the window's registered IDropTarget accepts our own CF_HDROP data object -> onExternalDrop paths
//   2. a real DoDragDrop started by AppWindow::beginExternalDrag lands back on the window (self drop)
//   3. os::nativeFileDialog (IFileOpenDialog) shows, a helper thread presses OK, the preset file comes back
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/os.hpp"

#include <ole2.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace fuse::app::detail { IDataObject* win32MakeDataObject(const AppWindow::ExternalDrag& d); }
namespace app = fuse::app;
using namespace fuse::ui;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main(int argc, char** argv) {
    app::Application a{{.name = "gate_win32_ole", .argc = argc, .argv = argv, .loadSettings = false}};
    auto& win = a.createWindow({.title = "fuse ole", .width = 320, .height = 220});
    std::vector<std::string> got;
    std::string mime;
    int ended = -1, drops = 0;
    win.onExternalDrop([&](app::AppWindow&, const app::AppWindow::ExternalDrop& d) { got = d.paths; mime = d.mime; ++drops; });
    win.onExternalDragEnd([&](bool ok) { ended = ok ? 1 : 0; });
    app::NativePlatform native(a);
    native.show(win);
    for (int i = 0; i < 50; ++i) a.step(0.01f);
    HWND hwnd = reinterpret_cast<HWND>(native.nativeWindow(win));
    CHECK(hwnd != nullptr);
    auto* target = static_cast<IDropTarget*>(GetPropW(hwnd, L"FuseDropTarget"));
    CHECK(target != nullptr);

    // 1. direct IDropTarget round trip
    if (target) {
        IDataObject* data = app::detail::win32MakeDataObject({.paths = {"C:/tmp/a b.txt", "C:/tmp/c.png"}});
        RECT rc;
        GetWindowRect(hwnd, &rc);
        POINTL pt{rc.left + 40, rc.top + 60};
        DWORD eff = DROPEFFECT_COPY;
        CHECK(target->DragEnter(data, MK_LBUTTON, pt, &eff) == S_OK && eff == DROPEFFECT_COPY);
        eff = DROPEFFECT_COPY;
        CHECK(target->Drop(data, 0, pt, &eff) == S_OK && eff == DROPEFFECT_COPY);
        data->Release();
        CHECK(got.size() == 2 && got[0] == "C:/tmp/a b.txt" && got[1] == "C:/tmp/c.png" && mime == "text/uri-list");
    }

    // 2. real DoDragDrop: cursor over our client area, button already up -> immediate drop on the window
    got.clear();
    {
        POINT c{100, 100};
        ClientToScreen(hwnd, &c);
        SetForegroundWindow(hwnd);
        SetCursorPos(c.x, c.y);
        CHECK(win.beginExternalDrag({.paths = {"C:/srv/one two.txt"}}));
        SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(100, 100));
        for (int i = 0; i < 50 && ended < 0; ++i) a.step(0.01f);
        CHECK(ended == 1);
        CHECK(got.size() == 1 && got[0] == "C:/srv/one two.txt");
    }

    // 3. IFileOpenDialog with a helper pressing OK
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        std::string dir = tmp;
        const std::string file = "fuse_dialog_pick.txt";
        if (FILE* f = std::fopen((dir + file).c_str(), "wb")) { std::fputs("x", f); std::fclose(f); }
        std::atomic<bool> stop{false}, pressed{false};
        std::thread helper([&] {
            for (int i = 0; i < 400 && !stop; ++i) {
                Sleep(50);
                HWND d = FindWindowW(nullptr, L"FUSE dialog test");
                if (d && IsWindowVisible(d)) {
                    Sleep(300);
                    PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
                    pressed = true;
                    return;
                }
            }
        });
        os::NativeDialogSpec spec;
        spec.title = "FUSE dialog test";
        spec.directory = dir;
        spec.fileName = file;
        spec.filters = {{"Text", {"*.txt"}}, {"All", {"*.*"}}};
        const auto r = os::nativeFileDialog(spec);
        stop = true;
        helper.join();
        CHECK(pressed.load());
        CHECK(!r.unavailable);
        std::string want = dir + file;
        for (auto& ch : want) if (ch == '\\') ch = '/';
        std::string have = r.paths.empty() ? "" : r.paths[0];
        for (auto& ch : have) if (ch == '\\') ch = '/';
        CHECK(r.paths.size() == 1);
        if (_stricmp(have.c_str(), want.c_str()) != 0) { std::fprintf(stderr, "dialog: got '%s' want '%s'\n", have.c_str(), want.c_str()); ++g_fail; }
    }
    std::printf("fuse_app_win32_ole: drops=%d ended=%d %s\n", drops, ended, g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
