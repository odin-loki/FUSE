// Win32 OLE drag and drop (G3/G4 Windows item): IDropTarget per top-level window -> AppWindow::deliverExternalDrop
// (CF_HDROP files, CF_UNICODETEXT text), and an IDataObject + IDropSource for AppWindow::beginExternalDrag
// (DoDragDrop, CF_HDROP + CF_UNICODETEXT of the uri-list) -> AppWindow::deliverExternalDragEnd.
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
        ok = win && win->acceptsExternalDrops() && (data->QueryGetData(&f) == S_OK || data->QueryGetData(&t) == S_OK);
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
            d.mime = "text/plain;charset=utf-8";
        }
        ok = false;
        win->deliverExternalDrop(d);
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }
    HWND hwnd;
    AppWindow* win;

private:
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
    ~DataObject() {
        if (hdrop) GlobalFree(hdrop);
        if (text) GlobalFree(text);
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
        return f->cfFormat == CF_HDROP ? hdrop : f->cfFormat == CF_UNICODETEXT ? text : nullptr;
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
        FORMATETC fs[2];
        UINT n = 0;
        if (hdrop) fs[n++] = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        if (text) fs[n++] = {CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return SHCreateStdEnumFmtEtc(n, fs, out);
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
    const std::string txt = !d.paths.empty() ? AppWindow::uriListFromPaths(d.paths) : d.data;
    if (!txt.empty()) {
        const std::wstring w = wide(txt);
        o->text = globalOf(w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    }
    return o;
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
