// Win32 OLE drag and drop (G3/G4 Windows item): IDropTarget per top-level window -> AppWindow::deliverExternalDrop
// (CF_HDROP files, CF_UNICODETEXT text), and an IDataObject + IDropSource for AppWindow::beginExternalDrag
// (DoDragDrop, CF_HDROP + CF_UNICODETEXT of the uri-list) -> AppWindow::deliverExternalDragEnd. Other MIME types
// travel as registered clipboard formats named by the MIME type (RegisterClipboardFormatW(L"text/html") ...), raw
// bytes in an HGLOBAL, both ways. Also the copy / link drag cursors Windows has no stock IDC_* for.
#if defined(_WIN32)
#include "fuse/app/app.hpp"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace fuse::app::detail {
using namespace fuse::ui;

namespace {
std::wstring wide(std::string_view s) {
    std::wstring w(std::size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}
std::string narrow(const wchar_t* w, int len = -1) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    if (len < 0 && !s.empty()) s.pop_back();
    return s;
}
/// Registered clipboard format of a MIME type (0 for the ones carried as CF_HDROP / CF_UNICODETEXT).
UINT mimeFormat(std::string_view mime) {
    if (mime == "text/uri-list" || mime == "text/plain" || mime.starts_with("text/plain;")) return 0;
    return RegisterClipboardFormatW(wide(mime).c_str());
}
/// The MIME type a registered format was named for ("" when it is not a type/subtype name).
std::string formatMime(UINT cf) {
    if (cf < 0xC000) return {};
    wchar_t name[256];
    const int n = GetClipboardFormatNameW(cf, name, 256);
    std::string s = n > 0 ? narrow(name, n) : std::string();
    return s.find('/') != std::string::npos && s.find(' ') == std::string::npos ? s : std::string();
}
std::string slashes(std::string p) {
    for (auto& c : p) if (c == '\\') c = '/';
    return p;
}

class DropTarget final : public IDropTarget {
public:
    DropTarget(HWND h, AppWindow* w) : hwnd(h), win(w) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        FORMATETC t{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        ok = win && win->acceptsExternalDrops() && (data->QueryGetData(&f) == S_OK || data->QueryGetData(&t) == S_OK || !mimeFormats(data).empty());
        return DragOver(0, pt, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        *effect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (ok) { win->ui().pointerMove(client(pt)); win->requestRedraw(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { ok = false; return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        *effect = DROPEFFECT_NONE;
        if (!ok) return S_OK;
        AppWindow::ExternalDrop d;
        d.pos = client(pt);
        FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        FORMATETC t{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM m{};
        if (data->GetData(&f, &m) == S_OK) {
            HDROP h = static_cast<HDROP>(GlobalLock(m.hGlobal));
            const UINT n = h ? DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0) : 0;
            for (UINT i = 0; i < n; ++i) {
                std::wstring p(DragQueryFileW(h, i, nullptr, 0), L'\0');
                DragQueryFileW(h, i, p.data(), UINT(p.size() + 1));
                d.paths.push_back(slashes(narrow(p.c_str(), int(p.size()))));
            }
            if (h) GlobalUnlock(m.hGlobal);
            ReleaseStgMedium(&m);
            d.mime = "text/uri-list";
            d.data = AppWindow::uriListFromPaths(d.paths);
        } else if (data->GetData(&t, &m) == S_OK) {
            if (auto* w = static_cast<const wchar_t*>(GlobalLock(m.hGlobal))) {
                d.data = narrow(w);
                GlobalUnlock(m.hGlobal);
            }
            ReleaseStgMedium(&m);
            d.mime = "text/plain";
        }
        if (!d.mime.empty()) d.formats.emplace_back(d.mime, d.data);
        for (const auto& [cf, mime] : mimeFormats(data)) { // registered MIME formats: raw bytes
            if (d.formats.size() >= 8) break;
            FORMATETC g{CLIPFORMAT(cf), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            if (data->GetData(&g, &m) != S_OK) continue;
            if (const void* p = GlobalLock(m.hGlobal)) {
                d.formats.emplace_back(mime, std::string(static_cast<const char*>(p), GlobalSize(m.hGlobal)));
                GlobalUnlock(m.hGlobal);
            }
            ReleaseStgMedium(&m);
        }
        if (d.mime.empty() && !d.formats.empty()) { d.mime = d.formats.front().first; d.data = d.formats.front().second; }
        if (d.formats.empty()) { ok = false; return S_OK; }
        ok = false;
        win->deliverExternalDrop(d);
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }
    HWND hwnd;
    AppWindow* win;

private:
    static std::vector<std::pair<UINT, std::string>> mimeFormats(IDataObject* data) {
        std::vector<std::pair<UINT, std::string>> out;
        IEnumFORMATETC* e = nullptr;
        if (data->EnumFormatEtc(DATADIR_GET, &e) != S_OK || !e) return out;
        FORMATETC f;
        while (e->Next(1, &f, nullptr) == S_OK) {
            if (f.ptd) CoTaskMemFree(f.ptd);
            if (!(f.tymed & TYMED_HGLOBAL)) continue;
            if (std::string m = formatMime(f.cfFormat); !m.empty()) out.emplace_back(f.cfFormat, std::move(m));
        }
        e->Release();
        return out;
    }
    Vec2 client(POINTL pt) const {
        POINT p{pt.x, pt.y};
        ScreenToClient(hwnd, &p);
        return {f32(p.x), f32(p.y)};
    }
    std::atomic<ULONG> refs{1};
    bool ok = false;
};

/// Static data object: CF_HDROP (+ CF_UNICODETEXT of the paths / text).
class DataObject final : public IDataObject {
public:
    HGLOBAL hdrop = nullptr, text = nullptr;
    std::vector<std::pair<UINT, HGLOBAL>> extra; ///< registered MIME formats
    ~DataObject() {
        if (hdrop) GlobalFree(hdrop);
        if (text) GlobalFree(text);
        for (auto& [cf, g] : extra) GlobalFree(g);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --refs; if (!r) delete this; return r; }
    HGLOBAL pick(const FORMATETC* f) const {
        if (!(f->tymed & TYMED_HGLOBAL) || f->dwAspect != DVASPECT_CONTENT) return nullptr;
        if (f->cfFormat == CF_HDROP) return hdrop;
        if (f->cfFormat == CF_UNICODETEXT) return text;
        for (const auto& [cf, g] : extra) if (cf == f->cfFormat) return g;
        return nullptr;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* m) override {
        HGLOBAL src = pick(f);
        if (!src) return DV_E_FORMATETC;
        const SIZE_T n = GlobalSize(src);
        HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, n);
        if (!copy) return E_OUTOFMEMORY;
        std::memcpy(GlobalLock(copy), GlobalLock(src), n);
        GlobalUnlock(src);
        GlobalUnlock(copy);
        m->tymed = TYMED_HGLOBAL;
        m->hGlobal = copy;
        m->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override { return pick(f) ? S_OK : DV_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override { out->ptd = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override {
        if (dir != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> fs;
        if (hdrop) fs.push_back({CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        if (text) fs.push_back({CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        for (const auto& [cf, g] : extra) fs.push_back({CLIPFORMAT(cf), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        return SHCreateStdEnumFmtEtc(UINT(fs.size()), fs.data(), out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    std::atomic<ULONG> refs{1};
};

class DropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    std::atomic<ULONG> refs{1};
};

HGLOBAL globalOf(const void* p, std::size_t n) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, n);
    if (g) { std::memcpy(GlobalLock(g), p, n); GlobalUnlock(g); }
    return g;
}
} // namespace

IDataObject* win32MakeDataObject(const AppWindow::ExternalDrag& d) {
    auto* o = new DataObject;
    if (!d.paths.empty()) {
        std::wstring list;
        for (const auto& p : d.paths) {
            std::wstring w = wide(p);
            for (auto& c : w) if (c == L'/') c = L'\\';
            list += w;
            list += L'\0';
        }
        list += L'\0';
        std::vector<char> buf(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
        auto* df = reinterpret_cast<DROPFILES*>(buf.data());
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        std::memcpy(buf.data() + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
        o->hdrop = globalOf(buf.data(), buf.size());
    }
    std::string txt = !d.paths.empty() ? AppWindow::uriListFromPaths(d.paths) : std::string();
    for (const auto& [mime, data] : d.offered()) {
        if (mime == "text/plain") txt = data; // the text wins over the paths' uri-list for CF_UNICODETEXT
        else if (const UINT cf = mimeFormat(mime); cf && o->extra.size() < 16)
            if (HGLOBAL g = globalOf(data.data(), data.size())) o->extra.emplace_back(cf, g);
    }
    if (!txt.empty()) {
        const std::wstring w = wide(txt);
        o->text = globalOf(w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    }
    return o;
}

/// Drag cursors with no stock IDC_*: Copy = arrow + "+" badge, Link = arrow + shortcut-arrow badge (32x32 ARGB,
/// hotspot at the arrow tip), built once. Other shapes: null (the caller uses LoadCursorW).
void* win32CursorHandle(CursorShape shape) {
    if (shape != CursorShape::Copy && shape != CursorShape::Link) return nullptr;
    static HCURSOR made[2]{};
    HCURSOR& out = made[shape == CursorShape::Link ? 1 : 0];
    if (out) return out;
    constexpr int S = 32;
    // classic arrow: '1' outline, '2' fill
    static constexpr const char* kArrow[] = {
        "1", "11", "121", "1221", "12221", "122221", "1222221", "12222221", "122222221", "1222222221",
        "12222222221", "122222211111", "1221221", "121 1221", "11  1221", "1    1221", "     1221", "      11"};
    std::vector<u32> px(S * S, 0); // 0xAARRGGBB, top-down
    auto put = [&](int x, int y, u32 c) { if (x >= 0 && y >= 0 && x < S && y < S) px[std::size_t(y * S + x)] = c; };
    for (int y = 0; y < int(std::size(kArrow)); ++y)
        for (int x = 0; kArrow[y][x]; ++x)
            if (kArrow[y][x] != ' ') put(x, y, kArrow[y][x] == '1' ? 0xFF000000u : 0xFFFFFFFFu);
    // badge: an 11x11 white box with a black frame at (13, 17)
    constexpr int bx = 13, by = 17, bs = 11;
    for (int y = 0; y < bs; ++y)
        for (int x = 0; x < bs; ++x)
            put(bx + x, by + y, x == 0 || y == 0 || x == bs - 1 || y == bs - 1 ? 0xFF000000u : 0xFFFFFFFFu);
    if (shape == CursorShape::Copy) {
        for (int i = 2; i < bs - 2; ++i) { put(bx + i, by + bs / 2, 0xFF000000u); put(bx + bs / 2, by + i, 0xFF000000u); }
    } else { // shortcut arrow: a diagonal to the top-right corner with its head
        for (int i = 2; i < bs - 2; ++i) put(bx + i, by + bs - 1 - i, 0xFF000000u);
        for (int i = 0; i < 4; ++i) { put(bx + bs - 3 - i, by + 2, 0xFF000000u); put(bx + bs - 3, by + 2 + i, 0xFF000000u); }
    }
    BITMAPV5HEADER h{};
    h.bV5Size = sizeof(h);
    h.bV5Width = S;
    h.bV5Height = -S; // top-down
    h.bV5Planes = 1;
    h.bV5BitCount = 32;
    h.bV5Compression = BI_BITFIELDS;
    h.bV5RedMask = 0x00FF0000; h.bV5GreenMask = 0x0000FF00; h.bV5BlueMask = 0x000000FF; h.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&h), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    std::memcpy(bits, px.data(), px.size() * 4);
    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr); // ignored for 32-bit ARGB colour, still required
    ICONINFO ii{FALSE, 0, 0, mask, color};
    out = HCURSOR(CreateIconIndirect(&ii));
    DeleteObject(mask);
    DeleteObject(color);
    return out;
}

void* win32RegisterDrop(HWND hwnd, AppWindow* w) {
    static const bool ole = SUCCEEDED(OleInitialize(nullptr)); // once per (UI) thread; balanced at exit by the OS
    if (!ole) return nullptr;
    auto* t = new DropTarget(hwnd, w);
    if (FAILED(RegisterDragDrop(hwnd, t))) { t->Release(); return nullptr; }
    SetPropW(hwnd, L"FuseDropTarget", t); // lets tests / embedders reach the target without OLE internals
    return t;
}
void win32RevokeDrop(HWND hwnd, void* target) {
    if (!target) return;
    RevokeDragDrop(hwnd);
    RemovePropW(hwnd, L"FuseDropTarget");
    static_cast<DropTarget*>(target)->Release();
}
IDropTarget* win32DropTarget(void* target) { return static_cast<DropTarget*>(target); }

/// Modal OLE drag (returns after the drop / cancel); reports the outcome to `w`.
bool win32DoDragDrop(AppWindow& w, const AppWindow::ExternalDrag& d) {
    IDataObject* data = win32MakeDataObject(d);
    auto* src = new DropSource;
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = DoDragDrop(data, src, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
    src->Release();
    data->Release();
    const bool ok = hr == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE;
    w.deliverExternalDragEnd(ok);
    return ok;
}

} // namespace fuse::app::detail
#endif
