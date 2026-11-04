// fuse_app platform services, Win32: DPI awareness, monitors, system theme, cursors, WM_POINTER touch / pen,
// IFileDialog file / folder pickers, message boxes, ShellExecute / SHOpenFolderAndSelectItems.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "platform_internal.hpp"

#include <fuse/platform/events_v2.hpp>

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwchar>

namespace fuse::app {
using fuse::ui::Color;
using fuse::ui::CursorShape;
using fuse::ui::Rect;

namespace {
std::wstring wide(std::string_view s) {
    std::wstring w(std::size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}
std::string utf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(std::max(0, n - 1)), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
bool regDword(const wchar_t* key, const wchar_t* name, DWORD& out) {
    DWORD size = sizeof out;
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
}
template <class F>
F proc(const wchar_t* dll, const char* name) {
    HMODULE m = GetModuleHandleW(dll);
    if (!m) m = LoadLibraryW(dll);
    return m ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}
/// COM for the calling thread for the duration of a dialog (STA when the thread has none yet).
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};
template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
};
} // namespace

const char* platformErrorName(PlatformError e) {
    switch (e) {
    case PlatformError::Cancelled: return "cancelled";
    case PlatformError::Unavailable: return "unavailable";
    case PlatformError::InvalidArgument: return "invalid argument";
    case PlatformError::Failed: return "failed";
    }
    return "?";
}

namespace detail {

// ---- DPI -------------------------------------------------------------------------------------------------------------
void winEnableDpiAwareness() {
    static const bool done = [] {
        using SetCtx = BOOL(WINAPI*)(HANDLE);
        if (auto set = proc<SetCtx>(L"user32.dll", "SetProcessDpiAwarenessContext")) {
            if (set(reinterpret_cast<HANDLE>(-4))) return true; // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            if (GetLastError() == ERROR_ACCESS_DENIED) return true; // already set (manifest / host)
            if (set(reinterpret_cast<HANDLE>(-3))) return true; // per-monitor v1
        }
        SetProcessDPIAware();
        return true;
    }();
    (void)done;
}
float winWindowScale(void* hwnd) {
    using GetDpi = UINT(WINAPI*)(HWND);
    static const auto get = proc<GetDpi>(L"user32.dll", "GetDpiForWindow");
    const UINT dpi = get ? get(static_cast<HWND>(hwnd)) : 96u;
    return dpi ? float(dpi) / 96.f : 1.f;
}
void winOuterSize(void* hwnd, unsigned w, unsigned h, unsigned dpi, int& ow, int& oh) {
    using Adjust = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto adjust = proc<Adjust>(L"user32.dll", "AdjustWindowRectExForDpi");
    const HWND hw = static_cast<HWND>(hwnd);
    const DWORD style = DWORD(GetWindowLongPtrW(hw, GWL_STYLE)), ex = DWORD(GetWindowLongPtrW(hw, GWL_EXSTYLE));
    RECT rc{0, 0, LONG(w), LONG(h)};
    if (adjust) adjust(&rc, style, FALSE, ex, dpi);
    else AdjustWindowRectEx(&rc, style, FALSE, ex);
    ow = rc.right - rc.left;
    oh = rc.bottom - rc.top;
}

// ---- theme -----------------------------------------------------------------------------------------------------------
bool winSetDarkTitleBar(void* hwnd, bool dark) {
    const BOOL v = dark ? TRUE : FALSE;
    if (SUCCEEDED(DwmSetWindowAttribute(static_cast<HWND>(hwnd), 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &v, sizeof v)))
        return true;
    return SUCCEEDED(DwmSetWindowAttribute(static_cast<HWND>(hwnd), 19 /*pre-20H1 builds*/, &v, sizeof v));
}
SystemTheme winQueryTheme() {
    SystemTheme t;
    DWORD v = 1;
    t.dark = regDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", v) && v == 0;
    DWORD accent = 0;
    BOOL opaque = FALSE;
    if (regDword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", accent)) { // 0xAABBGGRR
        t.accent = Color::rgb(((accent & 0xFFu) << 16) | (accent & 0xFF00u) | ((accent >> 16) & 0xFFu));
    } else if (SUCCEEDED(DwmGetColorizationColor(&accent, &opaque))) { // 0xAARRGGBB
        t.accent = Color::rgb(accent & 0xFFFFFFu);
    }
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof hc;
    t.highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    BOOL anim = TRUE;
    t.reducedMotion = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0) && !anim;
    DWORD ts = 100;
    if (regDword(L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", ts)) t.textScale = std::clamp(float(ts) / 100.f, 1.f, 2.25f);
    return t;
}
bool winIsThemeMessage(unsigned msg) {
    return msg == WM_SETTINGCHANGE || msg == WM_THEMECHANGED || msg == 0x0320 /*WM_DWMCOLORIZATIONCOLORCHANGED*/ ||
           msg == WM_SYSCOLORCHANGE;
}

// ---- monitors --------------------------------------------------------------------------------------------------------
std::vector<DisplayInfo> winDisplays() {
    std::vector<DisplayInfo> out;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM user) -> BOOL {
        auto& v = *reinterpret_cast<std::vector<DisplayInfo>*>(user);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(mon, &mi)) return TRUE;
        DisplayInfo d;
        d.name = utf8(mi.szDevice);
        d.bounds = {float(mi.rcMonitor.left), float(mi.rcMonitor.top), float(mi.rcMonitor.right - mi.rcMonitor.left),
                    float(mi.rcMonitor.bottom - mi.rcMonitor.top)};
        d.work = {float(mi.rcWork.left), float(mi.rcWork.top), float(mi.rcWork.right - mi.rcWork.left),
                  float(mi.rcWork.bottom - mi.rcWork.top)};
        d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        using GetDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        static const auto getDpi = proc<GetDpi>(L"shcore.dll", "GetDpiForMonitor");
        UINT dx = 96, dy = 96;
        if (getDpi && SUCCEEDED(getDpi(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy))) d.scale = float(dx) / 96.f;
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            d.refreshHz = float(dm.dmDisplayFrequency);
        v.push_back(std::move(d));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    return out;
}

// ---- cursors ---------------------------------------------------------------------------------------------------------
void winApplyCursor(ServiceState& st, CursorShape c) {
    static const HCURSOR table[] = {
        LoadCursorA(nullptr, IDC_ARROW),  LoadCursorA(nullptr, IDC_ARROW),    LoadCursorA(nullptr, IDC_IBEAM),
        LoadCursorA(nullptr, IDC_HAND),   LoadCursorA(nullptr, IDC_SIZENS),   LoadCursorA(nullptr, IDC_SIZEWE),
        LoadCursorA(nullptr, IDC_SIZENWSE), LoadCursorA(nullptr, IDC_SIZENESW), LoadCursorA(nullptr, IDC_SIZEALL),
        LoadCursorA(nullptr, IDC_WAIT),   LoadCursorA(nullptr, IDC_NO),       LoadCursorA(nullptr, IDC_CROSS),
    };
    static_assert(sizeof(table) / sizeof(table[0]) == std::size_t(CursorShape::Count));
    if (c == CursorShape::Auto) c = CursorShape::Arrow;
    SetCursor(table[std::size_t(c)]);
    st.cursor = c;
}

// ---- touch / pen -----------------------------------------------------------------------------------------------------
bool winPointerEvent(void* hwnd, unsigned msg, std::uintptr_t wp, std::intptr_t lp, fuse::platform::EventV2& out) {
    using T = fuse::platform::EventV2Type;
    if (msg != WM_POINTERDOWN && msg != WM_POINTERUPDATE && msg != WM_POINTERUP) return false;
    const UINT32 id = GET_POINTERID_WPARAM(wp);
    POINTER_INPUT_TYPE type = PT_POINTER;
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    float pressure = 1.f;
    bool eraser = false;
    if (GetPointerType(id, &type)) {
        if (type == PT_TOUCH) {
            POINTER_TOUCH_INFO ti{};
            if (!GetPointerTouchInfo(id, &ti)) return false;
            pt = ti.pointerInfo.ptPixelLocation;
        } else if (type == PT_PEN) {
            POINTER_PEN_INFO pi{};
            if (!GetPointerPenInfo(id, &pi)) return false;
            pt = pi.pointerInfo.ptPixelLocation;
            pressure = (pi.penMask & PEN_MASK_PRESSURE) ? float(pi.pressure) / 1024.f : 1.f;
            eraser = (pi.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0;
        } else {
            return false; // mouse / touchpad: Windows sends mouse messages
        }
    } else {
        // An id the pointer stack does not know (a message posted by a test without injection): touch at lParam.
        type = PT_TOUCH;
    }
    ScreenToClient(static_cast<HWND>(hwnd), &pt);
    const bool pen = type == PT_PEN;
    out = {};
    out.type = msg == WM_POINTERDOWN ? (pen ? T::PenDown : T::TouchDown)
             : msg == WM_POINTERUP   ? (pen ? T::PenUp : T::TouchUp)
                                     : (pen ? T::PenMove : T::TouchMove);
    out.pointerId = id;
    out.x = float(pt.x);
    out.y = float(pt.y);
    out.pressure = msg == WM_POINTERUP ? 0.f : pressure;
    out.penEraser = eraser;
    return true;
}

SystemTheme queryTheme(const NativeOptions& o) { return o.themeQuery ? o.themeQuery(o.themeQueryUser) : winQueryTheme(); }

} // namespace detail

// ---- dialogs ---------------------------------------------------------------------------------------------------------
namespace {
std::expected<std::vector<std::string>, PlatformError> runFileDialog(const FileDialogDesc& d, bool save, bool folder) {
    ComScope com;
    ComPtr<IFileDialog> dlg;
    HRESULT hr = save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog,
                                         reinterpret_cast<void**>(dlg.put()))
                      : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                         reinterpret_cast<void**>(dlg.put()));
    if (FAILED(hr) || !dlg.p) return std::unexpected(PlatformError::Unavailable);
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    if (folder) opts |= FOS_PICKFOLDERS;
    if (d.multiple && !save && !folder) opts |= FOS_ALLOWMULTISELECT;
    dlg->SetOptions(opts);
    if (!d.title.empty()) dlg->SetTitle(wide(d.title).c_str());
    std::vector<std::wstring> names, specs;
    std::vector<COMDLG_FILTERSPEC> filters;
    if (!folder && !d.filters.empty()) {
        for (const FileFilter& f : d.filters) {
            names.push_back(wide(f.name));
            specs.push_back(wide(f.patterns));
        }
        for (std::size_t i = 0; i < names.size(); ++i) filters.push_back({names[i].c_str(), specs[i].c_str()});
        dlg->SetFileTypes(UINT(filters.size()), filters.data());
        if (save) { // "*.png;*.jpg" -> default extension "png"
            const std::wstring& s = specs[0];
            const std::size_t dot = s.find(L"*.");
            if (dot != std::wstring::npos) {
                const std::wstring ext = s.substr(dot + 2, s.find(L';', dot) - dot - 2);
                if (!ext.empty() && ext != L"*") dlg->SetDefaultExtension(ext.c_str());
            }
        }
    }
    if (!d.folder.empty()) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(wide(d.folder).c_str(), nullptr, IID_IShellItem,
                                                  reinterpret_cast<void**>(item.put()))))
            dlg->SetFolder(item.p);
    }
    if (!d.fileName.empty()) dlg->SetFileName(wide(d.fileName).c_str());
    hr = dlg->Show(reinterpret_cast<HWND>(d.owner));
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::unexpected(PlatformError::Cancelled);
    if (FAILED(hr)) return std::unexpected(PlatformError::Failed);
    std::vector<std::string> paths;
    auto take = [&](IShellItem* it) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
            paths.push_back(utf8(p));
            CoTaskMemFree(p);
        }
    };
    if (!save && !folder && d.multiple) {
        ComPtr<IShellItemArray> items;
        if (SUCCEEDED(static_cast<IFileOpenDialog*>(dlg.p)->GetResults(items.put()))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                ComPtr<IShellItem> it;
                if (SUCCEEDED(items->GetItemAt(i, it.put()))) take(it.p);
            }
        }
    } else {
        ComPtr<IShellItem> it;
        if (SUCCEEDED(dlg->GetResult(it.put()))) take(it.p);
    }
    if (paths.empty()) return std::unexpected(PlatformError::Failed);
    return paths;
}
} // namespace

std::expected<std::vector<std::string>, PlatformError> openFileDialog(const FileDialogDesc& d) {
    return runFileDialog(d, false, false);
}
std::expected<std::string, PlatformError> saveFileDialog(const FileDialogDesc& d) {
    auto r = runFileDialog(d, true, false);
    if (!r) return std::unexpected(r.error());
    return std::move(r->front());
}
std::expected<std::string, PlatformError> pickFolderDialog(const FileDialogDesc& d) {
    auto r = runFileDialog(d, false, true);
    if (!r) return std::unexpected(r.error());
    return std::move(r->front());
}

std::expected<MessageResult, PlatformError> messageBox(std::string_view title, std::string_view text, MessageIcon icon,
                                                       MessageButtons buttons, unsigned long long owner) {
    UINT flags = MB_SETFOREGROUND;
    switch (icon) {
    case MessageIcon::Info: flags |= MB_ICONINFORMATION; break;
    case MessageIcon::Warning: flags |= MB_ICONWARNING; break;
    case MessageIcon::Error: flags |= MB_ICONERROR; break;
    case MessageIcon::Question: flags |= MB_ICONQUESTION; break;
    }
    switch (buttons) {
    case MessageButtons::Ok: flags |= MB_OK; break;
    case MessageButtons::OkCancel: flags |= MB_OKCANCEL; break;
    case MessageButtons::YesNo: flags |= MB_YESNO; break;
    case MessageButtons::YesNoCancel: flags |= MB_YESNOCANCEL; break;
    }
    const int r = MessageBoxW(reinterpret_cast<HWND>(owner), wide(text).c_str(), wide(title).c_str(), flags);
    switch (r) {
    case IDOK: return MessageResult::Ok;
    case IDYES: return MessageResult::Yes;
    case IDNO: return MessageResult::No;
    case IDCANCEL: return MessageResult::Cancel;
    case 0: return std::unexpected(PlatformError::Failed);
    default: return MessageResult::Cancel;
    }
}

// ---- shell -----------------------------------------------------------------------------------------------------------
bool isOpenableUrl(std::string_view url) {
    auto starts = [&](std::string_view p) {
        if (url.size() < p.size()) return false;
        for (std::size_t i = 0; i < p.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(url[i])) != p[i]) return false;
        return true;
    };
    if (url.find_first_of("\r\n\"") != std::string_view::npos) return false;
    return (starts("http://") && url.size() > 7) || (starts("https://") && url.size() > 8) || (starts("mailto:") && url.size() > 7) ||
           (starts("file:") && url.size() > 5);
}
std::expected<void, PlatformError> openUrl(std::string_view url) {
    if (!isOpenableUrl(url)) return std::unexpected(PlatformError::InvalidArgument);
    ComScope com;
    const auto r = reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) return std::unexpected(PlatformError::Failed);
    return {};
}
std::expected<void, PlatformError> revealInFileManager(std::string_view path) {
    if (path.empty()) return std::unexpected(PlatformError::InvalidArgument);
    const std::wstring w = wide(path);
    if (GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES) return std::unexpected(PlatformError::Failed);
    ComScope com;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(w.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) return std::unexpected(PlatformError::Failed);
    const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    CoTaskMemFree(pidl);
    if (FAILED(hr)) return std::unexpected(PlatformError::Failed);
    return {};
}

} // namespace fuse::app
#endif
