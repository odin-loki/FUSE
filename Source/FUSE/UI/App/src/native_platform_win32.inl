// fuse_app native platform layer, Win32 half (included by native_platform.inl): OS tool windows, client-side window
// chrome (WM_NCCALCSIZE / WM_NCHITTEST), OLE drag and drop (IDropTarget into UiContext::dropExternal, DoDragDrop for a
// UI drag that leaves the app), clipboard formats (CF_UNICODETEXT, CF_DIBV5 + "PNG", registered MIME formats) and the
// window-less dialogs (IFileDialog, ChooseColorW, MessageBoxW).
//
// NOT RUN: this file is compiled with MinGW-w64 on the development machine, where neither Windows nor Wine is
// available. Nothing in it has been executed (docs/ui/KNOWN_GAPS.md).

namespace fuse::app {
namespace {
// ---- small COM helpers ---------------------------------------------------------------------------------------------------
template <class T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
};
struct ComScope { // apartment-threaded COM for the calling (UI) thread, for the duration of a dialog
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};
HGLOBAL globalOf(const void* p, std::size_t n) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, n ? n : 1);
    if (g && n)
        if (void* dst = GlobalLock(g)) {
            std::memcpy(dst, p, n);
            GlobalUnlock(g);
        }
    return g;
}
std::string narrowN(const wchar_t* w, int len) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(std::max(0, n)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    return s;
}
bool textMime(std::string_view mime) { return mime == kClipboardText || mime == "text/plain"; }
UINT pngFormat() {
    static const UINT cf = RegisterClipboardFormatW(L"PNG"); // what browsers and image editors put next to the DIB
    return cf;
}
Vec2 screenToUi(const NativePlatform::Impl::Rec& r, POINT pt) {
    ScreenToClient(reinterpret_cast<HWND>(r.handle), &pt);
    return {f32(pt.x) / r.scale, f32(pt.y) / r.scale};
}

// ---- client-side chrome ---------------------------------------------------------------------------------------------------
constexpr const wchar_t* kChromeDownProp = L"FuseChromeDown"; // HT* code of the title-bar button under a press
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
bool isChromeButton(WPARAM ht) { return ht == HTMINBUTTON || ht == HTMAXBUTTON || ht == HTCLOSE; }

/// WS_* style of a top-level window with this frame: Custom keeps the overlapped style (snap, maximise animations),
/// Borderless is a popup.
DWORD chromeStyle(WindowFrame f) { return f == WindowFrame::Borderless ? DWORD(WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX) : DWORD(WS_OVERLAPPEDWINDOW); }
/// Client size -> window size for this window's frame (a Borderless / Custom window is all client area).
void chromeAdjust(const AppWindow& w, RECT& rc) {
    if (w.chrome().frame == WindowFrame::System) AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
}
/// The non-client messages of a Borderless / Custom window. True when handled (`result` = the window procedure's
/// return value).
bool chromeMessage(HWND hwnd, NativePlatform::Impl::Rec& r, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    AppWindow& w = *r.win;
    const WindowChrome& c = w.chrome();
    if (c.frame == WindowFrame::System) return false;
    auto uiPosOf = [&] { return screenToUi(r, POINT{short(LOWORD(lp)), short(HIWORD(lp))}); };
    switch (msg) {
    case WM_NCCALCSIZE:
        if (!wp || c.frame != WindowFrame::Custom) return false;
        if (IsZoomed(hwnd)) { // a maximised window extends past the monitor by its frame: keep the client on the monitor
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            const int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            const int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            p->rgrc[0].left += fx;
            p->rgrc[0].right -= fx;
            p->rgrc[0].top += fy;
            p->rgrc[0].bottom -= fy;
        }
        result = 0; // the whole window is client area
        return true;
    case WM_NCACTIVATE: // keep DefWindowProc from painting a classic frame over the client
        result = DefWindowProcW(hwnd, msg, wp, -1);
        return true;
    case WM_NCHITTEST:
        result = toHt(chromeHitTest(w, uiPosOf()));
        return true;
    case WM_NCMOUSEMOVE: // hover feedback for the title-bar buttons (they are non-client for the OS)
        w.ui().pointerMove(uiPosOf());
        if (!isChromeButton(wp)) return false;
        result = 0;
        return true;
    case WM_NCLBUTTONDOWN:
        if (!isChromeButton(wp)) return false; // caption / edges: DefWindowProc moves and sizes
        SetPropW(hwnd, kChromeDownProp, reinterpret_cast<HANDLE>(wp));
        result = 0;
        return true;
    case WM_NCLBUTTONUP: {
        if (!isChromeButton(wp)) return false;
        const WPARAM down = reinterpret_cast<WPARAM>(RemovePropW(hwnd, kChromeDownProp));
        if (down == wp) w.command(wp == HTMINBUTTON ? WindowCommand::Minimize : wp == HTMAXBUTTON ? WindowCommand::ToggleMaximize : WindowCommand::Close);
        result = 0;
        return true;
    }
    case WM_NCLBUTTONDBLCLK:
        if (!isChromeButton(wp)) return false; // a caption double click maximises through DefWindowProc
        result = 0;
        return true;
    default: return false;
    }
}

// ---- OS tool windows ------------------------------------------------------------------------------------------------------
LRESULT CALLBACK toolProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* r = reinterpret_cast<NativePlatform::Impl::Rec*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    // keyboard input stays with the main window: the panel is part of its UI context (one focus); a click on the tool
    // window brings the application forward instead of activating the tool window itself
    if (msg == WM_MOUSEACTIVATE) {
        if (r && r->handle && GetForegroundWindow() != reinterpret_cast<HWND>(r->handle)) SetForegroundWindow(reinterpret_cast<HWND>(r->handle));
        return MA_NOACTIVATE;
    }
    if (!r || !r->win) return DefWindowProcW(hwnd, msg, wp, lp);
    std::size_t idx = 0;
    if (!r->impl->findTool(reinterpret_cast<unsigned long long>(hwnd), idx)) return DefWindowProcW(hwnd, msg, wp, lp);
    NativePlatform::Impl::Rec::Popup& p = r->tools[idx];
    UiContext& ui = r->win->ui();
    // context coordinates from the screen position (the window may move under the pointer while its title is dragged)
    POINT pt{short(LOWORD(lp)), short(HIWORD(lp))};
    if (msg != WM_MOUSEWHEEL) ClientToScreen(hwnd, &pt); // the wheel's position is already on screen
    const Vec2 pos = screenToUi(*r, pt);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        r->impl->blitPopupOs(p);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        ui.setModifiers(u8(((wp & MK_SHIFT) ? kModShift : 0) | ((wp & MK_CONTROL) ? kModCtrl : 0) | ((GetKeyState(VK_MENU) < 0) ? kModAlt : 0)));
        break;
    default: break;
    }
    switch (msg) {
    case WM_MOUSEMOVE:
        ui.pointerMove(pos);
        r->impl->dragOutCheck(*r);
        break;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        r->impl->raiseToolOs(p);
        ui.pointerDown(pos, MouseButton::Left);
        break;
    case WM_RBUTTONDOWN: ui.pointerDown(pos, MouseButton::Right); break;
    case WM_MBUTTONDOWN: ui.pointerDown(pos, MouseButton::Middle); break;
    case WM_LBUTTONUP: ReleaseCapture(); ui.pointerUp(pos); break;
    case WM_RBUTTONUP: case WM_MBUTTONUP: ui.pointerUp(pos); break;
    case WM_MOUSEWHEEL: ui.wheel(pos, {0.f, f32(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA}); break;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
    r->win->requestRedraw();
    return 0;
}

// ---- OLE drag and drop ----------------------------------------------------------------------------------------------------
/// The drop target of one top-level window: another application's drag becomes an external drag of the window's
/// UiContext; a drag of our own that DoDragDrop carries back over the window drives the UI's own drag again.
class DropTarget final : public IDropTarget {
public:
    explicit DropTarget(NativePlatform::Impl::Rec* rec) : r(rec) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        FORMATETC t{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        type = data->QueryGetData(&f) == S_OK ? kDragFiles : data->QueryGetData(&t) == S_OK ? kDragText : 0;
        return DragOver(0, pt, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        *effect = DROPEFFECT_NONE;
        if (!r || !r->win) return S_OK;
        UiContext& ui = r->win->ui();
        const Vec2 pos = screenToUi(*r, POINT{pt.x, pt.y});
        if (ui.dragging() && r->impl->dragOut) { // our own drag came home: the UI handles it as before
            ui.pointerMove(pos);
            if (ui.dragTarget().valid()) *effect = ui.dropAction() == DropAction::Copy ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
        } else if (type) {
            DragData d;
            d.type = type;
            if (ui.dragExternalMove(pos, d)) *effect = DROPEFFECT_COPY; // the data stays the other application's
        }
        r->win->requestRedraw();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (r && r->win && !r->impl->dragOut) {
            r->win->ui().dragExternalLeave();
            r->win->requestRedraw();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        *effect = DROPEFFECT_NONE;
        if (!r || !r->win) return S_OK;
        UiContext& ui = r->win->ui();
        const Vec2 pos = screenToUi(*r, POINT{pt.x, pt.y});
        r->win->requestRedraw();
        if (ui.dragging() && r->impl->dragOut) { // dropped back inside: an ordinary UI drop
            ui.pointerMove(pos);
            ui.pointerUp(pos);
            const DropAction a = ui.lastDropAction();
            *effect = a == DropAction::Copy ? DROPEFFECT_COPY : a == DropAction::Move ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
            return S_OK;
        }
        DragData d;
        STGMEDIUM m{};
        FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        FORMATETC t{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        if (type == kDragFiles && data->GetData(&f, &m) == S_OK) {
            HDROP h = static_cast<HDROP>(GlobalLock(m.hGlobal));
            const UINT n = h ? DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0) : 0;
            for (UINT i = 0; i < n; ++i) {
                std::wstring p(DragQueryFileW(h, i, nullptr, 0), L'\0');
                DragQueryFileW(h, i, p.data(), UINT(p.size() + 1));
                std::string path = narrowN(p.c_str(), int(p.size()));
                std::replace(path.begin(), path.end(), '\\', '/');
                if (!d.text.empty()) d.text += '\n';
                d.text += path;
            }
            if (h) GlobalUnlock(m.hGlobal);
            ReleaseStgMedium(&m);
            d.type = kDragFiles;
        } else if (type == kDragText && data->GetData(&t, &m) == S_OK) {
            if (const auto* w = static_cast<const wchar_t*>(GlobalLock(m.hGlobal))) {
                d.text = narrow(w);
                std::erase(d.text, '\r');
                GlobalUnlock(m.hGlobal);
            }
            ReleaseStgMedium(&m);
            d.type = kDragText;
        }
        if (!d.type || (d.type == kDragFiles && d.text.empty())) {
            ui.dragExternalLeave();
            return S_OK;
        }
        if (ui.dropExternal(pos, d) != DropAction::None) *effect = DROPEFFECT_COPY;
        return S_OK;
    }
    NativePlatform::Impl::Rec* r; ///< cleared when the window goes away before the last reference

private:
    std::atomic<ULONG> refs{1};
    u32 type = 0; ///< kDragFiles / kDragText of the drag over the window (0: nothing we take)
};

/// Static data object of an outgoing drag: CF_HDROP for files, CF_UNICODETEXT (the text, or the paths one per line).
class DataObject final : public IDataObject {
public:
    HGLOBAL hdrop = nullptr, text = nullptr;
    ~DataObject() {
        if (hdrop) GlobalFree(hdrop);
        if (text) GlobalFree(text);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs;
        if (!n) delete this;
        return n;
    }
    HGLOBAL pick(const FORMATETC* f) const {
        if (!f || !(f->tymed & TYMED_HGLOBAL) || f->dwAspect != DVASPECT_CONTENT) return nullptr;
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
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; } // no shell drag image
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override {
        if (dir != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> fs;
        if (hdrop) fs.push_back({CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        if (text) fs.push_back({CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
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
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    std::atomic<ULONG> refs{1};
};
DataObject* makeDataObject(const DragData& d) {
    auto* o = new DataObject;
    if (d.type == kDragFiles) { // DROPFILES: a double-null-terminated list of wide paths
        std::wstring list;
        std::string_view rest = d.text;
        while (!rest.empty()) {
            const std::size_t nl = rest.find('\n');
            std::wstring w = widen(rest.substr(0, nl));
            std::replace(w.begin(), w.end(), L'/', L'\\');
            if (!w.empty()) {
                list += w;
                list += L'\0';
            }
            rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        }
        list += L'\0';
        std::vector<char> buf(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
        auto* df = reinterpret_cast<DROPFILES*>(buf.data());
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        std::memcpy(buf.data() + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
        o->hdrop = globalOf(buf.data(), buf.size());
    }
    std::string text = d.text;
    for (std::size_t i = 0; i < text.size(); ++i) // LF -> CRLF
        if (text[i] == '\n') text.insert(i++, 1, '\r');
    const std::wstring w = widen(text);
    o->text = globalOf(w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    return o;
}

// ---- clipboard formats ------------------------------------------------------------------------------------------------------
/// A packed DIB (BITMAPINFOHEADER / V4 / V5 + pixels; 24 or 32 bit, uncompressed or BI_BITFIELDS) -> image.
bool imageFromDib(const u8* p, std::size_t n, Image& out) {
    if (n < sizeof(BITMAPINFOHEADER)) return false;
    BITMAPINFOHEADER h;
    std::memcpy(&h, p, sizeof h);
    if (h.biSize < sizeof(BITMAPINFOHEADER) || h.biSize > n || h.biPlanes != 1 || (h.biBitCount != 24 && h.biBitCount != 32) || h.biWidth <= 0 ||
        h.biHeight == 0 || (h.biCompression != BI_RGB && h.biCompression != BI_BITFIELDS))
        return false;
    std::size_t off = h.biSize;
    if (h.biCompression == BI_BITFIELDS && h.biSize == sizeof(BITMAPINFOHEADER)) off += 12; // the masks follow the plain header
    const int w = h.biWidth, ht = h.biHeight < 0 ? -h.biHeight : h.biHeight;
    const std::size_t stride = (std::size_t(w) * h.biBitCount / 8 + 3) & ~std::size_t(3);
    if (off + stride * std::size_t(ht) > n) return false;
    const bool alpha = h.biBitCount == 32 && h.biSize >= sizeof(BITMAPV4HEADER); // V4 / V5 carry an alpha mask
    out.resize(w, ht);
    bool anyAlpha = false;
    for (int y = 0; y < ht; ++y) {
        const u8* row = p + off + stride * std::size_t(h.biHeight < 0 ? y : ht - 1 - y); // bottom-up unless the height is negative
        for (int x = 0; x < w; ++x) {
            const u8* q = row + std::size_t(x) * (h.biBitCount / 8);
            const u32 a = alpha ? q[3] : 255u;
            anyAlpha = anyAlpha || a != 0;
            out.pixels[std::size_t(y) * std::size_t(w) + std::size_t(x)] = (a << 24) | (u32(q[0]) << 16) | (u32(q[1]) << 8) | q[2];
        }
    }
    if (!anyAlpha) // an all-zero alpha channel means "no alpha" in practice
        for (u32& px : out.pixels) px |= 0xFF000000u;
    return true;
}
HGLOBAL dibV5Of(const Image& img) {
    BITMAPV5HEADER h{};
    h.bV5Size = sizeof h;
    h.bV5Width = img.width;
    h.bV5Height = img.height; // bottom-up
    h.bV5Planes = 1;
    h.bV5BitCount = 32;
    h.bV5Compression = BI_BITFIELDS;
    h.bV5RedMask = 0x00FF0000u;
    h.bV5GreenMask = 0x0000FF00u;
    h.bV5BlueMask = 0x000000FFu;
    h.bV5AlphaMask = 0xFF000000u;
    h.bV5CSType = 0x73524742; // 'sRGB'
    h.bV5Intent = 4;          // LCS_GM_IMAGES
    std::vector<u8> buf(sizeof h + img.pixels.size() * 4);
    std::memcpy(buf.data(), &h, sizeof h);
    u32* dst = reinterpret_cast<u32*>(buf.data() + sizeof h);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
            dst[std::size_t(img.height - 1 - y) * std::size_t(img.width) + std::size_t(x)] = toArgb(img.pixels[std::size_t(y) * std::size_t(img.width) + std::size_t(x)]);
    return globalOf(buf.data(), buf.size());
}
bool readClipboardBytes(UINT cf, std::vector<u8>& out) { // the clipboard is open
    HANDLE h = GetClipboardData(cf);
    if (!h) return false;
    const void* p = GlobalLock(h);
    if (!p) return false;
    const SIZE_T n = GlobalSize(h);
    out.assign(static_cast<const u8*>(p), static_cast<const u8*>(p) + n);
    GlobalUnlock(h);
    return true;
}
DWORD g_clipboardSequence = 0; ///< GetClipboardSequenceNumber right after this process last set the clipboard
} // namespace

bool NativePlatform::Impl::createToolOs(Rec& r, Rec::Popup& p) {
    // owned by the main window: stays above it (Qt::Tool), no task bar button, never activated (keys stay in the main one)
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kToolClass, L"", WS_POPUP, int(p.screen.x), int(p.screen.y), int(p.screen.w),
                                int(p.screen.h), reinterpret_cast<HWND>(r.handle), nullptr, hinst, nullptr);
    if (!hwnd) return false;
    p.handle = reinterpret_cast<unsigned long long>(hwnd);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&r));
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return true;
}
void NativePlatform::Impl::raiseToolOs(Rec::Popup& p) {
    if (p.handle) SetWindowPos(reinterpret_cast<HWND>(p.handle), HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
bool NativePlatform::Impl::postInputOs(unsigned long long h, Vec2 at, int kind) {
    HWND hwnd = reinterpret_cast<HWND>(h);
    const LPARAM lp = MAKELPARAM(int(at.x), int(at.y));
    switch (kind) {
    case 0: return PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp) != 0;
    case 1: return PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp) != 0;
    case 2: return PostMessageW(hwnd, WM_LBUTTONUP, 0, lp) != 0;
    default: return PostMessageW(hwnd, WM_CLOSE, 0, 0) != 0;
    }
}
void NativePlatform::Impl::registerDrop(Rec& r) {
    if (!opts.dragAndDrop || !oleReady || !r.handle || r.dropTarget) return;
    auto* t = new DropTarget(&r);
    if (RegisterDragDrop(reinterpret_cast<HWND>(r.handle), t) == S_OK) r.dropTarget = t; // OLE holds its own reference too
    else t->Release();
}
void NativePlatform::Impl::revokeDrop(Rec& r) {
    if (!r.dropTarget) return;
    auto* t = static_cast<DropTarget*>(r.dropTarget);
    RevokeDragDrop(reinterpret_cast<HWND>(r.handle));
    t->r = nullptr;
    t->Release();
    r.dropTarget = nullptr;
}
/// After a pointer move: a UI drag with text / files that left every window of the app goes on as an OLE drag
/// (DoDragDrop runs its own modal loop until the button is released).
void NativePlatform::Impl::dragOutCheck(Rec& r) {
    if (!opts.dragAndDrop || !oleReady || dragOut) return;
    UiContext& ui = r.win->ui();
    const DragData* d = ui.dragData();
    if (!ui.dragging() || !d || !ui.alive(d->source) || (d->type != kDragText && d->type != kDragFiles)) return;
    POINT pt;
    if (!GetCursorPos(&pt)) return;
    const HWND under = WindowFromPoint(pt);
    for (const Rec* q : recs) { // still over one of the app's windows: the UI handles it
        if (under == reinterpret_cast<HWND>(q->handle)) return;
        for (const Rec::Popup& p : q->tools)
            if (under == reinterpret_cast<HWND>(p.handle)) return;
        for (const Rec::Popup& p : q->popups)
            if (under == reinterpret_cast<HWND>(p.handle)) return;
    }
    DataObject* data = makeDataObject(*d);
    auto* source = new DropSource;
    dragOut = true;
    ReleaseCapture();
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = DoDragDrop(data, source, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
    dragOut = false;
    data->Release();
    source->Release();
    if (ui.dragging()) // not ended by a drop back inside the app (DropTarget::Drop)
        ui.dragFinishExternal(hr == DRAGDROP_S_DROP ? ((effect & DROPEFFECT_MOVE) ? DropAction::Move : (effect & DROPEFFECT_COPY) ? DropAction::Copy : DropAction::None)
                                                    : DropAction::None);
    r.win->requestRedraw();
}
bool NativePlatform::externalDragActive() const { return m->dragOut; }
int NativePlatform::cursorThemeSize() const { return 0; } // X11 only

bool NativePlatform::setClipboard(std::vector<ClipboardItem> items) {
    for (ClipboardItem& it : items) // delayed rendering is not implemented: lazy items are made now
        if (it.provide) {
            const auto fn = it.provide;
            it.provide = nullptr;
            if (!fn(it.user, it)) it.mime.clear();
        }
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    bool ok = true;
    auto put = [&](UINT cf, HGLOBAL g) {
        if (!g || !SetClipboardData(cf, g)) { // on success the system owns the block
            if (g) GlobalFree(g);
            ok = false;
        }
    };
    for (ClipboardItem& it : items) {
        if (it.mime.empty()) continue;
        if (textMime(it.mime)) {
            std::string crlf = it.text;
            for (std::size_t i = 0; i < crlf.size(); ++i)
                if (crlf[i] == '\n' && (i == 0 || crlf[i - 1] != '\r')) crlf.insert(i++, 1, '\r');
            const std::wstring w = widen(crlf);
            put(CF_UNICODETEXT, globalOf(w.c_str(), (w.size() + 1) * sizeof(wchar_t)));
        } else if (it.mime == kClipboardImage) {
            if (it.bytes.empty() && !encodePng(it.image, it.bytes)) { ok = false; continue; }
            if (it.image.width <= 0 && !decodePng(it.bytes, it.image)) { ok = false; continue; } // PNG bytes given: the DIB needs pixels
            put(pngFormat(), globalOf(it.bytes.data(), it.bytes.size()));
            put(CF_DIBV5, dibV5Of(it.image)); // Windows synthesises CF_DIB / CF_BITMAP from it
        } else if (const UINT cf = RegisterClipboardFormatW(widen(it.mime).c_str())) {
            put(cf, globalOf(it.bytes.data(), it.bytes.size()));
        }
    }
    CloseClipboard();
    g_clipboardSequence = GetClipboardSequenceNumber();
    return ok;
}
bool NativePlatform::clipboardData(std::string_view mime, std::vector<u8>& out) {
    out.clear();
    if (mime.empty()) return false;
    if (textMime(mime)) {
        std::string text;
        if (!clipboardText(text)) return false;
        out.assign(text.begin(), text.end());
        return true;
    }
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (mime == kClipboardImage) {
        ok = readClipboardBytes(pngFormat(), out);
        if (!ok) { // no PNG on the clipboard: encode the DIB
            std::vector<u8> dib;
            Image img;
            ok = (readClipboardBytes(CF_DIBV5, dib) || readClipboardBytes(CF_DIB, dib)) && imageFromDib(dib.data(), dib.size(), img) &&
                 encodePng(img, out);
        }
    } else if (const UINT cf = RegisterClipboardFormatW(widen(mime).c_str())) {
        ok = readClipboardBytes(cf, out);
    }
    CloseClipboard();
    if (!ok) out.clear();
    return ok;
}
std::vector<std::string> NativePlatform::clipboardFormats() {
    std::vector<std::string> out;
    if (!OpenClipboard(nullptr)) return out;
    auto add = [&](std::string name) {
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
    };
    for (UINT cf = EnumClipboardFormats(0); cf; cf = EnumClipboardFormats(cf)) {
        if (cf == CF_UNICODETEXT || cf == CF_TEXT) add(std::string(kClipboardText));
        else if (cf == CF_DIBV5 || cf == CF_DIB || cf == pngFormat()) add(std::string(kClipboardImage));
        else if (cf >= 0xC000) { // a registered format: MIME-named ones are ours / other toolkits'
            wchar_t name[256];
            const int n = GetClipboardFormatNameW(cf, name, 256);
            const std::string s = n > 0 ? narrowN(name, n) : std::string();
            if (s.find('/') != std::string::npos && s.find(' ') == std::string::npos) add(s);
        }
    }
    CloseClipboard();
    return out;
}
bool NativePlatform::clipboardOwned() const { return g_clipboardSequence != 0 && GetClipboardSequenceNumber() == g_clipboardSequence; }
bool NativePlatform::setPrimarySelectionText(std::string_view) { return false; } // X11 only
bool NativePlatform::primarySelectionText(std::string&) { return false; }

// ---- window-less dialogs ----------------------------------------------------------------------------------------------------
namespace {
std::expected<std::vector<std::string>, PlatformError> runFileDialog(const FileDialogDesc& d, DialogKind kind) {
    const bool save = kind == DialogKind::Save, folder = kind == DialogKind::Folder;
    ComScope com;
    ComPtr<IFileDialog> dlg;
    HRESULT hr = save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog, reinterpret_cast<void**>(dlg.put()))
                      : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void**>(dlg.put()));
    if (FAILED(hr) || !dlg.p) return std::unexpected(PlatformError::Unavailable);
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    if (folder) opts |= FOS_PICKFOLDERS;
    if (d.multiple && kind == DialogKind::Open) opts |= FOS_ALLOWMULTISELECT;
    dlg->SetOptions(opts);
    if (!d.title.empty()) dlg->SetTitle(widen(d.title).c_str());
    std::vector<std::wstring> names, specs;
    std::vector<COMDLG_FILTERSPEC> filters;
    if (!folder && !d.filters.empty()) {
        for (const FileFilter& f : d.filters) {
            names.push_back(widen(f.name.empty() ? f.patterns : f.name));
            specs.push_back(widen(f.patterns));
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
        std::wstring dir = widen(d.folder);
        std::replace(dir.begin(), dir.end(), L'/', L'\\');
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(item.put())))) dlg->SetFolder(item.p);
    }
    if (!d.fileName.empty()) dlg->SetFileName(widen(d.fileName).c_str());
    hr = dlg->Show(reinterpret_cast<HWND>(d.owner));
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::unexpected(PlatformError::Cancelled);
    if (FAILED(hr)) return std::unexpected(PlatformError::Failed);
    std::vector<std::string> paths;
    auto take = [&](IShellItem* it) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
            paths.push_back(narrow(p));
            CoTaskMemFree(p);
        }
    };
    if (kind == DialogKind::Open && d.multiple) {
        ComPtr<IShellItemArray> items;
        if (SUCCEEDED(static_cast<IFileOpenDialog*>(dlg.p)->GetResults(items.put())) && items.p) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                ComPtr<IShellItem> it;
                if (SUCCEEDED(items->GetItemAt(i, it.put())) && it.p) take(it.p);
            }
        }
    } else {
        ComPtr<IShellItem> it;
        if (SUCCEEDED(dlg->GetResult(it.put())) && it.p) take(it.p);
    }
    if (paths.empty()) return std::unexpected(PlatformError::Failed);
    return paths;
}
UINT_PTR CALLBACK colorHook(HWND dlg, UINT msg, WPARAM, LPARAM lp) {
    if (msg == WM_INITDIALOG) { // ChooseColor has no title field: the hook names the dialog
        const auto* cc = reinterpret_cast<const CHOOSECOLORW*>(lp);
        const auto* title = reinterpret_cast<const std::wstring*>(cc->lCustData);
        if (title && !title->empty()) SetWindowTextW(dlg, title->c_str());
    }
    return 0; // default processing
}
} // namespace

std::expected<std::vector<std::string>, PlatformError> openFileDialog(const FileDialogDesc& d) { return runFileDialog(d, DialogKind::Open); }
std::expected<std::string, PlatformError> saveFileDialog(const FileDialogDesc& d) {
    auto r = runFileDialog(d, DialogKind::Save);
    if (!r) return std::unexpected(r.error());
    return std::move(r->front());
}
std::expected<std::string, PlatformError> pickFolderDialog(const FileDialogDesc& d) {
    auto r = runFileDialog(d, DialogKind::Folder);
    if (!r) return std::unexpected(r.error());
    return std::move(r->front());
}
std::expected<Color, PlatformError> colorDialog(const ColorDialogDesc& d) {
    static COLORREF custom[16] = {}; // the dialog's custom colours, kept for the session
    auto byte = [](f32 v) { return BYTE(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
    std::wstring title = widen(d.title);
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = reinterpret_cast<HWND>(d.owner);
    cc.rgbResult = RGB(byte(d.initial.r), byte(d.initial.g), byte(d.initial.b));
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR | CC_ENABLEHOOK;
    cc.lpfnHook = &colorHook;
    cc.lCustData = reinterpret_cast<LPARAM>(&title);
    if (!ChooseColorW(&cc)) return std::unexpected(CommDlgExtendedError() ? PlatformError::Failed : PlatformError::Cancelled);
    return Color{f32(GetRValue(cc.rgbResult)) / 255.f, f32(GetGValue(cc.rgbResult)) / 255.f, f32(GetBValue(cc.rgbResult)) / 255.f, d.initial.a};
}
std::expected<MessageResult, PlatformError> messageBox(std::string_view title, std::string_view text, MessageIcon icon, MessageButtons buttons,
                                                       unsigned long long owner) {
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
    switch (MessageBoxW(reinterpret_cast<HWND>(owner), widen(text).c_str(), widen(title).c_str(), flags)) {
    case IDOK: return MessageResult::Ok;
    case IDYES: return MessageResult::Yes;
    case IDNO: return MessageResult::No;
    case IDCANCEL: return MessageResult::Cancel;
    case 0: return std::unexpected(PlatformError::Failed);
    default: return MessageResult::Cancel;
    }
}
std::expected<void, PlatformError> revealInFileManager(std::string_view path) {
    if (path.empty()) return std::unexpected(PlatformError::InvalidArgument);
    std::wstring w = widen(path);
    std::replace(w.begin(), w.end(), L'/', L'\\');
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
