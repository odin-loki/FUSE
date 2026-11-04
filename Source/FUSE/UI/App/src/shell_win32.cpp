// fuse_app shell services on Win32 (see shell.hpp): Shell_NotifyIconW tray icons with a hidden callback window each,
// TrackPopupMenu context menus and NIF_INFO notifications (toasts on Windows 10 / 11).
#include "fuse/app/shell.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

namespace fuse::app {
using namespace fuse::ui;

namespace {
constexpr UINT kCallback = WM_APP + 0x31;
constexpr const wchar_t* kTrayClass = L"FuseTrayWindow";

std::wstring widen(std::string_view s) {
    std::wstring w(std::size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}
template <std::size_t N>
void copyTo(wchar_t (&dst)[N], std::string_view s) {
    const std::wstring w = widen(s);
    const std::size_t n = std::min(w.size(), N - 1);
    std::copy_n(w.data(), n, dst);
    dst[n] = 0;
}
// fuse::ui::Image pixels are 0xAABBGGRR; the icon DIB wants 0xAARRGGBB
HICON makeIcon(const Image& img) {
    if (img.width <= 0 || img.height <= 0) return nullptr;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = img.width;
    bi.bV5Height = -img.height;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000u;
    bi.bV5GreenMask = 0x0000FF00u;
    bi.bV5BlueMask = 0x000000FFu;
    bi.bV5AlphaMask = 0xFF000000u;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    u32* dst = static_cast<u32*>(bits);
    for (std::size_t i = 0; i < img.pixels.size(); ++i) {
        const u32 p = img.pixels[i];
        dst[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
    }
    HBITMAP mask = CreateBitmap(img.width, img.height, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}
UINT taskbarCreated() {
    static const UINT m = RegisterWindowMessageW(L"TaskbarCreated");
    return m;
}
UINT nextId() {
    static UINT id = 1;
    return id++;
}
} // namespace

struct TrayIcon::Impl {
    HWND hwnd = nullptr;
    UINT id = nextId();
    HICON icon = nullptr;
    std::string tip;
    bool added = false;
    std::function<void()> click, dblClick, noteClick;
    std::function<void(unsigned, std::string_view)> menuFn;
    std::vector<std::string> items; ///< "" = separator, leading '!' = disabled
    unsigned menuCount = 0;

    NOTIFYICONDATAW base(UINT flags) const {
        NOTIFYICONDATAW d{};
        d.cbSize = sizeof d;
        d.hWnd = hwnd;
        d.uID = id;
        d.uFlags = flags;
        return d;
    }
    bool add() {
        NOTIFYICONDATAW d = base(NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
        d.uCallbackMessage = kCallback;
        d.hIcon = icon;
        copyTo(d.szTip, tip);
        if (!Shell_NotifyIconW(NIM_ADD, &d)) return false;
        d.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &d);
        added = true;
        return true;
    }
    void openMenu(int x, int y) {
        if (items.empty()) return;
        HMENU menu = CreatePopupMenu();
        unsigned index = 0;
        for (const std::string& it : items) {
            if (it.empty()) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                continue;
            }
            const bool off = it[0] == '!';
            AppendMenuW(menu, MF_STRING | (off ? MF_GRAYED : 0u), UINT_PTR(index + 1), widen(off ? std::string_view(it).substr(1) : it).c_str());
            ++index;
        }
        SetForegroundWindow(hwnd); // otherwise the menu does not close on an outside click
        const UINT pick = UINT(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, 0, hwnd, nullptr));
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (pick) pickItem(pick - 1);
    }
    void pickItem(unsigned index) {
        unsigned i = 0;
        for (const std::string& it : items) {
            if (it.empty()) continue;
            if (i++ == index) {
                if (menuFn) menuFn(index, it[0] == '!' ? std::string_view(it).substr(1) : std::string_view(it));
                return;
            }
        }
    }
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* m = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (m && msg == kCallback) {
            const UINT ev = LOWORD(lp);
            const int x = short(LOWORD(wp)), y = short(HIWORD(wp));
            switch (ev) {
            case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP: if (m->click) m->click(); break;
            case WM_LBUTTONDBLCLK: if (m->dblClick) m->dblClick(); break;
            case WM_CONTEXTMENU: m->openMenu(x, y); break;
            case WM_RBUTTONUP: { POINT pt; GetCursorPos(&pt); m->openMenu(pt.x, pt.y); break; } // pre-v4 shells
            case NIN_BALLOONUSERCLICK: if (m->noteClick) m->noteClick(); break;
            default: break;
            }
            return 0;
        }
        if (m && msg == taskbarCreated() && m->added) { // Explorer restarted: the icon is gone, add it again
            m->added = false;
            m->add();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

TrayIcon::TrayIcon() : m(std::make_unique<Impl>()) {}
TrayIcon::~TrayIcon() {
    remove();
    if (m->hwnd) DestroyWindow(m->hwnd);
    if (m->icon) DestroyIcon(m->icon);
}
bool TrayIcon::supported() { return FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr; }
bool TrayIcon::create(const Image& icon, std::string_view tooltip) {
    remove();
    if (!m->hwnd) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = &Impl::proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kTrayClass;
        RegisterClassExW(&wc); // fails harmlessly when already registered
        // a hidden top-level window (message-only windows miss the TaskbarCreated broadcast)
        m->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        if (!m->hwnd) return false;
        SetWindowLongPtrW(m->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(m.get()));
        taskbarCreated();
    }
    if (m->icon) DestroyIcon(m->icon);
    m->icon = makeIcon(icon);
    if (!m->icon) m->icon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    m->tip = tooltip;
    return m->add();
}
void TrayIcon::remove() {
    if (!m->added) return;
    NOTIFYICONDATAW d = m->base(0);
    Shell_NotifyIconW(NIM_DELETE, &d);
    m->added = false;
}
bool TrayIcon::shown() const { return m->added; }
bool TrayIcon::setIcon(const Image& icon) {
    HICON h = makeIcon(icon);
    if (!h) return false;
    if (m->icon) DestroyIcon(m->icon);
    m->icon = h;
    if (!m->added) return true;
    NOTIFYICONDATAW d = m->base(NIF_ICON);
    d.hIcon = h;
    return Shell_NotifyIconW(NIM_MODIFY, &d) != 0;
}
bool TrayIcon::setTooltip(std::string_view tooltip) {
    m->tip = tooltip;
    if (!m->added) return true;
    NOTIFYICONDATAW d = m->base(NIF_TIP | NIF_SHOWTIP);
    copyTo(d.szTip, tooltip);
    return Shell_NotifyIconW(NIM_MODIFY, &d) != 0;
}
void TrayIcon::onClick(std::function<void()> fn) { m->click = std::move(fn); }
void TrayIcon::onDoubleClick(std::function<void()> fn) { m->dblClick = std::move(fn); }
void TrayIcon::onNotificationClick(std::function<void()> fn) { m->noteClick = std::move(fn); }
void TrayIcon::setMenu(std::string_view items, std::function<void(unsigned, std::string_view)> fn) {
    m->items.clear();
    m->menuCount = 0;
    while (!items.empty()) {
        const std::size_t nl = items.find('\n');
        const std::string_view line = items.substr(0, nl);
        m->items.emplace_back(line == "-" ? std::string_view{} : line);
        if (line != "-") ++m->menuCount;
        if (nl == std::string_view::npos) break;
        items.remove_prefix(nl + 1);
    }
    m->menuFn = std::move(fn);
}
unsigned TrayIcon::menuItemCount() const { return m->menuCount; }
void TrayIcon::openMenu(Vec2 screen) { if (m->hwnd) m->openMenu(int(screen.x), int(screen.y)); }
NotifyResult TrayIcon::notify(std::string_view title, std::string_view body) {
    if (!m->added) return NotifyResult::Unsupported;
    NOTIFYICONDATAW d = m->base(NIF_INFO);
    copyTo(d.szInfoTitle, title);
    copyTo(d.szInfo, body);
    d.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    return Shell_NotifyIconW(NIM_MODIFY, &d) ? NotifyResult::Shown : NotifyResult::Failed;
}
bool TrayIcon::screenRect(Rect& out) const {
    if (!m->added) return false;
    NOTIFYICONIDENTIFIER nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = m->hwnd;
    nid.uID = m->id;
    RECT rc{};
    if (FAILED(Shell_NotifyIconGetRect(&nid, &rc))) return false;
    out = {f32(rc.left), f32(rc.top), f32(rc.right - rc.left), f32(rc.bottom - rc.top)};
    return true;
}
void TrayIcon::poll() {
    MSG msg;
    while (m->hwnd && PeekMessageW(&msg, m->hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
unsigned long long TrayIcon::nativeWindow() const { return reinterpret_cast<unsigned long long>(m->hwnd); }
unsigned TrayIcon::callbackMessage() const { return kCallback; }
unsigned TrayIcon::iconId() const { return m->id; }

namespace {
// notify() without an app tray icon: one transient icon, removed when the process exits
TrayIcon* transientIcon() {
    static std::unique_ptr<TrayIcon> icon;
    if (!icon) {
        icon = std::make_unique<TrayIcon>();
        Image img;
        img.resize(32, 32, 0xFFD08030u); // plain blue-ish square (0xAABBGGRR)
        if (!icon->create(img, "Notifications")) {
            icon.reset();
            return nullptr;
        }
    }
    return icon.get();
}
} // namespace

NotifyResult notify(std::string_view title, std::string_view body, std::string_view appName) {
    if (!TrayIcon::supported()) return NotifyResult::Unsupported;
    TrayIcon* t = transientIcon();
    if (!t) return NotifyResult::Failed;
    t->setTooltip(appName);
    return t->notify(title, body);
}
bool notificationsSupported() { return TrayIcon::supported(); }

} // namespace fuse::app
