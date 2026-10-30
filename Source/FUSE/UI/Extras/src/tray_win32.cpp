// Windows tray (Shell_NotifyIconW, popup menu via TrackPopupMenu) and global menu (native HMENU bar on the window).
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <map>

#include "tray_internal.hpp"

namespace fuse::ui::tray_detail {
namespace {
constexpr UINT kTrayMsg = WM_APP + 0x31;
constexpr UINT kFirstCmd = 0x7100;

std::wstring wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
/// Builds an HMENU from MenuItems; command ids map back to the items ('&' mnemonics are native on Win32).
struct MenuBuilder {
    std::map<UINT, MenuItem*> cmds;
    UINT next = kFirstCmd;
    HMENU build(std::vector<MenuItem>& items, bool bar) {
        HMENU h = bar ? CreateMenu() : CreatePopupMenu();
        for (MenuItem& it : items) {
            if (!it.visible) continue;
            if (it.separator) { AppendMenuW(h, MF_SEPARATOR, 0, nullptr); continue; }
            std::wstring label = wide(it.label);
            if (!it.shortcut.empty()) label += L"\t" + wide(it.shortcut);
            UINT flags = MF_STRING | (it.enabled ? 0 : MF_GRAYED) | (it.checked ? MF_CHECKED : 0);
            if (!it.children.empty()) AppendMenuW(h, flags | MF_POPUP, UINT_PTR(build(it.children, false)), label.c_str());
            else {
                cmds[next] = &it;
                AppendMenuW(h, flags, next++, label.c_str());
            }
        }
        return h;
    }
    bool fire(UINT id, std::vector<MenuItem>& root) {
        auto f = cmds.find(id);
        if (f == cmds.end()) return false;
        MenuItem* it = f->second;
        if (it->checkable) {
            if (it->radio) {
                // uncheck radio siblings: find the vector holding it
                auto walk = [&](auto& self, std::vector<MenuItem>& v) -> bool {
                    for (MenuItem& m : v) if (&m == it) { for (MenuItem& o : v) if (o.radio) o.checked = false; return true; }
                    for (MenuItem& m : v) if (self(self, m.children)) return true;
                    return false;
                };
                walk(walk, root);
                it->checked = true;
            } else it->checked = !it->checked;
        }
        if (auto fn = it->onTriggered) fn();
        return true;
    }
};

HICON iconFrom(const Image& img) {
    if (img.width <= 0 || img.height <= 0) return LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = img.width;
    bi.bV5Height = -img.height;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    auto* px = static_cast<u32*>(bits);
    for (std::size_t i = 0; i < img.pixels.size(); ++i) { // premultiplied RGBA -> premultiplied BGRA (icons accept it)
        const u32 p = img.pixels[i];
        px[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
    }
    HBITMAP mask = CreateBitmap(img.width, img.height, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return h;
}

class Win32Tray final : public TrayBackend {
public:
    explicit Win32Tray(TrayState& st) : m_st(st) {}
    ~Win32Tray() override { hide(); }
    bool show() override {
        if (m_visible) return true;
        static ATOM cls = [] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"FuseTrayWindow";
            return RegisterClassW(&wc);
        }();
        if (!cls) return false;
        m_hwnd = CreateWindowExW(0, L"FuseTrayWindow", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!m_hwnd) return false;
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, LONG_PTR(this));
        NOTIFYICONDATAW nd = data(NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
        m_visible = Shell_NotifyIconW(NIM_ADD, &nd);
        if (m_visible) { nd.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &nd); }
        return m_visible;
    }
    void hide() override {
        if (m_visible) { NOTIFYICONDATAW nd = data(0); Shell_NotifyIconW(NIM_DELETE, &nd); }
        if (m_icon) { DestroyIcon(m_icon); m_icon = nullptr; }
        if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
        m_visible = false;
    }
    bool visible() const override { return m_visible; }
    void changed(u32 what) override {
        if (!m_visible) return;
        if (what & (kIcon | kToolTip | kTitle | kStatus)) {
            NOTIFYICONDATAW nd = data(NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_STATE);
            Shell_NotifyIconW(NIM_MODIFY, &nd);
        }
    }
    void poll() override {
        MSG msg;
        while (m_hwnd && PeekMessageW(&msg, m_hwnd, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    std::string serviceName() const override { return "Shell_NotifyIcon"; }

private:
    NOTIFYICONDATAW data(UINT flags) {
        NOTIFYICONDATAW nd{};
        nd.cbSize = sizeof nd;
        nd.hWnd = m_hwnd;
        nd.uID = 1;
        nd.uFlags = flags;
        nd.uCallbackMessage = kTrayMsg;
        if (flags & NIF_ICON) {
            if (m_icon) DestroyIcon(m_icon);
            m_icon = iconFrom(m_st.icon);
            nd.hIcon = m_icon;
        }
        const std::wstring tip = wide(m_st.ttTitle.empty() ? m_st.title : m_st.ttTitle + (m_st.ttBody.empty() ? "" : "\n" + m_st.ttBody));
        wcsncpy_s(nd.szTip, tip.c_str(), _TRUNCATE);
        if (flags & NIF_STATE) { nd.dwStateMask = NIS_HIDDEN; nd.dwState = m_st.status == TrayStatus::Passive ? NIS_HIDDEN : 0; }
        return nd;
    }
    void popup(int x, int y) {
        MenuBuilder b;
        HMENU h = b.build(m_st.menu, false);
        SetForegroundWindow(m_hwnd); // required so the menu closes when focus leaves
        const UINT cmd = UINT(TrackPopupMenu(h, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, x, y, 0, m_hwnd, nullptr));
        DestroyMenu(h);
        if (cmd) b.fire(cmd, m_st.menu);
    }
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Win32Tray*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self && msg == kTrayMsg) {
            const int x = GET_X_LPARAM_(wp), y = GET_Y_LPARAM_(wp);
            switch (LOWORD(lp)) {
            case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP:
                if (self->m_st.owner->onActivated) self->m_st.owner->onActivated(x, y);
                return 0;
            case WM_MBUTTONUP:
                if (self->m_st.owner->onSecondaryActivated) self->m_st.owner->onSecondaryActivated(x, y);
                return 0;
            case WM_CONTEXTMENU: self->popup(x, y); return 0;
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
    static int GET_X_LPARAM_(WPARAM w) { return int(short(LOWORD(w))); }
    static int GET_Y_LPARAM_(WPARAM w) { return int(short(HIWORD(w))); }

    TrayState& m_st;
    HWND m_hwnd = nullptr;
    HICON m_icon = nullptr;
    bool m_visible = false;
};

/// Global menu on Windows is the window's own menu bar: SetMenu + a WM_COMMAND hook chained in front of the
/// window procedure.
class Win32MenuBar final : public MenuBackend {
public:
    explicit Win32MenuBar(u64 w) : m_hwnd(reinterpret_cast<HWND>(w)) {}
    ~Win32MenuBar() override {
        if (m_prev && IsWindow(m_hwnd)) {
            SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, m_prev);
            RemovePropW(m_hwnd, L"FuseMenuBar");
        }
        if (m_menu) { if (IsWindow(m_hwnd)) SetMenu(m_hwnd, nullptr); DestroyMenu(m_menu); }
    }
    bool setMenus(std::vector<MenuItem>& menus) override {
        if (!IsWindow(m_hwnd)) return false;
        m_root = &menus;
        m_builder = MenuBuilder{};
        HMENU h = m_builder.build(menus, true);
        SetMenu(m_hwnd, h);
        if (m_menu) DestroyMenu(m_menu);
        m_menu = h;
        DrawMenuBar(m_hwnd);
        if (!m_prev) {
            SetPropW(m_hwnd, L"FuseMenuBar", this);
            m_prev = SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, LONG_PTR(hook));
        }
        return true;
    }
    bool active() const override { return m_menu != nullptr; }
    void poll() override {}
    std::string objectPath() const override { return "HMENU"; }

private:
    static LRESULT CALLBACK hook(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = static_cast<Win32MenuBar*>(GetPropW(h, L"FuseMenuBar"));
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        if (msg == WM_COMMAND && HIWORD(wp) == 0 && self->m_root && self->m_builder.fire(LOWORD(wp), *self->m_root)) {
            self->setMenus(*self->m_root); // reflect check state
            return 0;
        }
        return CallWindowProcW(reinterpret_cast<WNDPROC>(self->m_prev), h, msg, wp, lp);
    }
    HWND m_hwnd;
    HMENU m_menu = nullptr;
    LONG_PTR m_prev = 0;
    MenuBuilder m_builder;
    std::vector<MenuItem>* m_root = nullptr;
};
} // namespace

std::unique_ptr<TrayBackend> makeTrayBackend(TrayState& st) { return std::make_unique<Win32Tray>(st); }
bool trayAvailable() { return FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr; }
const char* trayBackendName() { return "win32"; }
std::unique_ptr<MenuBackend> makeMenuBackend(u64 w) { return std::make_unique<Win32MenuBar>(w); }
const char* menuBackendName() { return "win32"; }
} // namespace fuse::ui::tray_detail
#endif
