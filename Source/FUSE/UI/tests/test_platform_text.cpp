// UI1 text gates on real OS windows (plan §4.13). One source, one executable per gate (FUSE_PLATFORM_TEXT_GATE):
//   text_input          committed text through the OS path into a focused LineEdit (Win32 WM_CHAR incl. a surrogate
//                       pair; X11 KeyPress -> XIM / Xutf8LookupString incl. remapped non-ASCII keysyms), Ctrl+A / C / V
//                       through the system clipboard against another client, IME session off outside text fields
//   ime_composition     a real IME composition: Win32 ImmSetCompositionStringW / ImmNotifyIME on the window's input
//                       context (IMM32 over TSF) -> inline pre-edit, caret, commit, cancel, candidate / composition
//                       window at the caret; X11 XIM compose (dead key) through libX11's built-in IM, then (child
//                       process) on-the-spot pre-edit, caret, commit and cancel through a real server IM: uim-xim with
//                       the Byeoru Hangul engine, started by the gate (skipped without uim-xim)
//   clipboard_roundtrip fuse::app clipboard vs another client (Win32: raw clipboard API on another thread; X11: a
//                       second X connection): text, RGBA image, custom MIME types, several formats, lazy provision,
//                       large INCR transfers (X11)
// Exit 77 when there is no display / input method.
#include "fuse/app/clipboard.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <imm.h>
#else
#include "x11_internal.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif

#ifndef FUSE_PLATFORM_TEXT_GATE
#define FUSE_PLATFORM_TEXT_GATE "text_input"
#endif

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
const std::string kTyped = "H\xC3\xA9\xF0\x9F\x98\x80!";     // "Hé😀!"
const std::string kPasted = "pasted \xE2\x9C\x93";            // "pasted ✓"
const std::string kNihon = "\xE3\x81\xAB\xE3\x81\xBB\xE3\x82\x93"; // "にほん"

void pump(app::Application& a, int n = 20) {
    for (int i = 0; i < n; ++i) a.step(0.005f);
}
bool waitFrames(app::Application& a, app::AppWindow& win, u64 frames = 1) {
    for (int i = 0; i < 600 && win.framesPresented() < frames; ++i) a.step(0.01f);
    pump(a);
    return win.framesPresented() >= frames;
}
template <class F>
bool until(app::Application& a, F&& f, int steps = 300) {
    for (int i = 0; i < steps && !f(); ++i) a.step(0.005f);
    return f();
}
/// Runs `fn` on another thread (the other client) while this thread keeps pumping the app's events.
void asOther(app::Application& a, const std::function<void()>& fn) {
    std::atomic<bool> done{false};
    std::thread t([&] { fn(); done = true; });
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done && std::chrono::steady_clock::now() < end) a.step(0.002f);
    t.join();
}
Image testImage(int wd, int ht, bool noise) {
    Image img;
    img.resize(wd, ht);
    u32 seed = 12345;
    for (int y = 0; y < ht; ++y)
        for (int x = 0; x < wd; ++x) {
            u32 p;
            if (noise) {
                seed = seed * 1664525u + 1013904223u;
                p = seed | 0xFF000000u;
            } else {
                p = u32(x * 255 / std::max(1, wd - 1)) | u32(y * 255 / std::max(1, ht - 1)) << 8 | u32((x * 7 + y * 3) & 0xFF) << 16 |
                    u32(x < wd / 2 ? 255 : 64 + (y & 0x7F)) << 24;
            }
            img.pixels[std::size_t(y) * std::size_t(wd) + std::size_t(x)] = p;
        }
    return img;
}
bool sameImage(const Image& a, const Image& b) { return a.width == b.width && a.height == b.height && a.pixels == b.pixels; }
bool hasFormat(const std::vector<std::string>& f, std::string_view m) { return std::find(f.begin(), f.end(), m) != f.end(); }

struct Window1 {
    app::AppWindow* win = nullptr;
    NodeHandle le, other;
};
Window1 makeWindow(app::Application& a, app::NativePlatform& native, const char* title) {
    Window1 r;
    r.win = &a.createWindow({.title = title, .width = 360, .height = 140});
    UiContext& ui = r.win->ui();
    const NodeHandle col = w::panel(ui, ui.root(), "col");
    ui.setCss(col, "padding", "10px");
    ui.setCss(col, "gap", "8px");
    r.le = w::lineEdit(ui, col, "", "le");
    r.other = w::button(ui, col, "Not a text field", "btn");
    native.show(*r.win);
    CHECK(waitFrames(a, *r.win));
    return r;
}
} // namespace

// ---- platform helpers: posting keys and acting as another clipboard client ----------------------------------------
#if defined(_WIN32)
namespace {
std::wstring widen(std::string_view s) {
    std::wstring o(std::size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), o.data(), int(o.size()));
    return o;
}
/// Ctrl held for this thread's keyboard state (what GetKeyState reports to the window procedure).
void setCtrl(bool down) {
    BYTE ks[256];
    GetKeyboardState(ks);
    ks[VK_CONTROL] = ks[VK_LCONTROL] = down ? 0x80 : 0;
    SetKeyboardState(ks);
}
void postKey(HWND h, UINT vk) {
    PostMessageW(h, WM_KEYDOWN, vk, 1);
    PostMessageW(h, WM_KEYUP, vk, LPARAM(0xC0000001));
}
void postChars(HWND h, const std::wstring& s) {
    for (wchar_t c : s) PostMessageW(h, WM_CHAR, WPARAM(c), 1); // a surrogate pair arrives as two WM_CHARs
}
bool rawOpen() {
    for (int i = 0; i < 200; ++i) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(5);
    }
    return false;
}
std::string rawText() {
    std::string out = "<none>";
    if (!rawOpen()) return "<locked>";
    if (HGLOBAL g = GetClipboardData(CF_UNICODETEXT))
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(g))) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
            out.assign(std::size_t(std::max(n - 1, 0)), '\0');
            WideCharToMultiByte(CP_UTF8, 0, p, -1, out.data(), n, nullptr, nullptr);
            GlobalUnlock(g);
        }
    CloseClipboard();
    return out;
}
bool rawSetText(std::string_view s) {
    const std::wstring wtext = widen(s);
    if (!rawOpen()) return false;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (wtext.size() + 1) * sizeof(wchar_t));
    std::memcpy(GlobalLock(g), wtext.c_str(), (wtext.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(g);
    const bool ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
    CloseClipboard();
    return ok;
}
std::vector<std::uint8_t> rawBytes(UINT fmt) {
    std::vector<std::uint8_t> out;
    if (!rawOpen()) return out;
    if (HGLOBAL g = GetClipboardData(fmt))
        if (const auto* p = static_cast<const std::uint8_t*>(GlobalLock(g))) {
            out.assign(p, p + GlobalSize(g));
            GlobalUnlock(g);
        }
    CloseClipboard();
    return out;
}
/// CF_DIBV5 as another application reads it: 32-bit BI_BITFIELDS, bottom-up.
bool rawImage(Image& out) {
    const std::vector<std::uint8_t> b = rawBytes(CF_DIBV5);
    if (b.size() < sizeof(BITMAPV5HEADER)) return false;
    BITMAPV5HEADER h;
    std::memcpy(&h, b.data(), sizeof h);
    if (h.bV5BitCount != 32 || h.bV5Compression != BI_BITFIELDS || h.bV5RedMask != 0xFF0000u || h.bV5AlphaMask != 0xFF000000u)
        return false;
    const int wd = h.bV5Width, ht = std::abs(h.bV5Height);
    if (b.size() < h.bV5Size + std::size_t(wd) * std::size_t(ht) * 4) return false;
    out.resize(wd, ht);
    for (int y = 0; y < ht; ++y)
        for (int x = 0; x < wd; ++x) {
            u32 p;
            std::memcpy(&p, b.data() + h.bV5Size + (std::size_t(h.bV5Height > 0 ? ht - 1 - y : y) * std::size_t(wd) + std::size_t(x)) * 4, 4);
            out.pixels[std::size_t(y) * std::size_t(wd) + std::size_t(x)] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
        }
    return true;
}
/// A 24-bit bottom-up CF_DIB (what Paint and many older applications put there).
bool rawSetDib24(const Image& img) {
    const std::size_t stride = (std::size_t(img.width) * 3 + 3) & ~std::size_t(3);
    std::vector<std::uint8_t> b(sizeof(BITMAPINFOHEADER) + stride * std::size_t(img.height));
    BITMAPINFOHEADER h{};
    h.biSize = sizeof h;
    h.biWidth = img.width;
    h.biHeight = img.height;
    h.biPlanes = 1;
    h.biBitCount = 24;
    h.biCompression = BI_RGB;
    std::memcpy(b.data(), &h, sizeof h);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            const u32 p = img.at(x, img.height - 1 - y);
            std::uint8_t* d = b.data() + sizeof h + std::size_t(y) * stride + std::size_t(x) * 3;
            d[0] = std::uint8_t(p >> 16);
            d[1] = std::uint8_t(p >> 8);
            d[2] = std::uint8_t(p);
        }
    if (!rawOpen()) return false;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, b.size());
    std::memcpy(GlobalLock(g), b.data(), b.size());
    GlobalUnlock(g);
    const bool ok = SetClipboardData(CF_DIB, g) != nullptr;
    CloseClipboard();
    return ok;
}
UINT customFormat(std::string_view mime) { return RegisterClipboardFormatW(widen(mime).c_str()); }
} // namespace
#else
namespace {
namespace x11 = fuse::app::x11;
/// Another X client on its own connection: types keys into the app, owns / reads the CLIPBOARD selection.
struct Other {
    x11::Lib x;
    x11::Display* d = nullptr;
    x11::Window w = 0;
    x11::Atom clipboard = 0, targets = 0, utf8 = 0, png = 0, incr = 0, prop = 0;
    unsigned char (*keysymToKeycode)(x11::Display*, x11::KeySym) = nullptr;
    int (*changeMapping)(x11::Display*, int, int, x11::KeySym*, int) = nullptr;
    int (*displayKeycodes)(x11::Display*, int*, int*) = nullptr;
    x11::KeySym* (*getMapping)(x11::Display*, unsigned char, int, int*) = nullptr;
    int spare = 0; ///< keycode remapped for keysyms the keymap lacks

    bool open() {
        if (!x.load() || !(d = x.OpenDisplay(nullptr))) return false;
        w = x.CreateSimpleWindow(d, x.RootWindow(d, x.DefaultScreen(d)), 0, 0, 1, 1, 0, 0, 0);
        x.SelectInput(d, w, x11::kPropertyChangeMask);
        auto atom = [&](const char* n) { return x.InternAtom(d, n, 0); };
        clipboard = atom("CLIPBOARD");
        targets = atom("TARGETS");
        utf8 = atom("UTF8_STRING");
        png = atom("image/png");
        incr = atom("INCR");
        prop = atom("GATE_SEL");
        keysymToKeycode = reinterpret_cast<decltype(keysymToKeycode)>(dlsym(x.so, "XKeysymToKeycode"));
        changeMapping = reinterpret_cast<decltype(changeMapping)>(dlsym(x.so, "XChangeKeyboardMapping"));
        displayKeycodes = reinterpret_cast<decltype(displayKeycodes)>(dlsym(x.so, "XDisplayKeycodes"));
        getMapping = reinterpret_cast<decltype(getMapping)>(dlsym(x.so, "XGetKeyboardMapping"));
        int lo = 0, hi = 0;
        displayKeycodes(d, &lo, &hi);
        int per = 0;
        x11::KeySym* map = getMapping(d, (unsigned char)lo, hi - lo + 1, &per);
        for (int k = hi; k >= lo && map && !spare; --k) { // a keycode with no symbols at all
            bool empty = true;
            for (int i = 0; i < per; ++i) empty &= map[(k - lo) * per + i] == 0;
            if (empty) spare = k;
        }
        if (map) x.Free(map);
        x.Flush(d);
        return true;
    }
    ~Other() { if (d) x.CloseDisplay(d); }
    /// Keycode for `sym`: the keymap's, else the spare keycode remapped to it (the app refreshes on MappingNotify).
    int keycode(x11::KeySym sym, app::Application& a) {
        if (const int k = keysymToKeycode(d, sym)) return k;
        x11::KeySym syms[2] = {sym, sym};
        changeMapping(d, spare, 2, syms, 1);
        x.Sync(d, 0);
        pump(a, 10);
        return spare;
    }
    void key(x11::Window target, int code, unsigned state, bool press) {
        x11::Event ev{};
        ev.input.type = press ? x11::KeyPress : x11::KeyRelease;
        ev.input.display = d;
        ev.input.window = target;
        ev.input.root = x.RootWindow(d, x.DefaultScreen(d));
        ev.input.state = state;
        ev.input.detail = unsigned(code);
        ev.input.same_screen = 1;
        ev.input.time = 1;
        x.SendEvent(d, target, 1, x11::kKeyPressMask | x11::kKeyReleaseMask, &ev);
        x.Flush(d);
    }
    void tap(app::Application& a, x11::Window target, x11::KeySym sym, unsigned state = 0) {
        const int code = keycode(sym, a);
        key(target, code, state, true);
        key(target, code, state, false);
        pump(a, 6);
    }
    bool wait(x11::Event& ev, int type, int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            while (x.Pending(d)) {
                x.NextEvent(d, &ev);
                if (ev.type == type && (type != x11::PropertyNotify || (ev.property.window == w && ev.property.state == 0))) return true;
            }
            pollfd p{x.ConnectionNumber(d), POLLIN, 0};
            ::poll(&p, 1, 20);
        }
        return false;
    }
    /// ConvertSelection(CLIPBOARD, target) as a paste (INCR transfers too). Run on its own thread while the app pumps.
    bool read(x11::Atom target, std::vector<std::uint8_t>& out, bool* sawIncr = nullptr) {
        out.clear();
        x.ConvertSelection(d, clipboard, target, prop, w, 0);
        x.Flush(d);
        x11::Event ev{};
        if (!wait(ev, x11::SelectionNotify, 4000) || !ev.selection.property) return false;
        auto take = [&](std::vector<std::uint8_t>& o, x11::Atom& type) {
            int format = 0;
            unsigned long n = 0, after = 0;
            unsigned char* p = nullptr;
            x.GetWindowProperty(d, w, prop, 0, 1L << 26, 1, 0, &type, &format, &n, &after, &p);
            o.assign(p, p + (p ? n * (format == 32 ? sizeof(long) : 1) : 0));
            if (p) x.Free(p);
            x.Flush(d);
        };
        x11::Atom type = 0;
        take(out, type);
        if (type == incr) {
            if (sawIncr) *sawIncr = true;
            out.clear();
            for (;;) {
                if (!wait(ev, x11::PropertyNotify, 4000)) return false;
                std::vector<std::uint8_t> part;
                take(part, type);
                if (part.empty()) break;
                out.insert(out.end(), part.begin(), part.end());
            }
        }
        return true;
    }
    /// Owns CLIPBOARD with `data` (target atom -> bytes) and serves requests until `stop` (own thread).
    void serve(const std::vector<std::pair<x11::Atom, std::vector<std::uint8_t>>>& data, std::atomic<bool>& stop, std::atomic<bool>& owned) {
        x.SetSelectionOwner(d, clipboard, w, 0);
        x.Flush(d);
        owned = x.GetSelectionOwner(d, clipboard) == w;
        while (!stop) {
            while (x.Pending(d)) {
                x11::Event ev{};
                x.NextEvent(d, &ev);
                if (ev.type != x11::SelectionRequest) continue;
                const x11::SelectionRequestEv& rq = ev.selectionRequest;
                x11::Event rep{};
                rep.selection.type = x11::SelectionNotify;
                rep.selection.requestor = rq.requestor;
                rep.selection.selection = rq.selection;
                rep.selection.target = rq.target;
                rep.selection.time = rq.time;
                if (rq.target == targets) {
                    std::vector<long> t{long(targets)};
                    for (const auto& [a, b] : data) t.push_back(long(a));
                    x.ChangeProperty(d, rq.requestor, rq.property, x11::kXA_ATOM, 32, 0, reinterpret_cast<const unsigned char*>(t.data()), int(t.size()));
                    rep.selection.property = rq.property;
                }
                for (const auto& [a, b] : data)
                    if (a == rq.target) {
                        x.ChangeProperty(d, rq.requestor, rq.property, a, 8, 0, b.data(), int(b.size()));
                        rep.selection.property = rq.property;
                    }
                x.SendEvent(d, rq.requestor, 0, 0, &rep);
                x.Flush(d);
            }
            pollfd p{x.ConnectionNumber(d), POLLIN, 0};
            ::poll(&p, 1, 5);
        }
    }
};
std::vector<std::uint8_t> encodePng(const Image& img) {
    std::vector<std::uint8_t> out;
    stbi_write_png_to_func([](void* c, void* p, int n) {
        auto* v = static_cast<std::vector<std::uint8_t>*>(c);
        v->insert(v->end(), static_cast<std::uint8_t*>(p), static_cast<std::uint8_t*>(p) + n);
    }, &out, img.width, img.height, 4, img.pixels.data(), img.width * 4);
    return out;
}
bool decodePng(const std::vector<std::uint8_t>& b, Image& out) {
    int wd = 0, ht = 0, n = 0;
    stbi_uc* p = stbi_load_from_memory(b.data(), int(b.size()), &wd, &ht, &n, 4);
    if (!p) return false;
    out.resize(wd, ht);
    std::memcpy(out.pixels.data(), p, out.pixels.size() * 4);
    stbi_image_free(p);
    return true;
}
/// Serves `data` from the other client while `fn` runs on this (the app's) thread.
void otherOwns(Other& o, const std::vector<std::pair<x11::Atom, std::vector<std::uint8_t>>>& data, const std::function<void()>& fn) {
    std::atomic<bool> stop{false}, owned{false};
    std::thread t([&] { o.serve(data, stop, owned); });
    for (int i = 0; i < 200 && !owned; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    fn();
    stop = true;
    t.join();
}
std::vector<std::uint8_t> bytesOf(std::string_view s) { return {s.begin(), s.end()}; }

// ---- a server IM for the pre-edit part of ime_composition ------------------------------------------------------------
// uim-xim (uim's XIM bridge) with the Byeoru Hangul engine: an on-the-spot (XIMPreeditCallbacks) server IM that needs
// no D-Bus session and composes from plain key events, so synthesized keys drive real compositions. Found as
// $FUSE_TEST_UIM_XIM, else the one found at configure time (also ~/opt/<pkg>/usr/bin: `apt-get download uim-xim
// uim-data libuim8 libuim-scm0 libuim-custom2 libuim-data libgcroots0 && dpkg -x <deb> ~/opt/uim`), else on PATH.
// An unpacked (non-/usr) prefix gets its libraries and Scheme files through LD_LIBRARY_PATH / LIBUIM_SCM_FILES and
// a private module registry (uim-module-manager --register byeoru, what the package's postinst does). uim-xim
// serves named locales only (not C.UTF-8): without an installed en_US.UTF-8 one is compiled with localedef into the
// scratch directory (LOCPATH). Everything private lives in a mkdtemp directory, removed afterwards.
bool exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}
std::string dirOf(const std::string& p) { return p.substr(0, p.rfind('/')); }
/// Runs `argv` with `env`, output appended to `log`; the exit status (-1: not started, killed or over `seconds`).
int runWait(const std::vector<std::string>& argv, const std::vector<std::string>& env, const std::string& log, int seconds = 60) {
    std::vector<char*> av, ev;
    for (const std::string& s : argv) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);
    for (const std::string& s : env) ev.push_back(const_cast<char*>(s.c_str()));
    ev.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), ev.data());
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    int status = 0;
    for (int i = 0; i < seconds * 50; ++i) {
        if (::waitpid(pid, &status, WNOHANG) == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return -1;
}
void removeTree(const std::string& p) {
    if (DIR* d = ::opendir(p.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            struct stat st{};
            const std::string c = p + "/" + n;
            if (::lstat(c.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) removeTree(c);
            else ::unlink(c.c_str());
        }
        ::closedir(d);
    }
    ::rmdir(p.c_str());
}
struct ServerIm {
    pid_t pid = 0;
    std::string path, dir;
    std::vector<std::string> extra; ///< what the app needs as well: LOCPATH (a compiled en_US.UTF-8)
    ~ServerIm() { stop(); }
    static std::string find() {
        if (const char* e = std::getenv("FUSE_TEST_UIM_XIM"); e && *e) return e;
#ifdef FUSE_TEST_UIM_XIM_PATH
        if (exists(FUSE_TEST_UIM_XIM_PATH)) return FUSE_TEST_UIM_XIM_PATH;
#endif
        const std::string dirs = std::getenv("PATH") ? std::getenv("PATH") : "";
        for (std::size_t at = 0; at <= dirs.size();) {
            const std::size_t end = std::min(dirs.find(':', at), dirs.size());
            const std::string p = dirs.substr(at, end - at) + "/uim-xim";
            if (end > at && ::access(p.c_str(), X_OK) == 0) return p;
            at = end + 1;
        }
        return {};
    }
    bool start(std::string& why) {
        path = find();
        if (path.empty()) { why = "no uim-xim (set FUSE_TEST_UIM_XIM, or unpack uim-xim into ~/opt/uim)"; return false; }
        char tmpl[] = "/tmp/fuse-ime-XXXXXX";
        if (!::mkdtemp(tmpl)) { why = "mkdtemp failed"; return false; }
        dir = tmpl;
        std::vector<std::string> env;
        for (char** e = environ; *e; ++e) {
            const std::string kv = *e;
            bool drop = false;
            for (const char* k : {"LD_LIBRARY_PATH=", "LIBUIM_", "HOME=", "LANG=", "LANGUAGE=", "LC_", "LOCPATH=", "XMODIFIERS="})
                drop |= kv.rfind(k, 0) == 0;
            if (!drop) env.push_back(kv);
        }
        env.push_back("HOME=" + dir); // uim's ~/.uim.d: none of the user's settings
        if (locale_t l = ::newlocale(LC_CTYPE_MASK, "en_US.UTF-8", nullptr)) {
            ::freelocale(l);
        } else {
            ::mkdir((dir + "/locale").c_str(), 0755);
            runWait({"localedef", "-i", "en_US", "-f", "UTF-8", dir + "/locale/en_US.UTF-8"}, env, dir + "/localedef.log");
            if (!exists(dir + "/locale/en_US.UTF-8/LC_CTYPE")) { why = "no en_US.UTF-8 locale and localedef failed"; return false; }
            extra.push_back("LOCPATH=" + dir + "/locale");
        }
        for (const std::string& kv : extra) env.push_back(kv);
        const std::string usr = dirOf(dirOf(path));
        if (usr != "/usr" && usr != "/usr/local" && exists(usr + "/share/uim")) { // an unpacked package
            const std::string lib = usr + "/lib/x86_64-linux-gnu", scm = dir + "/scm";
            ::mkdir(scm.c_str(), 0755);
            if (DIR* d = ::opendir((usr + "/share/uim").c_str())) { // the Scheme files, without the registry links
                while (dirent* e = ::readdir(d)) {
                    const std::string n = e->d_name;
                    if (n == "." || n == ".." || n == "installed-modules.scm" || n == "loader.scm") continue;
                    if (::symlink((usr + "/share/uim/" + n).c_str(), (scm + "/" + n).c_str()) != 0) {
                        ::closedir(d);
                        why = "symlink failed";
                        return false;
                    }
                }
                ::closedir(d);
            }
            const char* ld = std::getenv("LD_LIBRARY_PATH");
            env.push_back("LD_LIBRARY_PATH=" + lib + (ld && *ld ? std::string(":") + ld : ""));
            env.push_back("LIBUIM_SCM_FILES=" + scm);
            env.push_back("LIBUIM_SYSTEM_SCM_FILES=" + usr + "/share/uim/lib");
            env.push_back("LIBUIM_PLUGIN_LIB_DIR=" + lib + "/uim/plugin");
            runWait({usr + "/bin/uim-module-manager", "--register", "byeoru", "--path", scm}, env, dir + "/uim-module-manager.log");
            if (!exists(scm + "/loader.scm")) { why = "uim-module-manager --register byeoru failed"; return false; }
        }
        env.push_back("LANG=en_US.UTF-8");
        env.push_back("LC_ALL=en_US.UTF-8");
        std::vector<char*> av{const_cast<char*>(path.c_str()), const_cast<char*>("--engine=byeoru"), nullptr}, ev;
        for (const std::string& kv : env) ev.push_back(const_cast<char*>(kv.c_str()));
        ev.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        const std::string log = dir + "/uim-xim.log";
        posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
        const int rc = posix_spawn(&pid, path.c_str(), &fa, nullptr, av.data(), ev.data());
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0) { pid = 0; why = "cannot start " + path; return false; }
        // ready once the root window's XIM_SERVERS lists @server=uim
        x11::Lib x;
        x11::Display* d = x.load() ? x.OpenDisplay(nullptr) : nullptr;
        bool ready = false;
        for (int i = 0; d && i < 250 && !ready; ++i) {
            std::vector<unsigned char> atoms;
            if (x.property(d, x.RootWindow(d, x.DefaultScreen(d)), x.InternAtom(d, "XIM_SERVERS", 0), 4 /*XA_ATOM*/, atoms)) {
                for (std::size_t k = 0; k + sizeof(long) <= atoms.size() && !ready; k += sizeof(long)) {
                    long at = 0;
                    std::memcpy(&at, atoms.data() + k, sizeof at);
                    if (char* n = x.GetAtomName(d, x11::Atom(at))) {
                        ready = std::strcmp(n, "@server=uim") == 0;
                        x.Free(n);
                    }
                }
            }
            if (::waitpid(pid, nullptr, WNOHANG) == pid) { pid = 0; break; }
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (d) x.CloseDisplay(d);
        if (!ready) { why = "uim-xim did not register @server=uim"; return false; }
        return true;
    }
    /// Runs this executable again with `arg` against the server IM (XMODIFIERS=@im=uim, LOCPATH if compiled here).
    /// The locale is left alone: the app finds en_US.UTF-8 itself when uim-xim refuses C.UTF-8.
    int runChild(const char* arg) {
        std::vector<std::string> env;
        for (char** e = environ; *e; ++e)
            if (std::strncmp(*e, "XMODIFIERS=", 11) != 0 && std::strncmp(*e, "LOCPATH=", 8) != 0) env.push_back(*e);
        env.push_back("XMODIFIERS=@im=uim");
        for (const std::string& kv : extra) env.push_back(kv);
        std::vector<char*> av{const_cast<char*>("/proc/self/exe"), const_cast<char*>(arg), nullptr}, ev;
        for (const std::string& kv : env) ev.push_back(const_cast<char*>(kv.c_str()));
        ev.push_back(nullptr);
        std::fflush(stdout);
        std::fflush(stderr);
        pid_t child = 0;
        if (posix_spawn(&child, "/proc/self/exe", nullptr, nullptr, av.data(), ev.data()) != 0) return -1;
        int status = 0;
        for (int i = 0; i < 90 * 50; ++i) {
            if (::waitpid(child, &status, WNOHANG) == child) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        return -1;
    }
    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            for (int i = 0; i < 100 && ::waitpid(pid, nullptr, WNOHANG) == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, WNOHANG);
            pid = 0;
        }
        if (!dir.empty() && !std::getenv("FUSE_TEST_KEEP_IME_DIR")) removeTree(dir);
        dir.clear();
    }
};
} // namespace
#endif

// ---- text_input --------------------------------------------------------------------------------------------------------
static int gateTextInput(int argc, char** argv) {
    app::Application a{{.name = "gate_text_input", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    if (!native.available()) { std::printf("fuse_platform_text_input: skipped (%.*s)\n", int(native.lastError().size()), native.lastError().data()); return 77; }
    Window1 wnd = makeWindow(a, native, "fuse text input");
    UiContext& ui = wnd.win->ui();
    const unsigned long long hw = native.nativeWindow(*wnd.win);
    CHECK(!native.textInputActive(*wnd.win)); // nothing focused: the IME is off
    ui.setFocus(wnd.le);
    CHECK(until(a, [&] { return native.textInputActive(*wnd.win); }));
    const Rect area = native.textInputArea(*wnd.win);
    std::printf("  text field focused: IME session on, caret area %.0f,%.0f %.0fx%.0f\n", area.x, area.y, area.w, area.h);
    CHECK(area.w >= 1.f && area.h >= 8.f && area.x >= 0.f);
#if defined(_WIN32)
    HWND h = reinterpret_cast<HWND>(hw);
    // committed text: WM_CHAR, the emoji as a UTF-16 surrogate pair
    postChars(h, widen(kTyped));
    CHECK(until(a, [&] { return ui.text(wnd.le) == kTyped; }));
    std::printf("  WM_CHAR: \"%s\" (%zu bytes)\n", std::string(ui.text(wnd.le)).c_str(), ui.text(wnd.le).size());
    // Ctrl+A, Ctrl+C: another thread reads the OS clipboard
    setCtrl(true);
    postKey(h, 'A');
    postKey(h, 'C');
    pump(a, 10);
    setCtrl(false);
    std::string seen;
    asOther(a, [&] { seen = rawText(); });
    std::printf("  Ctrl+C -> CF_UNICODETEXT read by another thread: \"%s\"\n", seen.c_str());
    CHECK(seen == kTyped);
    // another client puts text there; End, Ctrl+V pastes it
    bool set = false;
    asOther(a, [&] { set = rawSetText(kPasted); });
    CHECK(set && !app::clipboardOwned());
    postKey(h, VK_END);
    pump(a, 5);
    setCtrl(true);
    postKey(h, 'V');
    pump(a, 10);
    setCtrl(false);
    CHECK(until(a, [&] { return ui.text(wnd.le) == kTyped + kPasted; }));
    // Ctrl+X empties the field and leaves the text on the clipboard
    setCtrl(true);
    postKey(h, 'A');
    postKey(h, 'X');
    pump(a, 10);
    setCtrl(false);
    CHECK(until(a, [&] { return ui.text(wnd.le).empty(); }));
    asOther(a, [&] { seen = rawText(); });
    CHECK(seen == kTyped + kPasted);
    // focus leaves the text field: the IME context is detached, typing does nothing
    ui.setFocus(wnd.other);
    CHECK(until(a, [&] { return !native.textInputActive(*wnd.win); }));
    HIMC imc = ImmGetContext(h);
    std::printf("  button focused: IME session %s, window input context %s\n", native.textInputActive(*wnd.win) ? "on" : "off",
                imc ? "attached" : "detached");
    CHECK(!imc);
    if (imc) ImmReleaseContext(h, imc);
    postChars(h, L"zz");
    pump(a, 10);
    CHECK(ui.text(wnd.le).empty());
#else
    Other o;
    CHECK(o.open());
    if (!o.d) return 1;
    const x11::Window target = x11::Window(hw);
    std::printf("  input method: %s\n", native.inputMethodAvailable() ? "open (XIM)" : "none (XLookupString)");
    // KeyPress events; é and 😀 on a remapped keycode (MappingNotify -> XRefreshKeyboardMapping in the app)
    o.tap(a, target, 'H', 1);
    o.tap(a, target, 0xE9);         // eacute
    o.tap(a, target, 0x0101F600);   // U+1F600
    o.tap(a, target, '!', 1);
    CHECK(until(a, [&] { return ui.text(wnd.le) == kTyped; }));
    std::printf("  KeyPress: \"%s\" (%zu bytes)\n", std::string(ui.text(wnd.le)).c_str(), ui.text(wnd.le).size());
    o.tap(a, target, 'a', 4);
    o.tap(a, target, 'c', 4);
    CHECK(app::clipboardOwned());
    std::vector<std::uint8_t> got;
    asOther(a, [&] { o.read(o.utf8, got); });
    std::printf("  Ctrl+C -> UTF8_STRING read by another client: \"%.*s\"\n", int(got.size()), reinterpret_cast<const char*>(got.data()));
    CHECK(got == bytesOf(kTyped));
    otherOwns(o, {{o.utf8, bytesOf(kPasted)}}, [&] {
        CHECK(!app::clipboardOwned());
        o.tap(a, target, 0xff57); // End
        o.tap(a, target, 'v', 4);
        CHECK(until(a, [&] { return ui.text(wnd.le) == kTyped + kPasted; }));
    });
    o.tap(a, target, 'a', 4);
    o.tap(a, target, 'x', 4);
    CHECK(until(a, [&] { return ui.text(wnd.le).empty(); }));
    asOther(a, [&] { o.read(o.utf8, got); });
    CHECK(got == bytesOf(kTyped + kPasted));
    ui.setFocus(wnd.other);
    CHECK(until(a, [&] { return !native.textInputActive(*wnd.win); }));
    o.tap(a, target, 'z');
    CHECK(ui.text(wnd.le).empty());
#endif
    std::printf("  after paste: \"%s\"; button focused: IME session off\n", (kTyped + kPasted).c_str());
    return 0;
}

// ---- ime_composition ---------------------------------------------------------------------------------------------------
static int gateIme(int argc, char** argv) {
    app::Application a{{.name = "gate_ime", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    if (!native.available()) { std::printf("fuse_platform_ime_composition: skipped (%.*s)\n", int(native.lastError().size()), native.lastError().data()); return 77; }
    if (!native.inputMethodAvailable()) { std::printf("fuse_platform_ime_composition: skipped (no input method)\n"); return 77; }
    Window1 wnd = makeWindow(a, native, "fuse IME");
    UiContext& ui = wnd.win->ui();
    ui.setFocus(wnd.le);
    ui.textInput("ab");
    CHECK(ui.text(wnd.le) == "ab");
    CHECK(until(a, [&] { return native.textInputActive(*wnd.win); }));
    pump(a);
    const unsigned long long hw = native.nativeWindow(*wnd.win);
    const f32 s = wnd.win->scale();
    const Rect caret = ui.textCaret();
    const Rect area = native.textInputArea(*wnd.win);
    std::printf("  caret %.1f,%.1f %.1fx%.1f (UI units, scale %.2f) -> IME area %.0f,%.0f %.0fx%.0f\n", caret.x, caret.y, caret.w,
                caret.h, s, area.x, area.y, area.w, area.h);
    CHECK(std::abs(area.x - std::round(caret.x * s)) <= 1.f && std::abs(area.y - std::round(caret.y * s)) <= 1.f);
#if defined(_WIN32)
    HWND h = reinterpret_cast<HWND>(hw);
    HKL hkl = GetKeyboardLayout(0);
    std::printf("  keyboard layout %08llx (IMM32 IME %d); compositions go through the window's input context\n",
                (unsigned long long)(reinterpret_cast<std::uintptr_t>(hkl) & 0xFFFFFFFFu), ImmIsIME(hkl));
    HIMC imc = ImmGetContext(h);
    CHECK(imc != nullptr);
    if (!imc) return 1;
    // the composition window sits at the caret, the candidate list below it and never over it
    COMPOSITIONFORM cf{};
    CANDIDATEFORM cand{};
    CHECK(ImmGetCompositionWindow(imc, &cf) && cf.dwStyle == CFS_POINT && cf.ptCurrentPos.x == LONG(area.x) && cf.ptCurrentPos.y == LONG(area.y));
    CHECK(ImmGetCandidateWindow(imc, 0, &cand) && cand.dwStyle == CFS_EXCLUDE && cand.rcArea.top == LONG(area.y) &&
          cand.rcArea.bottom == LONG(area.y + area.h));
    // Losing focus commits a composition (IMM32 does that on WM_KILLFOCUS) and other applications' windows (other
    // gates on this desktop) may take it at any time: a step that saw a focus loss is set back and retried.
    static std::atomic<int> s_focusLost{0};
    native.setMessageHook(*wnd.win, {nullptr, [](void*, unsigned m, std::uintptr_t, std::intptr_t, std::intptr_t&) {
        if (m == WM_KILLFOCUS) ++s_focusLost;
        return false; }});
    auto step = [&](const char* what, const std::string& base, const std::function<bool()>& body) {
        for (int attempt = 0; attempt < 10; ++attempt) {
            const int lost0 = s_focusLost;
            if (body()) return true;
            if (s_focusLost == lost0) return false; // a real failure
            std::printf("  %s: focus taken by another window meanwhile (composition committed): retrying\n", what);
            ui.setFocus(wnd.le);
            ui.keyDown(Key::A, kModCtrl);
            ui.textInput(base);
            ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            pump(a);
        }
        return false;
    };
    auto glyphs = [&] {
        int n = 0;
        for (const DrawCmd& c : ui.drawList().cmds()) n += c.op == DrawOp::Glyph;
        return n;
    };
    const int glyphs0 = glyphs();
    ImmSetOpenStatus(imc, TRUE);
    const std::wstring nihon = widen(kNihon);
    std::string preedit;
    i32 preCaret = -1;
    int glyphsDuring = 0;
    // pre-edit "にほん": shown inline, not committed; then CPS_COMPLETE types it once (no WM_IME_CHAR duplicate)
    CHECK(step("pre-edit + commit", "ab", [&] {
        if (!ImmSetCompositionStringW(imc, SCS_SETSTR, (void*)nihon.c_str(), DWORD(nihon.size() * 2), nullptr, 0)) return false;
        if (!until(a, [&] { return ui.composition() == kNihon; })) return false;
        pump(a, 3); // a frame with the pre-edit
        preedit = ui.composition();
        preCaret = ui.compositionCaret();
        glyphsDuring = glyphs();
        if (ui.text(wnd.le) != "ab" || preedit != kNihon) return false;
        if (!ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0)) return false;
        return until(a, [&] { return ui.composition().empty() && ui.text(wnd.le) == "ab" + kNihon; });
    }));
    std::printf("  ImmSetCompositionStringW: pre-edit \"%s\" caret %d, %d -> %d glyphs drawn\n", preedit.c_str(), preCaret,
                glyphs0, glyphsDuring);
    CHECK(preedit == kNihon && preCaret == i32(kNihon.size()));
    CHECK(glyphsDuring >= glyphs0 + 3); // the pre-edit is drawn inline
    pump(a, 10);
    std::printf("  CPS_COMPLETE: text \"%s\", pre-edit \"%s\"\n", std::string(ui.text(wnd.le)).c_str(), std::string(ui.composition()).c_str());
    CHECK(ui.text(wnd.le) == "ab" + kNihon);
    // cancel: the pre-edit goes, nothing is typed
    const std::wstring xyz = L"xyz";
    CHECK(step("cancel", "ab" + kNihon, [&] {
        if (!ImmSetCompositionStringW(imc, SCS_SETSTR, (void*)xyz.c_str(), DWORD(xyz.size() * 2), nullptr, 0)) return false;
        if (!until(a, [&] { return ui.composition() == "xyz"; })) return false;
        if (!ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0)) return false;
        return until(a, [&] { return ui.composition().empty(); }) && ui.text(wnd.le) == "ab" + kNihon;
    }));
    pump(a, 10);
    std::printf("  CPS_CANCEL: text \"%s\", pre-edit empty %d\n", std::string(ui.text(wnd.le)).c_str(), int(ui.composition().empty()));
    CHECK(ui.text(wnd.le) == "ab" + kNihon);
    ImmReleaseContext(h, imc);
    // focus leaves the field: no input context, no composition possible
    ui.setFocus(wnd.other);
    CHECK(until(a, [&] { return !native.textInputActive(*wnd.win); }));
    imc = ImmGetContext(h);
    CHECK(!imc);
    if (imc) ImmReleaseContext(h, imc);
#else
    Other o;
    CHECK(o.open());
    if (!o.d) return 1;
    const x11::Window target = x11::Window(hw);
    const bool serverIm = native.inputMethodPreedit(); // $XMODIFIERS already names a running server IM
    std::printf("  XIM open, pre-edit callbacks: %s\n", serverIm ? "yes ($XMODIFIERS server IM)" : "no (libX11's built-in IM)");
    if (!serverIm) {
        // a compose sequence through the built-in input method: dead_acute, e -> é (XFilterEvent swallows the dead key)
        o.tap(a, target, 0xfe51); // dead_acute
        pump(a, 5);
        const std::string mid(ui.text(wnd.le));
        o.tap(a, target, 'e');
        CHECK(until(a, [&] { return ui.text(wnd.le) == "ab\xC3\xA9"; }));
        std::printf("  dead_acute + e: after the dead key \"%s\", then \"%s\"\n", mid.c_str(), std::string(ui.text(wnd.le)).c_str());
        CHECK(mid == "ab");
    }
    const std::string before(ui.text(wnd.le));
    ui.setFocus(wnd.other);
    CHECK(until(a, [&] { return !native.textInputActive(*wnd.win); }));
    o.tap(a, target, 0xfe51);
    o.tap(a, target, 'e');
    CHECK(ui.text(wnd.le) == before);
#endif
    std::printf("  button focused: IME session off\n");
#if !defined(_WIN32)
    // on-the-spot pre-edit through a real server IM (uim-xim + Byeoru), in a child process: an IM is opened once
    // per process, and this one already has libX11's built-in IM. FUSE_TEST_REQUIRE_SERVER_IM=1 makes a missing
    // server a failure instead of a skip.
    if (!serverIm) {
        ServerIm im;
        std::string why;
        if (im.start(why)) {
            std::printf("  server IM: %s --engine=byeoru (XMODIFIERS=@im=uim%s)\n", im.path.c_str(), im.extra.empty() ? "" : ", en_US.UTF-8 compiled with localedef");
            const int rc = im.runChild("--server-im");
            if (rc != 0) std::fprintf(stderr, "  server IM part: child exit %d\n", rc);
            CHECK(rc == 0);
        } else {
            const char* req = std::getenv("FUSE_TEST_REQUIRE_SERVER_IM");
            std::printf("  server IM pre-edit: skipped (%s)\n", why.c_str());
            CHECK(!(req && *req == '1'));
        }
    }
#endif
    return 0;
}

#if !defined(_WIN32)
// The server-IM half of ime_composition (run by gateIme as a child process with XMODIFIERS=@im=uim): Byeoru, the
// Hangul engine of uim, on the 2-beolsik layout. Shift+Space turns it on; g k s compose 한 in the pre-edit (drawn
// inline, nothing typed yet), r starts the next syllable and commits 한, m f make 글, Return commits it; x then
// BackSpace cancels in the IM; a k then a focus change cancels in FUSE (the IM is reset, nothing typed); Shift+Space
// turns it off and z types z.
static int gateImeServer(int argc, char** argv) {
    app::Application a{{.name = "gate_ime_server", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    if (!native.available()) { std::printf("  server IM child: no display\n"); return 1; }
    CHECK(native.inputMethodAvailable() && native.inputMethodPreedit());
    if (!native.inputMethodPreedit()) { std::fprintf(stderr, "  server IM child: no pre-edit callbacks from @im=uim\n"); return 1; }
    Window1 wnd = makeWindow(a, native, "fuse IME (uim)");
    UiContext& ui = wnd.win->ui();
    ui.setFocus(wnd.le);
    ui.textInput("ab");
    CHECK(until(a, [&] { return native.textInputActive(*wnd.win); }));
    pump(a);
    Other o;
    CHECK(o.open());
    if (!o.d) return 1;
    const x11::Window target = x11::Window(native.nativeWindow(*wnd.win));
    auto glyphs = [&] {
        int n = 0;
        for (const DrawCmd& c : ui.drawList().cmds()) n += c.op == DrawOp::Glyph;
        return n;
    };
    const std::string kHieut = "\xE3\x85\x8E", kHa = "\xED\x95\x98", kHan = "\xED\x95\x9C"; // ㅎ 하 한
    const std::string kGiyeok = "\xE3\x84\xB1", kGeu = "\xEA\xB7\xB8", kGeul = "\xEA\xB8\x80"; // ㄱ 그 글
    const std::string kTieut = "\xE3\x85\x8C", kMieum = "\xE3\x85\x81", kMa = "\xEB\xA7\x88", kA = "\xE3\x85\x8F"; // ㅌ ㅁ 마 ㅏ
    int failedSteps = 0;
    auto step = [&](x11::KeySym sym, unsigned state, const std::string& pre, const std::string& text, const char* what) {
        o.tap(a, target, sym, state);
        const bool ok = until(a, [&] { return ui.composition() == pre && ui.text(wnd.le) == text; }, 600);
        std::printf("    %-22s pre-edit \"%s\" caret %d, text \"%s\"%s\n", what, std::string(ui.composition()).c_str(), ui.compositionCaret(),
                    std::string(ui.text(wnd.le)).c_str(), ok ? "" : "   <- expected different");
        if (!ok) ++failedSteps;
        return ok;
    };
    const int glyphs0 = glyphs();
    o.tap(a, target, ' ', 1 /*ShiftMask*/); // Byeoru on
    pump(a, 10);
    CHECK(ui.text(wnd.le) == "ab" && ui.composition().empty()); // the toggle types nothing
    step('g', 0, kHieut, "ab", "g");
    step('k', 0, kHa, "ab", "k");
    CHECK(step('s', 0, kHan, "ab", "s"));
    pump(a, 3);
    const int glyphsDuring = glyphs();
    CHECK(ui.compositionCaret() == i32(kHan.size())); // caret after the syllable
    CHECK(glyphsDuring >= glyphs0 + 1);                // drawn inline
    std::printf("    glyphs %d -> %d with the pre-edit\n", glyphs0, glyphsDuring);
    CHECK(step('r', 0, kGiyeok, "ab" + kHan, "r (commits)"));
    step('m', 0, kGeu, "ab" + kHan, "m");
    CHECK(step('f', 0, kGeul, "ab" + kHan, "f"));
    CHECK(step(0xff0d, 0, "", "ab" + kHan + kGeul, "Return (commits)"));
    // cancel in the IM: the last jamo backspaced away, nothing typed
    CHECK(step('x', 0, kTieut, "ab" + kHan + kGeul, "x"));
    CHECK(step(0xff08, 0, "", "ab" + kHan + kGeul, "BackSpace (cancels)"));
    // cancel by focus: the pre-edit is dropped (the IM is reset), nothing typed
    step('a', 0, kMieum, "ab" + kHan + kGeul, "a");
    CHECK(step('k', 0, kMa, "ab" + kHan + kGeul, "k"));
    ui.setFocus(wnd.other);
    CHECK(until(a, [&] { return !native.textInputActive(*wnd.win); }));
    pump(a, 10);
    std::printf("    focus to the button:   pre-edit \"%s\", text \"%s\"\n", std::string(ui.composition()).c_str(), std::string(ui.text(wnd.le)).c_str());
    CHECK(ui.composition().empty() && ui.text(wnd.le) == "ab" + kHan + kGeul);
    ui.setFocus(wnd.le);
    CHECK(until(a, [&] { return native.textInputActive(*wnd.win); }));
    pump(a, 5);
    CHECK(step('k', 0, kA, "ab" + kHan + kGeul, "k (fresh syllable)")); // not a continuation of 마
    CHECK(step(0xff08, 0, "", "ab" + kHan + kGeul, "BackSpace"));
    o.tap(a, target, ' ', 1); // Byeoru off
    pump(a, 10);
    CHECK(step('z', 0, "", "ab" + kHan + kGeul + "z", "Shift+Space, z (direct)"));
    CHECK(failedSteps == 0);
    return 0;
}
#endif

// ---- clipboard_roundtrip -----------------------------------------------------------------------------------------------
static int gateClipboard(int argc, char** argv) {
    app::Application a{{.name = "gate_clipboard", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    if (!native.available()) { std::printf("fuse_platform_clipboard_roundtrip: skipped (%.*s)\n", int(native.lastError().size()), native.lastError().data()); return 77; }
    Window1 wnd = makeWindow(a, native, "fuse clipboard"); // the pump serves the clipboard while the window lives
    (void)wnd;                                            // held only for its lifetime
    const std::string text = "Clipboard \xC3\xA9\xE2\x82\xAC \xF0\x9F\x98\x80 line\nline 2";
    const Image img = testImage(37, 23, false);
    const std::string mime = "application/x-fuse-gate";
    const std::vector<std::uint8_t> blob = {0, 1, 2, 0xFF, 0x80, 'f', 'u', 's', 'e', 0};
    std::string s;
    Image back;
    std::vector<std::uint8_t> bytes;
    // 1. text out and in
    CHECK(app::setClipboardText(text) && app::clipboardOwned());
#if defined(_WIN32)
    std::string seen;
    asOther(a, [&] { seen = rawText(); });
    CHECK(seen == text);
    asOther(a, [&] { rawSetText("from another client"); });
#else
    Other o;
    CHECK(o.open());
    if (!o.d) return 1;
    std::vector<std::uint8_t> got;
    asOther(a, [&] { o.read(o.utf8, got); });
    CHECK(got == bytesOf(text));
    asOther(a, [&] { o.read(o.x.InternAtom(o.d, "text/plain;charset=utf-8", 0), got); });
    CHECK(got == bytesOf(text));
    otherOwns(o, {{o.utf8, bytesOf("from another client")}}, [&] { CHECK(app::clipboardText(s) && s == "from another client"); });
    s.clear();
#endif
#if defined(_WIN32)
    CHECK(!app::clipboardOwned() && app::clipboardText(s) && s == "from another client");
#endif
    std::printf("  text: out \"%s\" read back by the other client, in \"%s\"\n", "Clipboard é€ 😀 ...", "from another client");
    // 2. RGBA image (straight alpha) both ways
    CHECK(app::setClipboardImage(img));
    CHECK(app::clipboardImage(back) && sameImage(back, img));
#if defined(_WIN32)
    Image other;
    asOther(a, [&] { rawImage(other); });
    CHECK(sameImage(other, img));
    const Image opaque = testImage(19, 11, true);
    asOther(a, [&] { rawSetDib24(opaque); });
    CHECK(app::clipboardImage(back) && sameImage(back, opaque));
    std::printf("  image: 37x23 RGBA as CF_DIBV5 read by the other client %s; a 24-bit CF_DIB from it read back %s\n",
                sameImage(other, img) ? "exact" : "DIFFERENT", sameImage(back, opaque) ? "exact" : "DIFFERENT");
#else
    asOther(a, [&] { o.read(o.png, got); });
    Image other;
    CHECK(decodePng(got, other) && sameImage(other, img));
    const Image opaque = testImage(19, 11, true);
    otherOwns(o, {{o.png, encodePng(opaque)}}, [&] { CHECK(app::clipboardImage(back) && sameImage(back, opaque)); });
    std::printf("  image: 37x23 RGBA as image/png read by the other client %s; its PNG read back %s\n",
                sameImage(other, img) ? "exact" : "DIFFERENT", sameImage(back, opaque) ? "exact" : "DIFFERENT");
#endif
    // 3. custom MIME type, and several formats at once
    std::vector<app::ClipboardItem> items(3);
    items[0].mime = std::string(app::kClipboardText);
    items[0].text = "with a blob";
    items[1].mime = std::string(app::kClipboardImage);
    items[1].image = img;
    items[2].mime = mime;
    items[2].bytes = blob;
    CHECK(app::setClipboard(std::move(items)));
    const std::vector<std::string> fmts = app::clipboardFormats();
    std::printf("  formats offered:");
    for (const std::string& f : fmts) std::printf(" %s", f.c_str());
    std::printf("\n");
    CHECK(hasFormat(fmts, app::kClipboardText) && hasFormat(fmts, app::kClipboardImage) && hasFormat(fmts, mime));
    CHECK(app::clipboardData(mime, bytes) && bytes == blob);
#if defined(_WIN32)
    std::vector<std::uint8_t> raw;
    asOther(a, [&] { raw = rawBytes(customFormat(mime)); seen = rawText(); });
    CHECK(raw == blob && seen == "with a blob");
    const std::vector<std::uint8_t> blob2 = {9, 8, 7};
    asOther(a, [&] {
        if (!rawOpen()) return;
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, blob2.size());
        std::memcpy(GlobalLock(g), blob2.data(), blob2.size());
        GlobalUnlock(g);
        SetClipboardData(customFormat(mime), g);
        CloseClipboard();
    });
    CHECK(app::clipboardData(mime, bytes) && bytes == blob2 && hasFormat(app::clipboardFormats(), mime));
#else
    std::vector<std::uint8_t> tl;
    asOther(a, [&] { o.read(o.targets, tl); o.read(o.x.InternAtom(o.d, mime.c_str(), 0), got); });
    int listed = 0;
    for (std::size_t i = 0; i + sizeof(long) <= tl.size(); i += sizeof(long)) {
        long at = 0;
        std::memcpy(&at, tl.data() + i, sizeof at);
        listed += x11::Atom(at) == o.x.InternAtom(o.d, mime.c_str(), 0) || x11::Atom(at) == o.png || x11::Atom(at) == o.utf8;
    }
    CHECK(listed == 3 && got == blob);
    const std::vector<std::uint8_t> blob2 = {9, 8, 7};
    otherOwns(o, {{o.x.InternAtom(o.d, mime.c_str(), 0), blob2}, {o.utf8, bytesOf("t")}}, [&] {
        CHECK(app::clipboardData(mime, bytes) && bytes == blob2);
        const auto f2 = app::clipboardFormats();
        CHECK(hasFormat(f2, mime) && hasFormat(f2, app::kClipboardText));
    });
#endif
    std::printf("  custom %s: %zu bytes out and back in\n", mime.c_str(), blob.size());
    // 4. lazy provision: nothing is made until a paste asks, then once
    int textCalls = 0, blobCalls = 0;
    std::vector<app::ClipboardItem> lazy(2);
    lazy[0].mime = std::string(app::kClipboardText);
    lazy[0].provide = [&textCalls](app::ClipboardItem& it) { ++textCalls; it.text = "made on demand"; return true; };
    lazy[1].mime = mime;
    lazy[1].provide = [&blobCalls, &blob](app::ClipboardItem& it) { ++blobCalls; it.bytes = blob; return true; };
    CHECK(app::setClipboard(std::move(lazy)));
    CHECK(textCalls == 0 && blobCalls == 0); // announced only (no events handled yet, so nobody could ask)
#if defined(_WIN32)
    // clipboard monitors (history, cloud clipboard) may paste as soon as we pump: still made at most once
    asOther(a, [&] { raw = rawBytes(customFormat(mime)); });
    CHECK(raw == blob && blobCalls == 1);
    asOther(a, [&] { seen = rawText(); raw = rawBytes(customFormat(mime)); });
    CHECK(seen == "made on demand" && textCalls == 1 && blobCalls == 1 && raw == blob);
#else
    asOther(a, [&] { o.read(o.x.InternAtom(o.d, mime.c_str(), 0), got); });
    CHECK(got == blob && blobCalls == 1 && textCalls == 0);
    asOther(a, [&] { o.read(o.utf8, got); o.read(o.x.InternAtom(o.d, mime.c_str(), 0), tl); });
    CHECK(got == bytesOf("made on demand") && textCalls == 1 && blobCalls == 1);
#endif
    std::printf("  lazy: provider calls on set 0, after two pastes each text %d / blob %d\n", textCalls, blobCalls);
#if !defined(_WIN32)
    // 5. a large image: INCR transfers both ways
    const Image big = testImage(700, 600, true);
    CHECK(app::setClipboardImage(big));
    bool incr = false;
    asOther(a, [&] { o.read(o.png, got, &incr); });
    Image bigBack;
    CHECK(incr && decodePng(got, bigBack) && sameImage(bigBack, big));
    std::printf("  large image 700x600 (%zu PNG bytes) via INCR: %s\n", got.size(), incr && sameImage(bigBack, big) ? "exact" : "FAILED");
#endif
    return 0;
}

int main(int argc, char** argv) {
    const std::string gate = FUSE_PLATFORM_TEXT_GATE;
    int rc = 0;
    if (gate == "text_input") rc = gateTextInput(argc, argv);
#if !defined(_WIN32)
    else if (gate == "ime_composition" && argc > 1 && std::strcmp(argv[1], "--server-im") == 0) return gateImeServer(1, argv) || g_fail ? 1 : 0;
#endif
    else if (gate == "ime_composition") rc = gateIme(argc, argv);
    else if (gate == "clipboard_roundtrip") rc = gateClipboard(argc, argv);
    else { std::fprintf(stderr, "unknown gate %s\n", gate.c_str()); return 2; }
    if (rc == 77) return 77;
    if (g_fail || rc) {
        std::fprintf(stderr, "fuse_platform_%s: FAILED (%d checks)\n", gate.c_str(), g_fail);
        return 1;
    }
    std::printf("fuse_platform_%s: OK\n", gate.c_str());
    return 0;
}
