// fuse_app custom window chrome on Win32 (see chrome.hpp): Custom frames keep WS_OVERLAPPEDWINDOW (snap, maximise,
// minimise animations, Win11 snap layouts on HTMAXBUTTON) but WM_NCCALCSIZE gives the whole window to the client, and
// WM_NCHITTEST answers from chromeHitTest(). Borderless frames are WS_POPUP with the same hit test, without edges.
#include "chrome_internal.hpp"

#if defined(_WIN32)
#include "fuse/app/chrome.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fuse::app::detail {
using namespace fuse::ui;

namespace {
constexpr const wchar_t* kDownProp = L"FuseChromeDown"; // HT* code of the title-bar button under a press

LRESULT toHt(HitArea a) {
    switch (a) {
    case HitArea::Caption: return HTCAPTION;
    case HitArea::Left: return HTLEFT;
    case HitArea::Right: return HTRIGHT;
    case HitArea::Top: return HTTOP;
    case HitArea::Bottom: return HTBOTTOM;
    case HitArea::TopLeft: return HTTOPLEFT;
    case HitArea::TopRight: return HTTOPRIGHT;
    case HitArea::BottomLeft: return HTBOTTOMLEFT;
    case HitArea::BottomRight: return HTBOTTOMRIGHT;
    case HitArea::Minimize: return HTMINBUTTON;
    case HitArea::Maximize: return HTMAXBUTTON;
    case HitArea::Close: return HTCLOSE;
    default: return HTCLIENT;
    }
}
bool isButton(WPARAM ht) { return ht == HTMINBUTTON || ht == HTMAXBUTTON || ht == HTCLOSE; }
Vec2 clientPos(HWND hwnd, LPARAM lp, f32 scale) { // screen pixels -> UI units (the window's DPI scale)
    POINT pt{short(LOWORD(lp)), short(HIWORD(lp))};
    ScreenToClient(hwnd, &pt);
    return {f32(pt.x) / scale, f32(pt.y) / scale};
}
void command(void* user, AppWindow&, WindowCommand c) {
    HWND hwnd = static_cast<HWND>(user);
    switch (c) {
    case WindowCommand::Minimize: ShowWindow(hwnd, SW_MINIMIZE); break;
    case WindowCommand::ToggleMaximize: ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
    case WindowCommand::Close: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
    }
}
} // namespace

unsigned long chromeWin32Style(WindowFrame f) {
    switch (f) {
    case WindowFrame::Borderless: return WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX;
    default: return WS_OVERLAPPEDWINDOW;
    }
}

void chromeWin32Attach(void* h, AppWindow& w) {
    HWND hwnd = static_cast<HWND>(h);
    WindowChrome& c = w.chrome();
    c.user = hwnd;
    c.command = &command;
    if (c.frame == WindowFrame::Custom) // the first WM_NCCALCSIZE ran before GWLP_USERDATA was set
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
void chromeWin32Detach(AppWindow& w) {
    WindowChrome& c = w.chrome();
    if (c.command == &command) {
        c.command = nullptr;
        c.user = nullptr;
    }
}

bool chromeWin32Message(void* h, AppWindow& w, unsigned msg, std::uintptr_t wp, std::intptr_t lp, std::intptr_t& result) {
    WindowChrome& c = w.chrome();
    if (c.frame == WindowFrame::System) return false;
    HWND hwnd = static_cast<HWND>(h);
    switch (msg) {
    case WM_NCCALCSIZE:
        if (!wp || c.frame != WindowFrame::Custom) return false;
        if (IsZoomed(hwnd)) {
            // a maximised window extends past the monitor by its frame; keep the client on the monitor
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            const int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            const int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            p->rgrc[0].left += fx;
            p->rgrc[0].right -= fx;
            p->rgrc[0].top += fy;
            p->rgrc[0].bottom -= fy;
        }
        result = 0;
        return true;
    case WM_NCACTIVATE: // keep DefWindowProc from painting a classic frame over the client
        result = DefWindowProcW(hwnd, msg, wp, -1);
        return true;
    case WM_SIZE:
        c.maximized = wp == SIZE_MAXIMIZED;
        return false;
    case WM_NCHITTEST:
        result = toHt(chromeHitTest(w, clientPos(hwnd, lp, w.scale())));
        return true;
    case WM_NCMOUSEMOVE: // hover feedback for the title-bar buttons (they are non-client for the OS)
        w.ui().pointerMove(clientPos(hwnd, lp, w.scale()));
        if (!isButton(wp)) return false;
        result = 0;
        return true;
    case WM_NCLBUTTONDOWN:
        if (!isButton(wp)) return false;
        SetPropW(hwnd, kDownProp, reinterpret_cast<HANDLE>(wp));
        result = 0;
        return true;
    case WM_NCLBUTTONUP: {
        if (!isButton(wp)) return false;
        const WPARAM down = reinterpret_cast<WPARAM>(RemovePropW(hwnd, kDownProp));
        if (down == wp)
            w.command(wp == HTMINBUTTON ? WindowCommand::Minimize : wp == HTMAXBUTTON ? WindowCommand::ToggleMaximize : WindowCommand::Close);
        result = 0;
        return true;
    }
    case WM_NCLBUTTONDBLCLK:
        if (!isButton(wp)) return false; // caption double click maximises through DefWindowProc
        result = 0;
        return true;
    default: return false;
    }
}

} // namespace fuse::app::detail
#endif
