// fuse_app system clipboard (clipboard.hpp): Win32 clipboard with delayed rendering, X11 CLIPBOARD selection on a
// private connection served from the NativePlatform pump, else a process-local store.
#include "fuse/app/clipboard.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include "platform_internal.hpp"
#include "x11_internal.hpp"

#include <chrono>
#include <cstdlib>
#include <poll.h>

// PNG for image/png (X11 only): static encoder + decoder in this translation unit
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#endif
#include "stb_image.h"
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

namespace fuse::app {
using fuse::ui::Image;
using fuse::ui::u32;
using fuse::ui::u8;

namespace {
bool isText(std::string_view m) { return m == kClipboardText; }
bool isImage(std::string_view m) { return m == kClipboardImage; }
/// Runs a lazy item's provider once.
bool provideOnce(ClipboardItem& it) {
    if (!it.provide) return true;
    auto fn = std::move(it.provide);
    it.provide = nullptr;
    return fn(it);
}
ClipboardItem* findItem(std::vector<ClipboardItem>& items, std::string_view mime) {
    for (ClipboardItem& it : items)
        if (it.mime == mime) return &it;
    return nullptr;
}
} // namespace

#if defined(_WIN32)
// ---- Win32 -----------------------------------------------------------------------------------------------------------
namespace {
struct WinClip {
    HWND hwnd = nullptr;
    std::vector<ClipboardItem> items; ///< what we put there (lazy items rendered on WM_RENDERFORMAT)
};
WinClip& wc() {
    static WinClip c;
    return c;
}
std::wstring widen(std::string_view s) {
    std::wstring w(std::size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}
std::string narrow(const wchar_t* s, std::size_t n) {
    std::string o(std::size_t(WideCharToMultiByte(CP_UTF8, 0, s, int(n), nullptr, 0, nullptr, nullptr)), '\0');
    if (!o.empty()) WideCharToMultiByte(CP_UTF8, 0, s, int(n), o.data(), int(o.size()), nullptr, nullptr);
    return o;
}
UINT formatOf(std::string_view mime) {
    if (isText(mime)) return CF_UNICODETEXT;
    if (isImage(mime)) return CF_DIBV5;
    return RegisterClipboardFormatW(widen(mime).c_str());
}
HGLOBAL globalOf(const void* data, std::size_t n) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, std::max<std::size_t>(n, 1));
    if (!h) return nullptr;
    if (void* p = GlobalLock(h)) {
        if (n) std::memcpy(p, data, n);
        GlobalUnlock(h);
    }
    return h;
}
/// The item's data in its OS format (CF_UNICODETEXT, CF_DIBV5 bottom-up 32-bit with alpha, raw bytes).
HGLOBAL render(ClipboardItem& it) {
    if (!provideOnce(it)) return nullptr;
    if (isText(it.mime)) {
        std::wstring w = widen(it.text);
        return globalOf(w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    }
    if (isImage(it.mime)) {
        const Image& img = it.image;
        if (img.width <= 0 || img.height <= 0) return nullptr;
        std::vector<std::uint8_t> b(sizeof(BITMAPV5HEADER) + img.pixels.size() * 4);
        BITMAPV5HEADER h{};
        h.bV5Size = sizeof h;
        h.bV5Width = img.width;
        h.bV5Height = img.height; // bottom-up: what most readers expect
        h.bV5Planes = 1;
        h.bV5BitCount = 32;
        h.bV5Compression = BI_BITFIELDS;
        h.bV5SizeImage = DWORD(img.pixels.size() * 4);
        h.bV5RedMask = 0x00FF0000u;
        h.bV5GreenMask = 0x0000FF00u;
        h.bV5BlueMask = 0x000000FFu;
        h.bV5AlphaMask = 0xFF000000u;
        h.bV5CSType = LCS_sRGB;
        h.bV5Intent = LCS_GM_IMAGES;
        std::memcpy(b.data(), &h, sizeof h);
        u32* px = reinterpret_cast<u32*>(b.data() + sizeof h);
        for (int y = 0; y < img.height; ++y)
            for (int x = 0; x < img.width; ++x) { // 0xAABBGGRR -> 0xAARRGGBB
                const u32 p = img.at(x, img.height - 1 - y);
                px[std::size_t(y) * std::size_t(img.width) + std::size_t(x)] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
            }
        return globalOf(b.data(), b.size());
    }
    return globalOf(it.bytes.data(), it.bytes.size());
}
LRESULT CALLBACK clipProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WinClip& c = wc();
    switch (msg) {
    case WM_RENDERFORMAT: // a paste asked for a delayed format: the clipboard is open (by the reader)
        for (ClipboardItem& it : c.items)
            if (formatOf(it.mime) == UINT(wp)) {
                if (HGLOBAL h = render(it); h && !SetClipboardData(UINT(wp), h)) GlobalFree(h);
                break;
            }
        return 0;
    case WM_RENDERALLFORMATS: // the owner window goes away: leave every format behind
        if (OpenClipboard(hwnd)) {
            if (GetClipboardOwner() == hwnd)
                for (ClipboardItem& it : c.items)
                    if (it.provide)
                        if (HGLOBAL h = render(it); h && !SetClipboardData(formatOf(it.mime), h)) GlobalFree(h);
            CloseClipboard();
        }
        return 0;
    case WM_DESTROYCLIPBOARD: c.items.clear(); return 0; // replaced (EmptyClipboard by anyone)
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
HWND owner() {
    WinClip& c = wc();
    if (!c.hwnd) {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof cls;
        cls.lpfnWndProc = &clipProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"FuseAppClipboard";
        RegisterClassExW(&cls);
        c.hwnd = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    }
    return c.hwnd;
}
/// OpenClipboard, retried briefly: another process may hold it for a moment.
bool openClip(HWND h) {
    for (int i = 0; i < 50; ++i) {
        if (OpenClipboard(h)) return true;
        Sleep(2);
    }
    return false;
}
struct ClipLock {
    bool ok;
    explicit ClipLock(HWND h) : ok(openClip(h)) {}
    ~ClipLock() { if (ok) CloseClipboard(); }
};
bool readDib(HGLOBAL g, Image& out) {
    const SIZE_T size = GlobalSize(g);
    const auto* p = static_cast<const std::uint8_t*>(GlobalLock(g));
    if (!p) return false;
    bool ok = false;
    BITMAPINFOHEADER h{};
    if (size >= sizeof h) std::memcpy(&h, p, sizeof h);
    const int w = h.biWidth, hh = h.biHeight < 0 ? -h.biHeight : h.biHeight;
    const bool topDown = h.biHeight < 0;
    if (size >= sizeof h && w > 0 && hh > 0 && w <= 16384 && hh <= 16384 && (h.biBitCount == 32 || h.biBitCount == 24) &&
        (h.biCompression == BI_RGB || h.biCompression == BI_BITFIELDS)) {
        DWORD masks[4] = {0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0}; // R G B A
        std::size_t off = h.biSize;
        if (h.biCompression == BI_BITFIELDS) {
            if (h.biSize >= 56) std::memcpy(masks, p + 40, 16);                        // V4 / V5: masks in the header
            else if (size >= 52) { std::memcpy(masks, p + 40, 12); off += 12; }         // after a 40-byte header
        }
        off += std::size_t(h.biClrUsed) * 4;
        const std::size_t stride = (std::size_t(w) * h.biBitCount / 8 + 3) & ~std::size_t(3);
        if (off + stride * std::size_t(hh) <= size) {
            auto chan = [](DWORD v, DWORD mask) -> u32 {
                if (!mask) return 0;
                int sh = 0;
                while (!((mask >> sh) & 1u)) ++sh;
                return ((v & mask) >> sh) & 0xFFu;
            };
            out.resize(w, hh);
            bool anyAlpha = false;
            for (int y = 0; y < hh; ++y) {
                const std::uint8_t* row = p + off + stride * std::size_t(topDown ? y : hh - 1 - y);
                for (int x = 0; x < w; ++x) {
                    u32 r, gg, b, a = 255;
                    if (h.biBitCount == 24) {
                        b = row[x * 3]; gg = row[x * 3 + 1]; r = row[x * 3 + 2];
                    } else {
                        DWORD v;
                        std::memcpy(&v, row + x * 4, 4);
                        r = chan(v, masks[0]); gg = chan(v, masks[1]); b = chan(v, masks[2]);
                        a = masks[3] ? chan(v, masks[3]) : (v >> 24) & 0xFFu; // BI_RGB: the spare byte, often 0
                        anyAlpha |= a != 0;
                    }
                    out.pixels[std::size_t(y) * std::size_t(w) + std::size_t(x)] = (a << 24) | (b << 16) | (gg << 8) | r;
                }
            }
            if (h.biBitCount == 32 && !anyAlpha) // no alpha channel at all: opaque
                for (u32& q : out.pixels) q |= 0xFF000000u;
            ok = true;
        }
    }
    GlobalUnlock(g);
    return ok;
}
} // namespace

bool setClipboard(std::vector<ClipboardItem> items) {
    HWND h = owner();
    if (!h) return false;
    ClipLock lock(h);
    if (!lock.ok || !EmptyClipboard()) return false; // WM_DESTROYCLIPBOARD clears our old items
    WinClip& c = wc();
    c.items = std::move(items);
    bool ok = true;
    for (ClipboardItem& it : c.items) {
        const UINT f = formatOf(it.mime);
        if (!f) { ok = false; continue; }
        if (it.provide) { SetClipboardData(f, nullptr); continue; } // delayed rendering
        HGLOBAL g = render(it);
        if (!g || !SetClipboardData(f, g)) {
            if (g) GlobalFree(g);
            ok = false;
        }
    }
    return ok;
}
bool clipboardText(std::string& out) {
    out.clear();
    ClipLock lock(owner());
    if (!lock.ok) return false;
    HGLOBAL g = GetClipboardData(CF_UNICODETEXT);
    const auto* w = g ? static_cast<const wchar_t*>(GlobalLock(g)) : nullptr;
    if (!w) return false;
    const std::size_t cap = GlobalSize(g) / sizeof(wchar_t);
    std::size_t n = 0;
    while (n < cap && w[n]) ++n;
    out = narrow(w, n);
    GlobalUnlock(g);
    return true;
}
bool clipboardImage(Image& out) {
    ClipLock lock(owner());
    if (!lock.ok) return false;
    for (UINT f : {UINT(CF_DIBV5), UINT(CF_DIB)})
        if (IsClipboardFormatAvailable(f))
            if (HGLOBAL g = GetClipboardData(f); g && readDib(g, out)) return true;
    return false;
}
bool clipboardData(std::string_view mime, std::vector<std::uint8_t>& out) {
    out.clear();
    if (isText(mime)) {
        std::string s;
        if (!clipboardText(s)) return false;
        out.assign(s.begin(), s.end());
        return true;
    }
    if (isImage(mime)) return false;
    const UINT f = formatOf(mime);
    ClipLock lock(owner());
    if (!lock.ok || !f) return false;
    HGLOBAL g = GetClipboardData(f);
    const auto* p = g ? static_cast<const std::uint8_t*>(GlobalLock(g)) : nullptr;
    if (!p) return false;
    out.assign(p, p + GlobalSize(g));
    GlobalUnlock(g);
    return true;
}
std::vector<std::string> clipboardFormats() {
    std::vector<std::string> out;
    ClipLock lock(owner());
    if (!lock.ok) return out;
    auto add = [&](std::string s) { if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s)); };
    for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f)) {
        if (f == CF_UNICODETEXT || f == CF_TEXT || f == CF_OEMTEXT) add(std::string(kClipboardText));
        else if (f == CF_DIB || f == CF_DIBV5 || f == CF_BITMAP) add(std::string(kClipboardImage));
        else if (f >= 0xC000) {
            wchar_t name[256];
            const int n = GetClipboardFormatNameW(f, name, 255);
            if (n > 0) {
                std::string s = narrow(name, std::size_t(n));
                if (s.find('/') != std::string::npos) add(std::move(s)); // MIME-like names only
            }
        }
    }
    return out;
}
bool clipboardOwned() { return wc().hwnd && GetClipboardOwner() == wc().hwnd; }

#else
// ---- X11 CLIPBOARD selection (+ process-local fallback) -------------------------------------------------------------
namespace {
struct XClip {
    x11::Lib x;
    x11::Display* d = nullptr;
    x11::Window w = 0;
    bool tried = false;
    x11::Atom clipboard = 0, targets = 0, utf8 = 0, text = 0, textPlain = 0, textPlainUtf8 = 0, png = 0, incr = 0,
              timestamp = 0, prop = 0;
    std::vector<ClipboardItem> items;
    bool owned = false;
    std::size_t chunk = 65536;
    struct Send { x11::Window req = 0; x11::Atom prop = 0, type = 0; std::vector<std::uint8_t> data; std::size_t at = 0; };
    std::vector<Send> sends; ///< INCR transfers in progress

    bool init();
    void drain();
    void handle(x11::Event& ev);
    void serve(const x11::SelectionRequestEv& rq);
    bool bytesFor(x11::Atom target, std::vector<std::uint8_t>& out, x11::Atom& type);
    bool waitEvent(x11::Event& ev, bool (*match)(const x11::Event&, const XClip&), int ms);
    bool convert(x11::Atom target, std::vector<std::uint8_t>& out, x11::Atom* typeOut = nullptr);
    bool ownsNow() { return owned && d && x.GetSelectionOwner(d, clipboard) == w; }
};
XClip& xc() {
    static XClip c;
    return c;
}
void pumpWatch(void* user) { static_cast<XClip*>(user)->drain(); }

bool XClip::init() {
    if (tried) return d != nullptr;
    tried = true;
    const char* disp = std::getenv("DISPLAY");
    if (!disp || !*disp || !x.load()) return false;
    d = x.OpenDisplay(nullptr);
    if (!d) return false;
    const x11::Window root = x.RootWindow(d, x.DefaultScreen(d));
    w = x.CreateSimpleWindow(d, root, 0, 0, 1, 1, 0, 0, 0); // never mapped: owns the selection, receives transfers
    x.SelectInput(d, w, x11::kPropertyChangeMask);
    auto atom = [&](const char* n) { return x.InternAtom(d, n, 0); };
    clipboard = atom("CLIPBOARD");
    targets = atom("TARGETS");
    utf8 = atom("UTF8_STRING");
    text = atom("TEXT");
    textPlain = atom("text/plain");
    textPlainUtf8 = atom("text/plain;charset=utf-8");
    png = atom("image/png");
    incr = atom("INCR");
    timestamp = atom("TIMESTAMP");
    prop = atom("FUSE_CLIPBOARD");
    const long maxReq = x.MaxRequestSize(d); // 4-byte units
    chunk = std::size_t(std::clamp(maxReq * 4 - 256, 4096L, 262144L));
    x.Flush(d);
    detail::addPumpWatch(x.ConnectionNumber(d), &pumpWatch, this);
    return true;
}
void XClip::drain() {
    while (d && x.Pending(d)) {
        x11::Event ev{};
        x.NextEvent(d, &ev);
        handle(ev);
    }
}
void XClip::handle(x11::Event& ev) {
    switch (ev.type) {
    case x11::SelectionRequest: serve(ev.selectionRequest); break;
    case x11::SelectionClear:
        if (ev.selectionClear.selection == clipboard) { owned = false; items.clear(); }
        break;
    case x11::PropertyNotify: // INCR: the requestor deleted the last chunk, send the next one
        if (ev.property.state != x11::kPropertyDelete) break;
        for (std::size_t i = 0; i < sends.size(); ++i) {
            Send& s = sends[i];
            if (s.req != ev.property.window || s.prop != ev.property.atom) continue;
            const std::size_t n = std::min(chunk, s.data.size() - s.at);
            x.ChangeProperty(d, s.req, s.prop, s.type, 8, x11::kPropModeReplace, s.data.data() + s.at, int(n));
            s.at += n;
            if (n == 0) { // the zero-length chunk ends the transfer
                x.SelectInput(d, s.req, 0);
                sends.erase(sends.begin() + std::ptrdiff_t(i));
            }
            x.Flush(d);
            break;
        }
        break;
    default: break;
    }
}
bool XClip::bytesFor(x11::Atom target, std::vector<std::uint8_t>& out, x11::Atom& type) {
    type = target;
    if (target == utf8 || target == textPlainUtf8 || target == text || target == textPlain || target == x11::kXA_STRING) {
        ClipboardItem* it = findItem(items, kClipboardText);
        if (!it || !provideOnce(*it)) return false;
        if (target == text) type = utf8;
        if (target == x11::kXA_STRING) { // Latin-1
            out.clear();
            const std::string& s = it->text;
            for (std::size_t i = 0; i < s.size();) {
                const u8 c = u8(s[i]);
                u32 cp = c;
                int len = 1;
                if (c >= 0xF0) { len = 4; cp = c & 7u; } else if (c >= 0xE0) { len = 3; cp = c & 15u; } else if (c >= 0xC0) { len = 2; cp = c & 31u; }
                for (int k = 1; k < len && i + std::size_t(k) < s.size(); ++k) cp = (cp << 6) | (u8(s[i + std::size_t(k)]) & 63u);
                out.push_back(cp < 256 ? std::uint8_t(cp) : std::uint8_t('?'));
                i += std::size_t(len);
            }
        } else {
            out.assign(it->text.begin(), it->text.end());
        }
        return true;
    }
    if (target == png) {
        ClipboardItem* it = findItem(items, kClipboardImage);
        if (!it || !provideOnce(*it) || it->image.width <= 0) return false;
        out.clear();
        // 0xAABBGGRR words are R, G, B, A bytes in memory
        const int ok = stbi_write_png_to_func([](void* ctx, void* data, int size) {
            auto* v = static_cast<std::vector<std::uint8_t>*>(ctx);
            v->insert(v->end(), static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
        }, &out, it->image.width, it->image.height, 4, it->image.pixels.data(), it->image.width * 4);
        return ok != 0;
    }
    char* name = x.GetAtomName(d, target);
    if (!name) return false;
    ClipboardItem* it = findItem(items, name);
    x.Free(name);
    if (!it || isText(it->mime) || isImage(it->mime) || !provideOnce(*it)) return false;
    out = it->bytes;
    return true;
}
void XClip::serve(const x11::SelectionRequestEv& rq) {
    x11::Event reply{};
    reply.selection.type = x11::SelectionNotify;
    reply.selection.display = d;
    reply.selection.requestor = rq.requestor;
    reply.selection.selection = rq.selection;
    reply.selection.target = rq.target;
    reply.selection.time = rq.time;
    const x11::Atom property = rq.property ? rq.property : rq.target; // obsolete clients pass None
    bool ok = false;
    if (rq.selection == clipboard && owned) {
        if (rq.target == targets) {
            std::vector<long> list = {long(targets), long(timestamp)};
            for (const ClipboardItem& it : items) {
                if (isText(it.mime)) for (x11::Atom a : {utf8, textPlainUtf8, x11::Atom(x11::kXA_STRING), text}) list.push_back(long(a));
                else if (isImage(it.mime)) list.push_back(long(png));
                else list.push_back(long(x.InternAtom(d, it.mime.c_str(), 0)));
            }
            x.ChangeProperty(d, rq.requestor, property, x11::kXA_ATOM, 32, x11::kPropModeReplace,
                             reinterpret_cast<const unsigned char*>(list.data()), int(list.size()));
            ok = true;
        } else if (rq.target == timestamp) {
            const long t = 0;
            x.ChangeProperty(d, rq.requestor, property, 19 /*XA_INTEGER*/, 32, x11::kPropModeReplace,
                             reinterpret_cast<const unsigned char*>(&t), 1);
            ok = true;
        } else {
            std::vector<std::uint8_t> data;
            x11::Atom type = 0;
            if (bytesFor(rq.target, data, type)) {
                if (data.size() > chunk) { // INCR: announce the size, then a chunk per property deletion
                    const long size = long(data.size());
                    x.SelectInput(d, rq.requestor, x11::kPropertyChangeMask);
                    x.ChangeProperty(d, rq.requestor, property, incr, 32, x11::kPropModeReplace,
                                     reinterpret_cast<const unsigned char*>(&size), 1);
                    sends.push_back({rq.requestor, property, type, std::move(data), 0});
                } else {
                    x.ChangeProperty(d, rq.requestor, property, type, 8, x11::kPropModeReplace, data.data(), int(data.size()));
                }
                ok = true;
            }
        }
    }
    reply.selection.property = ok ? property : 0;
    x.SendEvent(d, rq.requestor, 0, 0, &reply);
    x.Flush(d);
}
bool XClip::waitEvent(x11::Event& ev, bool (*match)(const x11::Event&, const XClip&), int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        while (x.Pending(d)) {
            x.NextEvent(d, &ev);
            if (match(ev, *this)) return true;
            handle(ev);
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        pollfd p{x.ConnectionNumber(d), POLLIN, 0};
        ::poll(&p, 1, int(std::min<long long>(left, 50)));
    }
}
bool XClip::convert(x11::Atom target, std::vector<std::uint8_t>& out, x11::Atom* typeOut) {
    out.clear();
    x.DeleteProperty(d, w, prop);
    x.ConvertSelection(d, clipboard, target, prop, w, 0);
    x.Flush(d);
    x11::Event ev{};
    if (!waitEvent(ev, [](const x11::Event& e, const XClip& c) {
            return e.type == x11::SelectionNotify && e.selection.requestor == c.w; }, 2000))
        return false;
    if (!ev.selection.property) return false;
    auto take = [&](std::vector<std::uint8_t>& chunkOut, x11::Atom& type, int& format) {
        unsigned long n = 0, after = 0;
        unsigned char* p = nullptr;
        if (x.GetWindowProperty(d, w, prop, 0, 1L << 26, 1 /*delete*/, 0 /*AnyPropertyType*/, &type, &format, &n, &after, &p) != 0)
            return false;
        const std::size_t item = format == 32 ? sizeof(long) : format == 16 ? sizeof(short) : 1;
        chunkOut.assign(p, p + (p ? n * item : 0));
        if (p) x.Free(p);
        return true;
    };
    x11::Atom type = 0;
    int format = 0;
    if (!take(out, type, format)) return false;
    if (type == incr) { // INCR: chunks arrive as new values of the property until an empty one
        out.clear();
        x.Flush(d);
        for (;;) {
            if (!waitEvent(ev, [](const x11::Event& e, const XClip& c) {
                    return e.type == x11::PropertyNotify && e.property.window == c.w && e.property.atom == c.prop &&
                           e.property.state == x11::kPropertyNewValue; }, 2000))
                return false;
            std::vector<std::uint8_t> part;
            if (!take(part, type, format)) return false;
            x.Flush(d);
            if (part.empty()) break;
            out.insert(out.end(), part.begin(), part.end());
        }
    }
    if (typeOut) *typeOut = type;
    return true;
}
std::vector<ClipboardItem> g_local; ///< no display: process-local store
} // namespace

bool setClipboard(std::vector<ClipboardItem> items) {
    XClip& c = xc();
    if (!c.init()) {
        g_local = std::move(items);
        return true;
    }
    c.items = std::move(items);
    c.x.SetSelectionOwner(c.d, c.clipboard, c.w, 0);
    c.owned = c.x.GetSelectionOwner(c.d, c.clipboard) == c.w;
    c.x.Flush(c.d);
    return c.owned;
}
namespace {
/// Our own items when this process owns the clipboard (no round trip), else null.
std::vector<ClipboardItem>* localItems() {
    XClip& c = xc();
    if (!c.init()) return &g_local;
    return c.ownsNow() ? &c.items : nullptr;
}
} // namespace
bool clipboardText(std::string& out) {
    out.clear();
    if (std::vector<ClipboardItem>* items = localItems()) {
        ClipboardItem* it = findItem(*items, kClipboardText);
        if (!it || !provideOnce(*it)) return false;
        out = it->text;
        return true;
    }
    XClip& c = xc();
    std::vector<std::uint8_t> b;
    if (c.convert(c.utf8, b) || c.convert(c.textPlainUtf8, b)) {
        out.assign(b.begin(), b.end());
        return true;
    }
    if (!c.convert(x11::kXA_STRING, b)) return false;
    for (std::uint8_t ch : b) { // Latin-1 -> UTF-8
        if (ch < 0x80) out += char(ch);
        else { out += char(0xC0 | (ch >> 6)); out += char(0x80 | (ch & 0x3F)); }
    }
    return true;
}
bool clipboardImage(Image& out) {
    if (std::vector<ClipboardItem>* items = localItems()) {
        ClipboardItem* it = findItem(*items, kClipboardImage);
        if (!it || !provideOnce(*it) || it->image.width <= 0) return false;
        out = it->image;
        return true;
    }
    XClip& c = xc();
    std::vector<std::uint8_t> b;
    if (!c.convert(c.png, b) || b.empty()) return false;
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(b.data(), int(b.size()), &w, &h, &n, 4);
    if (!px) return false;
    out.resize(w, h);
    std::memcpy(out.pixels.data(), px, out.pixels.size() * 4);
    stbi_image_free(px);
    return true;
}
bool clipboardData(std::string_view mime, std::vector<std::uint8_t>& out) {
    out.clear();
    if (isText(mime)) {
        std::string s;
        if (!clipboardText(s)) return false;
        out.assign(s.begin(), s.end());
        return true;
    }
    if (std::vector<ClipboardItem>* items = localItems()) {
        ClipboardItem* it = findItem(*items, mime);
        if (!it || isImage(mime) || !provideOnce(*it)) return false;
        out = it->bytes;
        return true;
    }
    XClip& c = xc();
    return c.convert(c.x.InternAtom(c.d, std::string(mime).c_str(), 0), out);
}
std::vector<std::string> clipboardFormats() {
    std::vector<std::string> out;
    if (std::vector<ClipboardItem>* items = localItems()) {
        for (const ClipboardItem& it : *items) out.push_back(it.mime);
        return out;
    }
    XClip& c = xc();
    std::vector<std::uint8_t> b;
    if (!c.convert(c.targets, b)) return out;
    auto add = [&](std::string s) { if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s)); };
    for (std::size_t i = 0; i + sizeof(long) <= b.size(); i += sizeof(long)) {
        long a = 0;
        std::memcpy(&a, b.data() + i, sizeof a);
        const x11::Atom t = x11::Atom(a);
        if (t == c.utf8 || t == c.textPlainUtf8 || t == c.textPlain || t == x11::kXA_STRING || t == c.text) add(std::string(kClipboardText));
        else if (t == c.png) add(std::string(kClipboardImage));
        else if (t != c.targets && t != c.timestamp)
            if (char* name = c.x.GetAtomName(c.d, t)) {
                if (std::strchr(name, '/')) add(name);
                c.x.Free(name);
            }
    }
    return out;
}
bool clipboardOwned() {
    XClip& c = xc();
    return c.init() ? c.ownsNow() : !g_local.empty();
}
#endif

bool setClipboardText(std::string_view utf8) {
    std::vector<ClipboardItem> v(1);
    v[0].mime = std::string(kClipboardText);
    v[0].text = std::string(utf8);
    return setClipboard(std::move(v));
}
bool setClipboardImage(const Image& image) {
    std::vector<ClipboardItem> v(1);
    v[0].mime = std::string(kClipboardImage);
    v[0].image = image;
    return setClipboard(std::move(v));
}

} // namespace fuse::app
