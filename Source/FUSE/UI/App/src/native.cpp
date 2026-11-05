// fuse_app native windows: X11 (libX11 loaded at runtime, so neither X11 headers nor libX11 are needed to build) and
// Win32, plus the presenter selection (Vulkan swapchain -> GDI on Win32 -> headless). See native.hpp.
#include "native_internal.hpp"
#include "platform_internal.hpp"
#include "fuse/ui/platform_bridge.hpp"
#include "chrome_internal.hpp"
#include "x11_internal.hpp"
#include "fuse/app/chrome.hpp"
#include "fuse/app/clipboard.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <imm.h>
#else
#include <clocale>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#if defined(FUSE_APP_HAS_VULKAN)
#define FUSE_APP_ASYNC_VK true // async init only when there is a Vulkan presenter to wait for
#else
#define FUSE_APP_ASYNC_VK false
#endif

namespace fuse::app {
using namespace fuse::ui;
using detail::SurfaceSource;

#if !defined(FUSE_APP_HAS_VULKAN)
// Built without a Vulkan loader: no swapchain presenter.
namespace detail {
struct VkCtx {};
struct VkWin {};
VkCtx* vkCtxCreate(SurfaceSource::Kind, std::string& err) { err = "built without Vulkan"; return nullptr; }
VkCtx* vkCtxAdopt(SurfaceSource::Kind, const VulkanDeviceHandles&, std::string& err) { err = "built without Vulkan"; return nullptr; }
void vkCtxDestroy(VkCtx*) {}
bool vkWinSetExternal(VkWin*, unsigned, const ExternalTexture&) { return false; }
const char* vkCtxDeviceName(const VkCtx*) { return ""; }
VkWin* vkWinCreate(VkCtx*, const SurfaceSource&, const NativeOptions&, std::string&) { return nullptr; }
bool vkWinPrepare(VkWin*, unsigned, unsigned, std::string&) { return false; }
bool vkWinPresent(VkWin*, AppWindow&, Rect) { return false; }
bool vkWinGpuDraws(const VkWin*) { return false; }
void vkWinDestroy(VkWin*) {}
bool vkWinReadback(VkWin*, Image&) { return false; }
} // namespace detail
#endif

namespace {
u8 asciiKey(Key& k, u32 c) {
    switch (c) {
    case 'a': case 'A': k = Key::A; return 1;
    case 'c': case 'C': k = Key::C; return 1;
    case 'v': case 'V': k = Key::V; return 1;
    case 'x': case 'X': k = Key::X; return 1;
    case 'y': case 'Y': k = Key::Y; return 1;
    case 'z': case 'Z': k = Key::Z; return 1;
    case 'w': case 'W': k = Key::W; return 1;
    case 's': case 'S': k = Key::S; return 1;
    case 'd': case 'D': k = Key::D; return 1;
    case 'q': case 'Q': k = Key::Q; return 1;
    case 'e': case 'E': k = Key::E; return 1;
    case 'n': case 'N': k = Key::N; return 1;
    case 'o': case 'O': k = Key::O; return 1;
    case ' ': k = Key::Space; return 1;
    case '1': k = Key::Num1; return 1;
    case '2': k = Key::Num2; return 1;
    case '3': k = Key::Num3; return 1;
    case '4': k = Key::Num4; return 1;
    case 'b': case 'B': k = Key::B; return 1;
    case 'f': case 'F': k = Key::F; return 1;
    case 'g': case 'G': k = Key::G; return 1;
    case 'h': case 'H': k = Key::H; return 1;
    case 'i': case 'I': k = Key::I; return 1;
    case 'j': case 'J': k = Key::J; return 1;
    case 'k': case 'K': k = Key::K; return 1;
    case 'l': case 'L': k = Key::L; return 1;
    case 'm': case 'M': k = Key::M; return 1;
    case 'p': case 'P': k = Key::P; return 1;
    case 'r': case 'R': k = Key::R; return 1;
    case 't': case 'T': k = Key::T; return 1;
    case 'u': case 'U': k = Key::U; return 1;
    case '0': k = Key::Num0; return 1;
    case '5': k = Key::Num5; return 1;
    case '6': k = Key::Num6; return 1;
    case '7': k = Key::Num7; return 1;
    case '8': k = Key::Num8; return 1;
    case '9': k = Key::Num9; return 1;
    case '[': k = Key::BracketLeft; return 1;
    case ']': k = Key::BracketRight; return 1;
    default: return 0;
    }
}
void appendUtf8(std::string& s, u32 cp) {
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    else { s += char(0xF0 | (cp >> 18)); s += char(0x80 | ((cp >> 12) & 0x3F)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
}
} // namespace

#if !defined(_WIN32)
// ---- X11 (runtime-loaded) ----------------------------------------------------------------------------------------
namespace x11 {
// main windows: keys, buttons, motion, exposure, configure / map, focus, properties (_NET_WM_STATE)
constexpr long kMask = kKeyPressMask | kKeyReleaseMask | kButtonPressMask | kButtonReleaseMask | kPointerMotionMask |
                       kExposureMask | kStructureNotifyMask | kFocusChangeMask | kPropertyChangeMask;
constexpr long kPopupMask = kButtonPressMask | kButtonReleaseMask | kPointerMotionMask | kExposureMask;
int ignoreErrors(Display*, void*) { return 0; } // a capture of an unmapped window must not abort the process

Key mapKey(KeySym s, bool& ascii) {
    ascii = false;
    switch (s) {
    case 0xff09: case 0xfe20: return Key::Tab;
    case 0xff0d: case 0xff8d: return Key::Enter;
    case 0xff1b: return Key::Escape;
    case 0xff08: return Key::Backspace;
    case 0xffff: return Key::Delete;
    case 0xff51: return Key::Left;
    case 0xff52: return Key::Up;
    case 0xff53: return Key::Right;
    case 0xff54: return Key::Down;
    case 0xff50: return Key::Home;
    case 0xff57: return Key::End;
    case 0xff55: return Key::PageUp;
    case 0xff56: return Key::PageDown;
    case 0xffbf: return Key::F2;
    case 0xffc2: return Key::F5;
    case 0xffc7: return Key::F10;
    case 0xffe9: case 0xffea: return Key::Alt; // Alt_L / Alt_R (not ISO_Level3_Shift: AltGr)
    case 0xff63: return Key::Insert;
    case 0xffbe: return Key::F1;
    case 0xffc0: return Key::F3;
    case 0xffc1: return Key::F4;
    case 0xffc3: return Key::F6;
    case 0xffc4: return Key::F7;
    case 0xffc5: return Key::F8;
    case 0xffc6: return Key::F9;
    case 0xffc8: return Key::F11;
    case 0xffc9: return Key::F12;
    default: break;
    }
    Key k = Key::Other;
    ascii = s < 0x80 && asciiKey(k, u32(s));
    return k;
}
u8 mods(unsigned state) {
    return u8(((state & 1) ? kModShift : 0) | ((state & 4) ? kModCtrl : 0) | ((state & 8) ? kModAlt : 0) |
              ((state & 64) ? kModSuper : 0));
}
// XIM (Xlib.h): input styles, lookup status, callback structs
using XimStyle = unsigned long;
constexpr XimStyle kPreeditCallbacks = 0x0002, kPreeditPosition = 0x0004, kPreeditNothing = 0x0008, kPreeditNone = 0x0010,
                   kStatusNothing = 0x0400, kStatusNone = 0x0800;
constexpr int kLookupChars = 2, kLookupBoth = 4, kBufferOverflow = -1;
struct XimStyles { unsigned short count; XimStyle* styles; };
struct XimCallback { char* client; void (*fn)(void* ic, char* client, void* call); };
struct XimText { unsigned short length; unsigned long* feedback; int isWchar; union { char* mb; wchar_t* wc; } s; };
struct XimDraw { int caret, chgFirst, chgLength; XimText* text; };
struct XimCaret { int position; int direction; int style; };
struct XPoint { short x, y; };
} // namespace x11
#endif

// ---- shared ----------------------------------------------------------------------------------------------------------
struct NativePlatform::Impl {
    struct Rec {
        Impl* impl = nullptr;
        AppWindow* win = nullptr;
        detail::VkWin* vk = nullptr;
        PresenterKind kind = PresenterKind::Headless;
        unsigned long long handle = 0; ///< X11 Window / HWND
        bool mapped = false;
        bool focused = false;
        struct Popup {
            unsigned long long handle = 0;
            Rect screen;   ///< physical screen rect
            Rect rect;     ///< context rect (input offset)
            u64 revision = ~0ull;
            Image img;
            std::vector<u32> staged; ///< OS pixel order
        };
        std::vector<Popup> popups;
        detail::ServiceState svc; ///< cursor / title bar (platform services)
        MessageHook hook{};
        std::vector<u32> staged; ///< software presenter staging (OS pixel order)
        // asyncVulkan: the worker creating this window's Vulkan presenter, and what it produced
        std::thread worker;
        std::atomic<bool> vkDone{false};
        detail::VkCtx* vkCtxNew = nullptr;
        detail::VkWin* vkNew = nullptr;
        std::string vkErr;
        bool vkPending = false; ///< worker running or result not yet taken over
        bool vkWanted = false;  ///< waits for another window's worker, then gets Vulkan on the UI thread
        // text-input session (IME): on while a text field has focus; caret area given to the IME (device pixels)
        bool imeOn = false, imeKnown = false;
        Rect imeArea{-1.f, -1.f, 0.f, 0.f};
#if defined(_WIN32)
        wchar_t highSurrogate = 0;
#else
        void* xic = nullptr;     ///< XIM input context
        std::u32string preedit;  ///< XIM pre-edit (preedit callbacks)
        int preeditCaret = 0;    ///< in code points
        x11::XimCallback cbStart{}, cbDone{}, cbDraw{}, cbCaret{};
        x11::Display* vkDpy = nullptr; ///< the worker's own X connection (Xlib is not thread-safe without XInitThreads)
        CursorShape edgeCursor = CursorShape::Auto; ///< custom chrome: resize cursor over a frame edge
        unsigned long captionPress = 0;             ///< time of the last caption press (double click maximises)
#endif
    };

    Application& app;
    NativeOptions opts;
    std::vector<Rec*> recs;
    detail::VkCtx* vk = nullptr;
    bool vkTried = false;
    std::string error;
    EventSource previous{};
    NativeHooks hooks{};
#if defined(_WIN32)
    HINSTANCE hinst = nullptr;
    DWORD uiThread = 0; ///< wake() posts WM_NULL here
#else
    x11::Lib x;
    x11::Display* dpy = nullptr;
    x11::Atom wmProtocols = 0, wmDelete = 0;
    unsigned long cursors[std::size_t(CursorShape::Count)]{}; ///< theme / font cursors, created on first use
    x11::Xcursor xc;          ///< libXcursor (themed cursors at the DPI-scaled size); absent: font cursors
    bool xcOk = false;
    int cursorSize = 0;       ///< device px size the theme cursors are loaded at (0: font cursors)
    std::string cursorTheme;  ///< theme they come from ("" = Xcursor's default)
    f32 xScale = 1.f; ///< $FUSE_UI_SCALE, XSETTINGS Xft/DPI or RESOURCE_MANAGER Xft.dpi, / 96
    x11::Randr xr;
    bool xrOk = false;
    x11::Window root = 0;
    x11::Atom netWmState = 0, netMaxV = 0, netMaxH = 0, netMoveResize = 0, wmChangeState = 0, motifHints = 0, netWorkarea = 0,
              resourceManager = 0, manager = 0, xsSelection = 0, xsSettings = 0;
    x11::Window xsOwner = 0;  ///< XSETTINGS manager window (0: none)
    int xsDpi = 0;            ///< Xft/DPI * 1024 (0: not set)
    std::string xsTheme;      ///< Net/ThemeName
    int xsCursorSize = 0;     ///< Gtk/CursorThemeSize (logical px; 0: not set)
    std::string xsCursorTheme; ///< Gtk/CursorThemeName
    int wakeFd[2] = {-1, -1}; ///< self-pipe: wake() from other threads
    void* xim = nullptr;      ///< XIM input method (XOpenIM), null without one
    x11::XimStyle ximStyle = 0;
    void openXim();
    void createIc(Rec& r);
    static void publishPreedit(Rec& r);
    void readXSettings();     ///< (re)reads the manager's settings (and follows a new manager window)
    void settingsChanged();   ///< re-reads them and applies DPI / theme changes
    void updateCursorTheme(); ///< cursor size / theme from XSETTINGS + DPI; reloads the cursors when they change
    void x11Event(x11::Event& ev);
    void startMoveResize(Rec& r, HitArea a, const x11::InputEv& in);
    std::vector<DisplayInfo> x11Displays() const;
    static void x11Command(void* user, AppWindow& w, WindowCommand c);
#endif
    SystemTheme theme() const;

    Impl(Application& a, const NativeOptions& o) : app(a), opts(o) {}

    // Vulkan presenter creation: synchronous, or on a worker while the window shows software frames (asyncVulkan)
    bool vkInFlight = false; ///< a worker owns instance / device creation right now
    SurfaceSource surfaceOf(const Rec& r) const;
    void createVkSync(Rec& r);
    void startVkAsync(Rec& r);
    /// UI thread: takes over finished workers (switches their windows to Vulkan) and serves vkWanted windows.
    void pollVk();
    /// Blocks until `r`'s worker is done (keeps answering the OS's cross-thread window messages meanwhile).
    void waitVk(Rec& r);
    void takeOverVk(Rec& r);
    /// Software presenter: the window's CPU raster (AppWindow::image) to the OS window (GDI / XPutImage).
    void blitSoftware(Rec& r);
    bool anyVkPending() const {
        for (const Rec* r : recs)
            if (r->vkPending) return true;
        return false;
    }

    Rec* find(unsigned long long h) {
        for (Rec* r : recs)
            if (r->handle == h) return r;
        return nullptr;
    }
    const Rec* find(const AppWindow& w) const {
        for (const Rec* r : recs)
            if (r->win == &w) return r;
        return nullptr;
    }
    detail::VkCtx* vkContext() {
        if (!vkTried && opts.vulkan) {
            vkTried = true;
#if defined(_WIN32)
            const auto kind = SurfaceSource::Win32;
#else
            const auto kind = SurfaceSource::Xlib;
#endif
            vk = opts.adoptDevice.instance && opts.adoptDevice.device ? detail::vkCtxAdopt(kind, opts.adoptDevice, error)
                                                                      : detail::vkCtxCreate(kind, error);
        }
        return vk;
    }

    static bool present(void* user, AppWindow& w, Rect damage);
    static void destroy(void* user);
    static bool pump(void* user, Application& app, f32 wait);
    void destroyOsWindow(Rec& r);
    bool createOsWindow(Rec& r);
    bool captureOs(const Rec& r, Image& out);
    // OS-window popups
    void syncPopups(Rec& r);
    /// An outside click (grabbed pointer outside every window of the app, or the app deactivated): close every popup.
    static void closeAllPopups(Rec& r) {
        UiContext& ui = r.win->ui();
        std::vector<NodeHandle> nodes;
        for (const auto& ps : ui.popupSurfaces()) nodes.push_back(ps.node);
        for (NodeHandle n : nodes)
            if (ui.alive(n)) fuse::ui::widgets::closePopup(ui, n);
        r.win->requestRedraw();
    }
    void grabPointer(Rec& r, bool on);
    bool screenInfo(const Rec& r, int& ox, int& oy, Rect& work) const; ///< client origin + work area (screen px)
    bool createPopupOs(Rec& r, Rec::Popup& p);
    void movePopupOs(Rec::Popup& p);
    void destroyPopupOs(Rec::Popup& p);
    void blitPopupOs(Rec::Popup& p);
    bool capturePopupOs(const Rec::Popup& p, Image& out);
    bool postClickOs(unsigned long long h, Vec2 at);
    // platform services
    void syncCursor(Rec& r);           ///< shows ui.cursor() when it changed
    /// Text-input session from the UI (each present): IME on / off with text-field focus, caret area for the IME.
    void syncTextInput(Rec& r);
    void applyScale(Rec& r, f32 scale, const void* suggested); ///< DPI change (Win32: suggested RECT*)
    void updateTitleBars();
    void themeChanged() { app.setSystemTheme(theme()); updateTitleBars(); }
#if defined(_WIN32)
    bool serviceMessage(Rec& r, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& res);
#else
    bool captureX(unsigned long win, unsigned w, unsigned h, Image& out);
    f32 readXScale();
#endif
    Rec* findPopup(unsigned long long h, std::size_t& idx) {
        for (Rec* r : recs)
            for (std::size_t i = 0; i < r->popups.size(); ++i)
                if (r->popups[i].handle == h) { idx = i; return r; }
        return nullptr;
    }
};

void NativePlatform::Impl::syncPopups(Rec& r) {
    if (!opts.osPopups || !r.handle) return;
    UiContext& ui = r.win->ui();
    int ox = 0, oy = 0;
    Rect work;
    if (screenInfo(r, ox, oy, work)) {
        const f32 s = r.win->scale(); // context px = physical / scale
        ui.setOsPopups(true, {(work.x - f32(ox)) / s, (work.y - f32(oy)) / s, work.w / s, work.h / s});
    }
    const auto& surf = ui.popupSurfaces();
    const bool had = !r.popups.empty();
    while (r.popups.size() > surf.size()) {
        destroyPopupOs(r.popups.back());
        r.popups.pop_back();
    }
    if (had && r.popups.empty()) grabPointer(r, false);
    const f32 s = r.win->scale();
    for (std::size_t i = 0; i < surf.size(); ++i) {
        if (i == r.popups.size()) r.popups.emplace_back();
        Rec::Popup& p = r.popups[i];
        const auto& ps = surf[i];
        const Rect scr{std::round(f32(ox) + ps.rect.x * s), std::round(f32(oy) + ps.rect.y * s), std::max(1.f, std::ceil(ps.rect.w * s)),
                       std::max(1.f, std::ceil(ps.rect.h * s))};
        const bool moved = scr.x != p.screen.x || scr.y != p.screen.y || scr.w != p.screen.w || scr.h != p.screen.h;
        p.rect = ps.rect;
        p.screen = scr;
        if (!p.handle) {
            if (!createPopupOs(r, p)) { r.popups.pop_back(); return; }
            if (i == 0) grabPointer(r, true); // Qt::Popup: clicks anywhere else on screen close the menu
        } else if (moved) {
            movePopupOs(p);
        }
        if (moved || p.revision != ps.revision) {
            p.revision = ps.revision;
            u32 clear = 0xFF000000u; // the panel's own fill shows in the corners its radius leaves
            for (const DrawCmd& c : ps.draw.cmds())
                if (c.op != DrawOp::Shadow && c.fill.a > 0.9f) { clear = packPremultiplied(c.fill) | 0xFF000000u; break; }
            p.img.resize(int(scr.w), int(scr.h), clear);
            CpuRasterizer rz = r.win->rasterizer(); // the window's images + the current glyph atlas
            rz.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
            rz.render(ps.draw, p.img); // popup lists are already in device pixels (UiContext::scale)
            blitPopupOs(p);
        }
    }
}

bool NativePlatform::Impl::present(void* user, AppWindow& w, Rect damage) {
    Rec& r = *static_cast<Rec*>(user);
    // a finished async Vulkan init switches presenters here, so this frame (the whole current UI) is the first on Vulkan
    if (r.vkPending && r.vkDone.load(std::memory_order_acquire)) r.impl->takeOverVk(r);
    bool ok = true;
    switch (r.kind) {
    case PresenterKind::Vulkan: ok = detail::vkWinPresent(r.vk, w, damage); break;
    case PresenterKind::Software:
        w.rasterize(damage);
        r.impl->blitSoftware(r);
        break;
    default: w.rasterize(damage); break;
    }
    r.impl->syncPopups(r);
    // after the frame: the first IME (dis)association loads the input stack, which must not delay the first frame
    r.impl->syncTextInput(r);
    return ok;
}

void NativePlatform::Impl::destroy(void* user) {
    Rec* r = static_cast<Rec*>(user);
    Impl& m = *r->impl;
    if (r->vkPending) {
        m.waitVk(*r); // the worker uses the window: let it finish, keep its device for the platform, drop its swapchain
        if (r->vkCtxNew && !m.vk) {
            m.vk = r->vkCtxNew;
            m.vkTried = true;
        }
        detail::vkWinDestroy(r->vkNew);
        r->vkNew = nullptr;
        r->vkPending = false;
        m.vkInFlight = false;
    }
    detail::vkWinDestroy(r->vk);
    r->vk = nullptr;
#if !defined(_WIN32)
    if (r->vkDpy) m.x.CloseDisplay(r->vkDpy);
    r->vkDpy = nullptr;
#endif
    for (Rec::Popup& p : r->popups) m.destroyPopupOs(p);
    r->popups.clear();
    if (r->win) r->win->ui().setOsPopups(false);
    m.destroyOsWindow(*r);
    std::erase(m.recs, r);
    delete r;
}

#if defined(_WIN32)
// ---- Win32 -----------------------------------------------------------------------------------------------------------
namespace {
constexpr const wchar_t* kClass = L"FuseAppWindow";
Key mapVk(WPARAM vk, bool& ascii) {
    ascii = false;
    switch (vk) {
    case VK_TAB: return Key::Tab;
    case VK_RETURN: return Key::Enter;
    case VK_ESCAPE: return Key::Escape;
    case VK_SPACE: return Key::Space;
    case VK_BACK: return Key::Backspace;
    case VK_DELETE: return Key::Delete;
    case VK_LEFT: return Key::Left;
    case VK_RIGHT: return Key::Right;
    case VK_UP: return Key::Up;
    case VK_DOWN: return Key::Down;
    case VK_HOME: return Key::Home;
    case VK_END: return Key::End;
    case VK_PRIOR: return Key::PageUp;
    case VK_NEXT: return Key::PageDown;
    case VK_F2: return Key::F2;
    case VK_F5: return Key::F5;
    case VK_F10: return Key::F10;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return Key::Alt;
    case VK_INSERT: return Key::Insert;
    case VK_F1: return Key::F1;
    case VK_F3: return Key::F3;
    case VK_F4: return Key::F4;
    case VK_F6: return Key::F6;
    case VK_F7: return Key::F7;
    case VK_F8: return Key::F8;
    case VK_F9: return Key::F9;
    case VK_F11: return Key::F11;
    case VK_F12: return Key::F12;
    case VK_OEM_4: return Key::BracketLeft;
    case VK_OEM_6: return Key::BracketRight;
    default: break;
    }
    Key k = Key::Other;
    ascii = vk < 0x80 && vk != 0x5B && vk != 0x5D && asciiKey(k, u32(vk)); // 0x5B / 0x5D are VK_LWIN / VK_APPS, not [ ]
    return k;
}
u8 winMods() {
    return u8(((GetKeyState(VK_SHIFT) & 0x8000) ? kModShift : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) ? kModCtrl : 0) |
              ((GetKeyState(VK_MENU) & 0x8000) ? kModAlt : 0) |
              (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) ? kModSuper : 0));
}
Vec2 lparamPos(LPARAM lp, f32 s = 1.f) { return {f32(short(LOWORD(lp))) / s, f32(short(HIWORD(lp))) / s}; }
std::string utf8Of(const wchar_t* s, std::size_t n) {
    std::string o(std::size_t(WideCharToMultiByte(CP_UTF8, 0, s, int(n), nullptr, 0, nullptr, nullptr)), '\0');
    if (!o.empty()) WideCharToMultiByte(CP_UTF8, 0, s, int(n), o.data(), int(o.size()), nullptr, nullptr);
    return o;
}
std::wstring imeWide(HIMC imc, DWORD which) { // GCS_COMPSTR / GCS_RESULTSTR
    const LONG bytes = ImmGetCompositionStringW(imc, which, nullptr, 0);
    std::wstring s(std::size_t(std::max(bytes, 0L)) / sizeof(wchar_t), L'\0');
    if (!s.empty()) ImmGetCompositionStringW(imc, which, s.data(), DWORD(s.size() * sizeof(wchar_t)));
    return s;
}
std::string imeString(HIMC imc, DWORD which) {
    const std::wstring s = imeWide(imc, which);
    return utf8Of(s.data(), s.size());
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* r = reinterpret_cast<NativePlatform::Impl::Rec*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!r || !r->win) return DefWindowProcW(hwnd, msg, wp, lp);
    if (std::intptr_t res = 0; r->hook.fn && r->hook.fn(r->hook.user, msg, wp, lp, res)) return LRESULT(res);
    AppWindow& w = *r->win;
    UiContext& ui = w.ui();
    const f32 s = w.scale(); // client pixels -> UI units
    if (std::intptr_t res = 0; detail::chromeWin32Message(hwnd, w, msg, wp, lp, res)) return res; // custom chrome
    if (LRESULT res = 0; r->impl->serviceMessage(*r, hwnd, msg, wp, lp, res)) return res; // DPI, cursor, theme, touch
    switch (msg) {
    case WM_CLOSE: w.requestClose(); return 0;
    case WM_ACTIVATEAPP: // another application took the pointer / focus: popups close (Qt::Popup)
        if (!wp && !r->popups.empty()) NativePlatform::Impl::closeAllPopups(*r);
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        r->mapped = true;
        w.requestRedraw();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (LOWORD(lp) && HIWORD(lp)) w.resizePixels(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        ui.setPointerKind(UiContext::PointerKind::Mouse);
        ui.setModifiers(u8(((wp & MK_SHIFT) ? kModShift : 0) | ((wp & MK_CONTROL) ? kModCtrl : 0) | ((GetKeyState(VK_MENU) < 0) ? kModAlt : 0)));
        break;
    default: break;
    }
    switch (msg) {
    case WM_MOUSEMOVE: ui.pointerMove(lparamPos(lp, s)); r->impl->syncCursor(*r); return 0;
    case WM_LBUTTONDOWN: SetCapture(hwnd); ui.pointerDown(lparamPos(lp, s), MouseButton::Left); return 0;
    case WM_RBUTTONDOWN: ui.pointerDown(lparamPos(lp, s), MouseButton::Right); return 0;
    case WM_MBUTTONDOWN: ui.pointerDown(lparamPos(lp, s), MouseButton::Middle); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); ui.pointerUp(lparamPos(lp, s)); r->impl->syncCursor(*r); return 0;
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: ui.pointerUp(lparamPos(lp, s)); return 0;
    case WM_MOUSEWHEEL: {
        POINT p{short(LOWORD(lp)), short(HIWORD(lp))};
        ScreenToClient(hwnd, &p);
        const WORD ks = GET_KEYSTATE_WPARAM(wp);
        ui.setModifiers(u8(((ks & MK_SHIFT) ? kModShift : 0) | ((ks & MK_CONTROL) ? kModCtrl : 0) | ((GetKeyState(VK_MENU) < 0) ? kModAlt : 0)));
        ui.wheel({f32(p.x) / s, f32(p.y) / s}, {0.f, f32(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA});
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        bool ascii = false;
        const Key k = mapVk(wp, ascii);
        const u8 md = winMods();
        if (k != Key::Other) { // printable keys too: viewport WASD / 1-4; text arrives separately
            if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ui.keyDown(k, md);
            else if (ui.keyUp(k, md) && k == Key::Alt) return 0; // the UI's menu bar took Alt: no window-menu mode
        }
        break;
    }
    case WM_CHAR: {
        const wchar_t c = wchar_t(wp);
        if (c >= 0xD800 && c < 0xDC00) { r->highSurrogate = c; return 0; }
        u32 cp = c;
        if (c >= 0xDC00 && c < 0xE000 && r->highSurrogate) cp = 0x10000u + ((u32(r->highSurrogate) - 0xD800u) << 10) + (c - 0xDC00u);
        r->highSurrogate = 0;
        if (cp < 0x20 || cp == 0x7F || (winMods() & kModCtrl)) return 0;
        std::string s;
        appendUtf8(s, cp);
        ui.textInput(s);
        return 0;
    }
    // IMM32 (TSF IMEs through the compatibility layer): the pre-edit is drawn inline by the focused field
    case WM_IME_SETCONTEXT: return DefWindowProcW(hwnd, msg, wp, lp & ~LPARAM(ISC_SHOWUICOMPOSITIONWINDOW));
    case WM_IME_STARTCOMPOSITION: return 0; // no default composition window
    case WM_IME_COMPOSITION: {
        HIMC imc = ImmGetContext(hwnd);
        if (!imc) break;
        if (lp & GCS_RESULTSTR) { // committed: the pre-edit goes, the result is typed (no WM_IME_CHAR follows)
            const std::string s = imeString(imc, GCS_RESULTSTR);
            if (!ui.composition().empty()) ui.textComposition({}, 0);
            if (!s.empty()) ui.textInput(s);
        }
        if (lp & GCS_COMPSTR) {
            const std::wstring ws = imeWide(imc, GCS_COMPSTR);
            LONG cur = (lp & GCS_CURSORPOS) ? ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0) : LONG(ws.size());
            cur = std::clamp(cur, 0L, LONG(ws.size()));
            ui.textComposition(utf8Of(ws.data(), ws.size()), i32(utf8Of(ws.data(), std::size_t(cur)).size()));
        }
        ImmReleaseContext(hwnd, imc);
        w.requestRedraw();
        return 0;
    }
    case WM_IME_ENDCOMPOSITION:
        if (!ui.composition().empty()) ui.textComposition({}, 0);
        w.requestRedraw();
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
constexpr const wchar_t* kPopupClass = L"FuseAppPopup";
LRESULT CALLBACK popupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* r = reinterpret_cast<NativePlatform::Impl::Rec*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE; // the main window keeps keyboard focus (Qt popup behaviour)
    if (!r || !r->win) return DefWindowProcW(hwnd, msg, wp, lp);
    std::size_t idx = 0;
    if (!r->impl->findPopup(reinterpret_cast<unsigned long long>(hwnd), idx)) return DefWindowProcW(hwnd, msg, wp, lp);
    NativePlatform::Impl::Rec::Popup& p = r->popups[idx];
    UiContext& ui = r->win->ui();
    const f32 s = r->win->scale();
    const Vec2 lp2 = lparamPos(lp);
    const Vec2 pos{p.rect.x + lp2.x / s, p.rect.y + lp2.y / s};
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        r->impl->blitPopupOs(p);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: ui.pointerMove(pos); r->win->requestRedraw(); return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        if (lp2.x < 0.f || lp2.y < 0.f || lp2.x >= p.screen.w || lp2.y >= p.screen.h) { // captured click elsewhere
            NativePlatform::Impl::closeAllPopups(*r);
            return 0;
        }
        if (msg == WM_LBUTTONDOWN) ui.pointerDown(pos, MouseButton::Left);
        else ui.pointerDown(pos, MouseButton::Right);
        r->win->requestRedraw();
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: ui.pointerUp(pos); r->win->requestRedraw(); return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

bool NativePlatform::Impl::screenInfo(const Rec& r, int& ox, int& oy, Rect& work) const {
    HWND hwnd = reinterpret_cast<HWND>(r.handle);
    POINT o{0, 0};
    if (!ClientToScreen(hwnd, &o)) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    ox = o.x;
    oy = o.y;
    work = {f32(mi.rcWork.left), f32(mi.rcWork.top), f32(mi.rcWork.right - mi.rcWork.left), f32(mi.rcWork.bottom - mi.rcWork.top)};
    return true;
}
void NativePlatform::Impl::grabPointer(Rec&, bool) {} // Win32: WM_ACTIVATEAPP closes popups instead of a grab
bool NativePlatform::Impl::createPopupOs(Rec& r, Rec::Popup& p) {
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kPopupClass, L"", WS_POPUP, int(p.screen.x),
                                int(p.screen.y), int(p.screen.w), int(p.screen.h), reinterpret_cast<HWND>(r.handle), nullptr, hinst, nullptr);
    if (!hwnd) return false;
    p.handle = reinterpret_cast<unsigned long long>(hwnd);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&r));
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return true;
}
void NativePlatform::Impl::movePopupOs(Rec::Popup& p) {
    SetWindowPos(reinterpret_cast<HWND>(p.handle), nullptr, int(p.screen.x), int(p.screen.y), int(p.screen.w), int(p.screen.h),
                 SWP_NOACTIVATE | SWP_NOZORDER);
}
void NativePlatform::Impl::destroyPopupOs(Rec::Popup& p) {
    if (!p.handle) return;
    HWND hwnd = reinterpret_cast<HWND>(p.handle);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    p.handle = 0;
}
void NativePlatform::Impl::blitPopupOs(Rec::Popup& p) {
    if (!p.handle || p.img.pixels.empty()) return;
    p.staged.resize(p.img.pixels.size());
    for (std::size_t i = 0; i < p.img.pixels.size(); ++i) {
        const u32 q = p.img.pixels[i];
        p.staged[i] = (q & 0xFF00FF00u) | ((q & 0xFFu) << 16) | ((q >> 16) & 0xFFu);
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = p.img.width;
    bi.bmiHeader.biHeight = -p.img.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HWND hwnd = reinterpret_cast<HWND>(p.handle);
    HDC dc = GetDC(hwnd);
    SetDIBitsToDevice(dc, 0, 0, DWORD(p.img.width), DWORD(p.img.height), 0, 0, 0, UINT(p.img.height), p.staged.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(hwnd, dc);
}
bool NativePlatform::Impl::postClickOs(unsigned long long h, Vec2 at) {
    HWND hwnd = reinterpret_cast<HWND>(h);
    const LPARAM lp = MAKELPARAM(int(at.x), int(at.y));
    return PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp) && PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp) &&
           PostMessageW(hwnd, WM_LBUTTONUP, 0, lp);
}

bool NativePlatform::Impl::createOsWindow(Rec& r) {
    const AppWindow::PixelSize sz = r.win->pixelSize();
    RECT rc{0, 0, LONG(sz.w), LONG(sz.h)};
    const DWORD style = detail::chromeWin32Style(r.win->chrome().frame); // custom chrome: client = whole window
    if (r.win->chrome().frame == WindowFrame::System) AdjustWindowRect(&rc, style, FALSE);
    const std::string& t = r.win->title();
    std::wstring wt(std::size_t(MultiByteToWideChar(CP_UTF8, 0, t.data(), int(t.size()), nullptr, 0)), L'\0');
    if (!wt.empty()) MultiByteToWideChar(CP_UTF8, 0, t.data(), int(t.size()), wt.data(), int(wt.size()));
    HWND hwnd = CreateWindowExW(0, kClass, wt.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);
    if (!hwnd) {
        error = "CreateWindowExW failed";
        return false;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&r));
    r.handle = reinterpret_cast<unsigned long long>(hwnd);
    detail::chromeWin32Attach(hwnd, *r.win);
    if (opts.dpiAware) { // the monitor's scale; the window keeps its logical size
        const f32 sc = detail::winWindowScale(hwnd);
        if (sc != r.win->scale() || sc != 1.f) applyScale(r, sc, nullptr);
    }
    r.svc.darkTitle = detail::winSetDarkTitleBar(hwnd, !app.followSystemTheme() || app.systemTheme().dark) &&
                      (!app.followSystemTheme() || app.systemTheme().dark);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return true;
}
bool NativePlatform::Impl::serviceMessage(Rec& r, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& res) {
    AppWindow& w = *r.win;
    switch (msg) {
    case WM_DPICHANGED: // per-monitor v2: new DPI in wParam, the suggested window rect in lParam
        if (!opts.dpiAware) return false;
        applyScale(r, f32(LOWORD(wp)) / 96.f, reinterpret_cast<const RECT*>(lp));
        res = 0;
        return true;
    case WM_SETCURSOR: // the widget under the pointer picks the shape (I-beam, hand, resize, ...)
        if (LOWORD(lp) != HTCLIENT) return false;
        detail::winApplyCursor(r.svc, w.ui().cursor());
        res = TRUE;
        return true;
    case WM_POINTERCAPTURECHANGED: w.ui().touchCancel(); return false;
    case WM_SETFOCUS: r.focused = true; return false;
    case WM_KILLFOCUS: r.focused = false; return false;
    default: break;
    }
    if (detail::winIsThemeMessage(msg)) {
        themeChanged(); // DefWindowProc still runs
        return false;
    }
    fuse::platform::EventV2 e;
    if (!opts.touch || !detail::winPointerEvent(hwnd, msg, wp, lp, e)) return false;
    fuse::ui::feed(w.ui(), e, w.scale()); // events_v2 Touch* / Pen* -> UiContext touch / pen input
    res = 0;
    return true;
}
void NativePlatform::Impl::syncCursor(Rec& r) {
    const CursorShape c = r.win->ui().cursor();
    if (c != r.svc.cursor) detail::winApplyCursor(r.svc, c);
}
void NativePlatform::Impl::syncTextInput(Rec& r) {
    HWND hwnd = reinterpret_cast<HWND>(r.handle);
    UiContext& ui = r.win->ui();
    const bool want = ui.wantsTextInput();
    if (!r.imeKnown || want != r.imeOn) { // game keys do not open the IME outside text fields
        ImmAssociateContextEx(hwnd, nullptr, want ? IACE_DEFAULT : 0);
        r.imeOn = want;
        r.imeKnown = true;
        r.imeArea = {-1.f, -1.f, 0.f, 0.f};
    }
    if (!want || !ui.composition().empty()) return; // placed when the composition starts, not moved during it
    const f32 s = r.win->scale();
    const Rect c = ui.textCaret();
    const Rect px{std::round(c.x * s), std::round(c.y * s), std::max(1.f, std::round(c.w * s)), std::max(1.f, std::round(c.h * s))};
    if (px.x == r.imeArea.x && px.y == r.imeArea.y && px.w == r.imeArea.w && px.h == r.imeArea.h) return;
    r.imeArea = px;
    if (HIMC imc = ImmGetContext(hwnd)) { // composition window at the caret, candidates below it (never over it)
        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos = {LONG(px.x), LONG(px.y)};
        ImmSetCompositionWindow(imc, &cf);
        CANDIDATEFORM cand{};
        cand.dwStyle = CFS_EXCLUDE;
        cand.ptCurrentPos = {LONG(px.x), LONG(px.y + px.h)};
        cand.rcArea = {LONG(px.x), LONG(px.y), LONG(px.x + px.w), LONG(px.y + px.h)};
        ImmSetCandidateWindow(imc, &cand);
        ImmReleaseContext(hwnd, imc);
    }
}
void NativePlatform::Impl::applyScale(Rec& r, f32 scale, const void* suggested) {
    HWND hwnd = reinterpret_cast<HWND>(r.handle);
    r.win->setScale(scale); // keeps the logical size; text re-rasterises, layout re-runs
    if (const RECT* rc = static_cast<const RECT*>(suggested)) {
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
    } else { // forced scale (setWindowScale, first show): keep the frame the window has now
        RECT wr{}, cr{};
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        const int ow = int(r.win->pixelSize().w) + (wr.right - wr.left) - cr.right;
        const int oh = int(r.win->pixelSize().h) + (wr.bottom - wr.top) - cr.bottom;
        SetWindowPos(hwnd, nullptr, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RECT cr{}; // WM_SIZE arrived inside SetWindowPos; take the client area the OS settled on
    if (GetClientRect(hwnd, &cr) && cr.right > 0 && cr.bottom > 0) r.win->resizePixels(u32(cr.right), u32(cr.bottom));
    r.win->requestRedraw();
}
void NativePlatform::Impl::updateTitleBars() {
    const bool dark = !app.followSystemTheme() || app.systemTheme().dark; // the dark default theme unless following
    for (Rec* r : recs)
        if (r->handle) r->svc.darkTitle = detail::winSetDarkTitleBar(reinterpret_cast<HWND>(r->handle), dark) && dark;
}
void NativePlatform::Impl::destroyOsWindow(Rec& r) {
    if (!r.handle) return;
    HWND hwnd = reinterpret_cast<HWND>(r.handle);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    if (r.win) detail::chromeWin32Detach(*r.win);
    DestroyWindow(hwnd);
    r.handle = 0;
}
bool NativePlatform::Impl::pump(void* user, Application&, f32 wait) {
    Impl& m = *static_cast<Impl*>(user);
    m.pollVk();
    if (m.anyVkPending()) wait = std::min(wait, 0.004f); // look for the worker's result soon
    MSG msg;
    if (wait > 0.f && !PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE)) {
        const DWORD ms = std::isinf(wait) ? INFINITE : DWORD(std::ceil(double(wait) * 1000.0));
        MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT);
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}
static bool captureHwnd(HWND hwnd, Image& out) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    HDC wdc = GetDC(hwnd);
    HDC mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    const bool okBlt = BitBlt(mdc, 0, 0, w, h, wdc, 0, 0, SRCCOPY) != 0;
    GdiFlush();
    if (okBlt && bits) {
        out.resize(w, h);
        const u32* src = static_cast<const u32*>(bits);
        for (std::size_t i = 0; i < out.pixels.size(); ++i) {
            const u32 p = src[i];
            out.pixels[i] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
        }
    }
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
    return okBlt;
}
bool NativePlatform::Impl::captureOs(const Rec& r, Image& out) {
    // GDI cannot read a Vulkan-presented client area (DWM flip model on Windows; a separate X drawable under Wine), so
    // a Vulkan window reports the frame it presented; the GDI fallback is read from the window DC.
    if (r.vk) return detail::vkWinReadback(r.vk, out);
    return captureHwnd(reinterpret_cast<HWND>(r.handle), out);
}
bool NativePlatform::Impl::capturePopupOs(const Rec::Popup& p, Image& out) {
    return p.handle && captureHwnd(reinterpret_cast<HWND>(p.handle), out);
}

NativePlatform::NativePlatform(Application& app, const NativeOptions& o) : m(std::make_unique<Impl>(app, o)) {
    if (o.dpiAware) detail::winEnableDpiAwareness(); // before the first window
    app.setSystemTheme(detail::queryTheme(o));
    m->hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &wndProc;
    wc.hInstance = m->hinst;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc); // fails harmlessly when a second NativePlatform registers again
    WNDCLASSEXW pc = wc;
    pc.style = CS_SAVEBITS | CS_DROPSHADOW;
    pc.lpfnWndProc = &popupProc;
    pc.lpszClassName = kPopupClass;
    RegisterClassExW(&pc);
    app.setEventSource({m.get(), &Impl::pump});
    m->uiThread = GetCurrentThreadId();
}
bool NativePlatform::available() const { return true; }
void* NativePlatform::nativeDisplay() const { return m->hinst; }
SystemTheme NativePlatform::Impl::theme() const { return detail::queryTheme(opts); }
void NativePlatform::wake() { PostThreadMessageW(m->uiThread, WM_NULL, 0, 0); }

#else
// ---- X11 platform ------------------------------------------------------------------------------------------------
bool NativePlatform::Impl::createOsWindow(Rec& r) {
    if (opts.dpiAware && xScale != r.win->scale()) r.win->setScale(xScale); // keeps the logical size
    const AppWindow::PixelSize sz = r.win->pixelSize();
    const int scr = x.DefaultScreen(dpy);
    const x11::Window w = x.CreateSimpleWindow(dpy, x.RootWindow(dpy, scr), 0, 0, std::max(1u, sz.w), std::max(1u, sz.h), 0, 0, 0);
    if (!w) {
        error = "XCreateSimpleWindow failed";
        return false;
    }
    x.StoreName(dpy, w, r.win->title().c_str());
    x11::SetWindowAttributes a{};
    a.backing_store = 2; // Always: capture() reads the window even where another one overlaps it (servers may ignore it)
    x.ChangeWindowAttributes(dpy, w, x11::kCWBackingStore, &a);
    x.SelectInput(dpy, w, x11::kMask);
    x.SetWMProtocols(dpy, w, &wmDelete, 1);
    WindowChrome& c = r.win->chrome();
    if (c.frame != WindowFrame::System) {
        // custom chrome: _MOTIF_WM_HINTS { flags = decorations, functions, decorations = none, input mode, status }
        const long hints[5] = {2, 0, 0, 0, 0};
        x.ChangeProperty(dpy, w, motifHints, motifHints, 32, x11::kPropModeReplace, reinterpret_cast<const unsigned char*>(hints), 5);
        c.user = &r;
        c.command = &Impl::x11Command;
    }
    r.handle = w;
    createIc(r);
    x.MapWindow(dpy, w);
    x.Flush(dpy);
    return true;
}
void NativePlatform::Impl::destroyOsWindow(Rec& r) {
    if (r.win && r.win->chrome().command == &Impl::x11Command) {
        r.win->chrome().command = nullptr;
        r.win->chrome().user = nullptr;
    }
    if (r.xic) {
        x.DestroyIC(r.xic);
        r.xic = nullptr;
    }
    if (!r.handle || !dpy) return;
    x.DestroyWindow(dpy, x11::Window(r.handle));
    x.Flush(dpy);
    r.handle = 0;
}
void NativePlatform::Impl::x11Command(void* user, AppWindow& w, WindowCommand c) {
    Rec& r = *static_cast<Rec*>(user);
    Impl& m = *r.impl;
    const x11::Window win = x11::Window(r.handle);
    switch (c) {
    case WindowCommand::Minimize: // XIconifyWindow: WM_CHANGE_STATE IconicState to the root
        m.x.clientMessage(m.dpy, m.root, win, m.wmChangeState, 3);
        break;
    case WindowCommand::ToggleMaximize: // _NET_WM_STATE remove (0) / add (1) both maximised states, source: application
        m.x.clientMessage(m.dpy, m.root, win, m.netWmState, w.chrome().maximized ? 0 : 1, long(m.netMaxV), long(m.netMaxH), 1);
        break;
    case WindowCommand::Close: w.requestClose(); break;
    }
    m.x.Flush(m.dpy);
}
void NativePlatform::Impl::startMoveResize(Rec& r, HitArea a, const x11::InputEv& in) {
    // _NET_WM_MOVERESIZE directions: 0..7 = top-left, top, top-right, right, bottom-right, bottom, bottom-left, left; 8 move
    long dir = 8;
    switch (a) {
    case HitArea::TopLeft: dir = 0; break;
    case HitArea::Top: dir = 1; break;
    case HitArea::TopRight: dir = 2; break;
    case HitArea::Right: dir = 3; break;
    case HitArea::BottomRight: dir = 4; break;
    case HitArea::Bottom: dir = 5; break;
    case HitArea::BottomLeft: dir = 6; break;
    case HitArea::Left: dir = 7; break;
    default: break;
    }
    x.UngrabPointer(dpy, in.time); // the press grabbed the pointer; the window manager takes it over
    x.clientMessage(dpy, root, x11::Window(r.handle), netMoveResize, in.x_root, in.y_root, dir, long(in.detail), 1);
    x.Flush(dpy);
}
bool NativePlatform::Impl::pump(void* user, Application& app, f32 wait) {
    Impl& m = *static_cast<Impl*>(user);
    if (!m.dpy) return m.previous.pump ? m.previous.pump(m.previous.user, app, wait) : true;
    m.pollVk();
    if (m.anyVkPending()) wait = std::min(wait, 0.004f); // look for the worker's result soon
    auto& x = m.x;
    std::vector<detail::PumpWatch>& watches = detail::pumpWatches();
    if (wait > 0.f && !x.Pending(m.dpy)) {
        // the X connection, the wake pipe and the registered watches (D-Bus connections)
        pollfd fds[16];
        nfds_t n = 0;
        fds[n++] = {x.ConnectionNumber(m.dpy), POLLIN, 0};
        if (m.wakeFd[0] >= 0) fds[n++] = {m.wakeFd[0], POLLIN, 0};
        for (const detail::PumpWatch& pw : watches)
            if (pw.fd >= 0 && n < 16) fds[n++] = {pw.fd, POLLIN, 0};
        const int ms = std::isinf(wait) ? -1 : int(std::min(std::ceil(double(wait) * 1000.0), 2.0e9));
        ::poll(fds, n, ms);
    }
    if (m.wakeFd[0] >= 0) {
        char buf[64];
        while (::read(m.wakeFd[0], buf, sizeof buf) > 0) {}
    }
    for (std::size_t i = 0; i < watches.size(); ++i) watches[i].fn(watches[i].user); // may add / remove watches
    while (x.Pending(m.dpy)) {
        x11::Event ev{};
        x.NextEvent(m.dpy, &ev);
        if (ev.type == x11::MappingNotify) { // keyboard remapped: Xlib's keysym tables follow
            x.RefreshKeyboardMapping(&ev);
            continue;
        }
        if (m.xim && x.FilterEvent(&ev, 0)) continue; // the input method took it (compose, pre-edit)
        if (ev.any.window == m.root || (m.xsOwner && ev.any.window == m.xsOwner)) {
            m.x11Event(ev); // XSETTINGS / RESOURCE_MANAGER
            continue;
        }
        std::size_t pi = 0;
        if (Rec* pr = m.findPopup(ev.any.window, pi)) { // a popup window: input in context coordinates of its owner
            Rec::Popup& p = pr->popups[pi];
            UiContext& ui = pr->win->ui();
            const f32 s = pr->win->scale();
            const Vec2 pos{p.rect.x + f32(ev.input.x) / s, p.rect.y + f32(ev.input.y) / s};
            if (ev.type == x11::Expose) m.blitPopupOs(p);
            else if (ev.type == x11::MotionNotify) { ui.setModifiers(x11::mods(ev.input.state)); ui.pointerMove(pos); }
            else if (ev.type == x11::ButtonPress && (ev.input.x < 0 || ev.input.y < 0 || f32(ev.input.x) >= p.screen.w ||
                                                     f32(ev.input.y) >= p.screen.h)) {
                closeAllPopups(*pr); // grabbed click outside every window of the app
                continue;
            } else if (ev.type == x11::ButtonPress) {
                ui.setModifiers(x11::mods(ev.input.state));
                if (ev.input.detail == 4 || ev.input.detail == 5) ui.wheel(pos, {0.f, ev.input.detail == 4 ? 1.f : -1.f});
                else ui.pointerDown(pos, ev.input.detail == 3 ? MouseButton::Right : MouseButton::Left);
            } else if (ev.type == x11::ButtonRelease && ev.input.detail <= 3) ui.pointerUp(pos);
            pr->win->requestRedraw();
            continue;
        }
        Rec* r = m.find(ev.any.window);
        if (ev.type == x11::ConfigureNotify) r = m.find(ev.configure.window);
        if (!r || !r->win) continue;
        AppWindow& w = *r->win;
        UiContext& ui = w.ui();
        const x11::InputEv& in = ev.input;
        const Vec2 pos{f32(in.x) / w.scale(), f32(in.y) / w.scale()}; // device pixels -> UI units
        switch (ev.type) {
        case x11::ClientMessage:
            if (ev.client.message_type == m.wmProtocols && x11::Atom(ev.client.l[0]) == m.wmDelete) w.requestClose();
            break;
        case x11::MapNotify: r->mapped = true; w.requestRedraw(); break;
        case x11::FocusIn: r->focused = true; break; // keyboard focus (accessibility adapters report it)
        case x11::FocusOut: r->focused = false; break;
        case x11::Expose: r->mapped = true; w.requestRedraw(); break;
        case x11::ConfigureNotify: {
            const AppWindow::PixelSize s = w.pixelSize();
            if (ev.configure.width > 0 && ev.configure.height > 0 &&
                (u32(ev.configure.width) != s.w || u32(ev.configure.height) != s.h))
                w.resizePixels(u32(ev.configure.width), u32(ev.configure.height));
            break;
        }
        case x11::PropertyNotify:
            if (ev.property.atom == m.netWmState) { // the window manager (un)maximised the window
                std::vector<unsigned char> v;
                bool vert = false, horz = false;
                if (x.property(m.dpy, x11::Window(r->handle), m.netWmState, x11::kXA_ATOM, v))
                    for (std::size_t i = 0; i + sizeof(long) <= v.size(); i += sizeof(long)) {
                        long a = 0;
                        std::memcpy(&a, v.data() + i, sizeof a);
                        vert |= x11::Atom(a) == m.netMaxV;
                        horz |= x11::Atom(a) == m.netMaxH;
                    }
                if (w.chrome().maximized != (vert && horz)) {
                    w.chrome().maximized = vert && horz;
                    w.requestRedraw();
                }
            }
            break;
        case x11::MotionNotify: {
            ui.setModifiers(x11::mods(in.state));
            ui.pointerMove(pos);
            // custom chrome: the resize cursor over a frame edge (the window manager draws none)
            CursorShape edge = CursorShape::Auto;
            if (w.chrome().frame == WindowFrame::Custom && !(in.state & (1u << 8))) switch (chromeHitTest(w, pos)) {
                case HitArea::Left: case HitArea::Right: edge = CursorShape::ResizeEW; break;
                case HitArea::Top: case HitArea::Bottom: edge = CursorShape::ResizeNS; break;
                case HitArea::TopLeft: case HitArea::BottomRight: edge = CursorShape::ResizeNWSE; break;
                case HitArea::TopRight: case HitArea::BottomLeft: edge = CursorShape::ResizeNESW; break;
                default: break;
                }
            r->edgeCursor = edge;
            m.syncCursor(*r);
            break;
        }
        case x11::ButtonPress:
            ui.setModifiers(x11::mods(in.state));
            if (in.detail == 1 && w.chrome().frame != WindowFrame::System) { // custom chrome: caption / edges go to the WM
                const HitArea a = chromeHitTest(w, pos);
                if (a == HitArea::Caption && r->captionPress && in.time - r->captionPress < 400) {
                    r->captionPress = 0; // double click on the caption toggles maximise
                    w.command(WindowCommand::ToggleMaximize);
                    break;
                }
                if (a != HitArea::Client && a != HitArea::Minimize && a != HitArea::Maximize && a != HitArea::Close) {
                    r->captionPress = a == HitArea::Caption ? std::max(in.time, 1ul) : 0;
                    m.startMoveResize(*r, a, in);
                    break;
                }
            }
            if (in.detail == 4 || in.detail == 5) ui.wheel(pos, {0.f, in.detail == 4 ? 1.f : -1.f});
            else if (in.detail == 6 || in.detail == 7) ui.wheel(pos, {in.detail == 6 ? -1.f : 1.f, 0.f});
            else ui.pointerDown(pos, in.detail == 3 ? MouseButton::Right : in.detail == 2 ? MouseButton::Middle : MouseButton::Left);
            break;
        case x11::ButtonRelease:
            ui.setModifiers(x11::mods(in.state));
            if (in.detail <= 3) ui.pointerUp(pos);
            m.syncCursor(*r);
            break;
        case x11::KeyPress:
        case x11::KeyRelease: {
            char buf[32];
            x11::KeySym sym = 0;
            x11::InputEv copy = in;
            const int n = x.LookupString(&copy, buf, int(sizeof buf), &sym, nullptr);
            bool ascii = false;
            const Key k = x11::mapKey(sym, ascii);
            const u8 md = x11::mods(in.state);
            if (k != Key::Other) { // printable keys too: viewport WASD / 1-4; text arrives separately
                if (ev.type == x11::KeyPress) ui.keyDown(k, md);
                else ui.keyUp(k, md);
            }
            if (ev.type == x11::KeyPress && r->xic) { // through the input method: UTF-8 (composed text arrives here)
                char tb[64];
                x11::KeySym ts = 0;
                int status = 0;
                x11::InputEv c2 = in;
                int tn = x.Utf8LookupString(r->xic, &c2, tb, int(sizeof tb), &ts, &status);
                std::string s;
                if (status == x11::kBufferOverflow) {
                    s.resize(std::size_t(tn));
                    tn = x.Utf8LookupString(r->xic, &c2, s.data(), int(s.size()), &ts, &status);
                    s.resize(std::size_t(std::max(tn, 0)));
                } else if (tn > 0) {
                    s.assign(tb, std::size_t(tn));
                }
                if ((status == x11::kLookupChars || status == x11::kLookupBoth) && !(md & (kModCtrl | kModAlt))) {
                    std::erase_if(s, [](char ch) { return u8(ch) < 0x20 || u8(ch) == 0x7F; });
                    if (!s.empty()) ui.textInput(s);
                }
            } else if (ev.type == x11::KeyPress && n > 0 && !(md & (kModCtrl | kModAlt))) {
                std::string s; // XLookupString yields Latin-1
                for (int i = 0; i < n; ++i)
                    if (u8(buf[i]) >= 0x20 && u8(buf[i]) != 0x7F) appendUtf8(s, u8(buf[i]));
                if (!s.empty()) ui.textInput(s);
            }
            break;
        }
        default: break;
        }
    }
    return true;
}
bool NativePlatform::Impl::screenInfo(const Rec& r, int& ox, int& oy, Rect& work) const {
    const int scr = x.DefaultScreen(dpy);
    x11::Window child = 0;
    if (!x.TranslateCoordinates(dpy, x11::Window(r.handle), x.RootWindow(dpy, scr), 0, 0, &ox, &oy, &child)) return false;
    work = {0.f, 0.f, f32(x.DisplayWidth(dpy, scr)), f32(x.DisplayHeight(dpy, scr))};
    for (const DisplayInfo& d : x11Displays()) // the work area of the monitor holding the window's origin
        if (f32(ox) >= d.bounds.x && f32(oy) >= d.bounds.y && f32(ox) < d.bounds.x + d.bounds.w && f32(oy) < d.bounds.y + d.bounds.h) {
            work = d.work;
            break;
        }
    return true;
}

std::vector<DisplayInfo> NativePlatform::Impl::x11Displays() const {
    std::vector<DisplayInfo> out;
    if (!dpy) return out;
    const int scr = x.DefaultScreen(dpy);
    const Rect screen{0.f, 0.f, f32(x.DisplayWidth(dpy, scr)), f32(x.DisplayHeight(dpy, scr))};
    if (xrOk) { // RandR 1.5 monitors (outputs and user-defined monitors), refresh from the output's CRTC mode
        int n = 0;
        if (x11::MonitorInfo* mons = xr.GetMonitors(dpy, root, 1, &n)) {
            x11::ScreenResources* res = xr.GetScreenResourcesCurrent(dpy, root);
            for (int i = 0; i < n; ++i) {
                const x11::MonitorInfo& mi = mons[i];
                DisplayInfo d;
                if (char* nm = mi.name ? x.GetAtomName(dpy, mi.name) : nullptr) {
                    d.name = nm;
                    x.Free(nm);
                }
                d.bounds = d.work = {f32(mi.x), f32(mi.y), f32(mi.width), f32(mi.height)};
                d.primary = mi.primary != 0;
                if (res && mi.noutput > 0)
                    if (x11::OutputInfo* oi = xr.GetOutputInfo(dpy, res, mi.outputs[0])) {
                        if (oi->crtc)
                            if (x11::CrtcInfo* ci = xr.GetCrtcInfo(dpy, res, oi->crtc)) {
                                for (int k = 0; k < res->nmode; ++k) {
                                    const x11::ModeInfo& md = res->modes[k];
                                    if (md.id == ci->mode && md.hTotal && md.vTotal)
                                        d.refreshHz = f32(double(md.dotClock) / (double(md.hTotal) * double(md.vTotal)));
                                }
                                xr.FreeCrtcInfo(ci);
                            }
                        xr.FreeOutputInfo(oi);
                    }
                out.push_back(std::move(d));
            }
            if (res) xr.FreeScreenResources(res);
            xr.FreeMonitors(mons);
        }
    }
    if (out.empty()) { // core X11: the whole screen
        DisplayInfo d;
        d.name = "screen " + std::to_string(scr);
        d.bounds = d.work = screen;
        d.primary = true;
        out.push_back(std::move(d));
    }
    bool primary = false;
    for (const DisplayInfo& d : out) primary |= d.primary;
    if (!primary) out.front().primary = true; // RandR may name none
    // EWMH work area (desktop 0; panels / docks excluded), clipped to each monitor
    std::vector<unsigned char> wa;
    if (netWorkarea && x.property(dpy, root, netWorkarea, x11::kXA_CARDINAL, wa) && wa.size() >= 4 * sizeof(long)) {
        long v[4];
        std::memcpy(v, wa.data(), sizeof v);
        for (DisplayInfo& d : out) {
            const f32 x0 = std::max(d.bounds.x, f32(v[0])), y0 = std::max(d.bounds.y, f32(v[1]));
            const f32 x1 = std::min(d.bounds.x + d.bounds.w, f32(v[0] + v[2])), y1 = std::min(d.bounds.y + d.bounds.h, f32(v[1] + v[3]));
            if (x1 > x0 && y1 > y0) d.work = {x0, y0, x1 - x0, y1 - y0};
        }
    }
    for (DisplayInfo& d : out) d.scale = xScale; // X11 has one DPI for the whole screen
    return out;
}
bool NativePlatform::Impl::createPopupOs(Rec& r, Rec::Popup& p) {
    (void)r;
    const int scr = x.DefaultScreen(dpy);
    const x11::Window w = x.CreateSimpleWindow(dpy, x.RootWindow(dpy, scr), int(p.screen.x), int(p.screen.y), unsigned(p.screen.w),
                                               unsigned(p.screen.h), 0, 0, 0);
    if (!w) return false;
    x11::SetWindowAttributes a{};
    a.override_redirect = 1; // no WM frame / focus: a menu window (Qt::Popup)
    a.save_under = 1;
    x.ChangeWindowAttributes(dpy, w, x11::kCWOverrideRedirect | x11::kCWSaveUnder, &a);
    const x11::Atom type = x.InternAtom(dpy, "_NET_WM_WINDOW_TYPE", 0), menu = x.InternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", 0);
    x.ChangeProperty(dpy, w, type, 4 /*XA_ATOM*/, 32, 0, reinterpret_cast<const unsigned char*>(&menu), 1);
    x.SelectInput(dpy, w, x11::kPopupMask);
    x.MapWindow(dpy, w);
    p.handle = w;
    return true;
}
void NativePlatform::Impl::grabPointer(Rec& r, bool on) {
    if (!on) { x.UngrabPointer(dpy, 0); x.Flush(dpy); return; }
    if (r.popups.empty() || !r.popups[0].handle) return;
    x.Sync(dpy, 0); // the override-redirect window must be viewable before it can grab
    // owner_events: the app's own windows (main + popups) keep their events; everything else reports to the popup
    x.GrabPointer(dpy, x11::Window(r.popups[0].handle), 1, (1u << 2) | (1u << 3) | (1u << 6), 1, 1, 0, 0, 0);
    x.Flush(dpy);
}
void NativePlatform::Impl::movePopupOs(Rec::Popup& p) {
    x.MoveResizeWindow(dpy, x11::Window(p.handle), int(p.screen.x), int(p.screen.y), unsigned(p.screen.w), unsigned(p.screen.h));
}
void NativePlatform::Impl::destroyPopupOs(Rec::Popup& p) {
    if (!p.handle || !dpy) return;
    x.DestroyWindow(dpy, x11::Window(p.handle));
    x.Flush(dpy);
    p.handle = 0;
}
void NativePlatform::Impl::blitPopupOs(Rec::Popup& p) {
    if (!p.handle || p.img.pixels.empty()) return;
    const int scr = x.DefaultScreen(dpy);
    if (x.DefaultDepth(dpy, scr) < 24) return;
    p.staged.resize(p.img.pixels.size());
    for (std::size_t i = 0; i < p.img.pixels.size(); ++i) { // 0xAABBGGRR -> TrueColor 0x00RRGGBB
        const u32 q = p.img.pixels[i];
        p.staged[i] = ((q & 0xFFu) << 16) | (q & 0xFF00u) | ((q >> 16) & 0xFFu);
    }
    x11::Image* img = x.CreateImage(dpy, x.DefaultVisual(dpy, scr), unsigned(x.DefaultDepth(dpy, scr)), 2 /*ZPixmap*/, 0,
                                    reinterpret_cast<char*>(p.staged.data()), unsigned(p.img.width), unsigned(p.img.height), 32, 0);
    if (!img) return;
    x.PutImage(dpy, x11::Window(p.handle), x.DefaultGC(dpy, scr), img, 0, 0, 0, 0, unsigned(p.img.width), unsigned(p.img.height));
    img->data = nullptr; // ours (XDestroyImage would free it)
    img->f.destroy(img);
    x.Flush(dpy);
}
bool NativePlatform::Impl::postClickOs(unsigned long long h, Vec2 at) {
    int rx = 0, ry = 0; // screen position, like a real press (custom chrome hands it to _NET_WM_MOVERESIZE)
    x11::Window child = 0;
    x.TranslateCoordinates(dpy, x11::Window(h), root, int(at.x), int(at.y), &rx, &ry, &child);
    for (int type : {int(x11::ButtonPress), int(x11::ButtonRelease)}) {
        x11::Event ev{};
        ev.input.type = type;
        ev.input.display = dpy;
        ev.input.window = x11::Window(h);
        ev.input.root = root;
        ev.input.x = int(at.x);
        ev.input.y = int(at.y);
        ev.input.x_root = rx;
        ev.input.y_root = ry;
        ev.input.detail = 1;
        ev.input.same_screen = 1;
        if (!x.SendEvent(dpy, x11::Window(h), 0, 0, &ev)) return false;
    }
    x.Flush(dpy);
    return true;
}
bool NativePlatform::Impl::captureOs(const Rec& r, Image& out) {
    const AppWindow::PixelSize sz = r.win->pixelSize();
    if (captureX(x11::Window(r.handle), sz.w, sz.h, out)) return true;
    // window not fully on screen (GetImage: BadMatch) or not mapped: a Vulkan window reports the frame it presented
    return r.vk && detail::vkWinReadback(r.vk, out);
}
bool NativePlatform::Impl::capturePopupOs(const Rec::Popup& p, Image& out) {
    return p.handle && captureX(x11::Window(p.handle), unsigned(p.screen.w), unsigned(p.screen.h), out);
}
bool NativePlatform::Impl::captureX(unsigned long win, unsigned w, unsigned h, Image& out) {
    x.Sync(dpy, 0);
    x11::Image* img = x.GetImage(dpy, win, 0, 0, w, h, ~0ul, 2 /*ZPixmap*/);
    if (!img) return false;
    bool good = img->bits_per_pixel == 32;
    if (good) {
        out.resize(img->width, img->height);
        for (int y = 0; y < img->height; ++y) {
            const u32* row = reinterpret_cast<const u32*>(img->data + std::ptrdiff_t(y) * img->bytes_per_line);
            for (int i = 0; i < img->width; ++i) {
                const u32 p = row[i]; // 0x00RRGGBB (TrueColor 24/32)
                out.pixels[std::size_t(y) * std::size_t(img->width) + std::size_t(i)] =
                    0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
            }
        }
    }
    img->f.destroy(img);
    return good;
}

f32 NativePlatform::Impl::readXScale() {
    if (const char* e = std::getenv("FUSE_UI_SCALE"); e && *e) { // explicit override
        const f32 v = std::strtof(e, nullptr);
        if (v > 0.f) return std::clamp(v, 0.25f, 8.f);
    }
    if (xsDpi > 0) { // XSETTINGS Xft/DPI (dpi * 1024): what GNOME / KDE / xsettingsd announce, changes live
        const f32 dpi = f32(xsDpi) / 1024.f;
        if (dpi >= 48.f && dpi <= 768.f) return dpi / 96.f;
    }
    // "Xft.dpi:\t144" in the root's RESOURCE_MANAGER property (read live; XResourceManagerString is a startup copy)
    std::vector<unsigned char> rm;
    if (x.property(dpy, root, resourceManager, x11::kXA_STRING, rm)) {
        rm.push_back(0);
        if (const char* at = std::strstr(reinterpret_cast<const char*>(rm.data()), "Xft.dpi:")) {
            const f32 dpi = std::strtof(at + 8, nullptr);
            if (dpi >= 48.f && dpi <= 768.f) return dpi / 96.f;
        }
    }
    return 1.f;
}
void NativePlatform::Impl::readXSettings() {
    const x11::Window owner = x.GetSelectionOwner(dpy, xsSelection);
    if (owner != xsOwner) {
        xsOwner = owner;
        if (owner) x.SelectInput(dpy, owner, x11::kPropertyChangeMask | x11::kStructureNotifyMask); // changes, manager exit
    }
    xsDpi = 0;
    xsTheme.clear();
    xsCursorSize = 0;
    xsCursorTheme.clear();
    std::vector<unsigned char> v;
    if (owner && x.property(dpy, owner, xsSettings, xsSettings, v))
        for (const x11::XSetting& s : x11::parseXSettings(v.data(), v.size())) {
            if (s.name == "Xft/DPI" && s.type == 0) xsDpi = s.integer;
            else if (s.name == "Net/ThemeName" && s.type == 1) xsTheme = s.string;
            else if (s.name == "Gtk/CursorThemeSize" && s.type == 0) xsCursorSize = s.integer;
            else if (s.name == "Gtk/CursorThemeName" && s.type == 1) xsCursorTheme = s.string;
        }
}
void NativePlatform::Impl::updateCursorTheme() {
    if (!xcOk) return;
    // Gtk/CursorThemeSize is logical (GTK multiplies it by the window scale); else $XCURSOR_SIZE, else 24
    int base = xsCursorSize > 0 && xsCursorSize <= 512 ? xsCursorSize : 0;
    if (!base)
        if (const char* e = std::getenv("XCURSOR_SIZE"); e && *e) base = std::clamp(std::atoi(e), 0, 512);
    if (!base) base = 24;
    const int size = std::max(8, int(std::lround(f32(base) * xScale)));
    if (size == cursorSize && xsCursorTheme == cursorTheme) return;
    cursorSize = size;
    cursorTheme = xsCursorTheme;
    xc.SetTheme(dpy, cursorTheme.empty() ? nullptr : cursorTheme.c_str());
    xc.SetDefaultSize(dpy, size);
    for (unsigned long& c : cursors)
        if (c) { x.FreeCursor(dpy, c); c = 0; }
    for (Rec* r : recs) { // re-apply at the new size
        r->svc.cursor = CursorShape::Auto;
        if (r->handle) syncCursor(*r);
    }
}
void NativePlatform::Impl::settingsChanged() {
    const std::string oldTheme = xsTheme;
    readXSettings();
    if (opts.dpiAware) { // DPI: every window re-scales (DpiChanged), keeping its logical size
        const f32 s = readXScale();
        if (s != xScale) {
            xScale = s;
            for (Rec* r : recs)
                if (r->handle && r->win->scale() != s) applyScale(*r, s, nullptr);
        }
    }
    updateCursorTheme(); // Gtk/CursorThemeSize / Gtk/CursorThemeName or the DPI scale changed
    if (xsTheme != oldTheme) themeChanged(); // Application::setSystemTheme fires ThemeChanged only on a real change
}
void NativePlatform::Impl::x11Event(x11::Event& ev) {
    switch (ev.type) {
    case x11::ClientMessage: // a new XSETTINGS manager announces itself (MANAGER, l[1] = the selection)
        if (ev.client.message_type == manager && x11::Atom(ev.client.l[1]) == xsSelection) settingsChanged();
        break;
    case x11::PropertyNotify:
        if ((ev.any.window == xsOwner && ev.property.atom == xsSettings) || (ev.any.window == root && ev.property.atom == resourceManager))
            settingsChanged();
        break;
    case x11::DestroyNotify: // the manager went away: back to RESOURCE_MANAGER / defaults
        if (ev.any.window == xsOwner) {
            xsOwner = 0;
            settingsChanged();
        }
        break;
    default: break;
    }
}
void NativePlatform::Impl::syncCursor(Rec& r) {
    CursorShape c = r.edgeCursor != CursorShape::Auto ? r.edgeCursor : r.win->ui().cursor();
    if (c == CursorShape::Auto) c = CursorShape::Arrow;
    if (c == r.svc.cursor || !r.handle) return;
    // X font cursor glyphs (cursorfont.h): left_ptr, xterm, hand2, sb_v/h_double_arrow, corners, fleur, watch, X, cross
    static constexpr unsigned kGlyph[] = {68, 68, 152, 60, 116, 108, 14, 12, 52, 150, 0, 34};
    static_assert(sizeof(kGlyph) / sizeof(kGlyph[0]) == std::size_t(CursorShape::Count));
    // themed (libXcursor, freedesktop cursor-spec name, then the legacy X name): sized by updateCursorTheme()
    static constexpr const char* kNames[][2] = {{"default", "left_ptr"}, {"default", "left_ptr"}, {"text", "xterm"},
        {"pointer", "hand2"}, {"ns-resize", "sb_v_double_arrow"}, {"ew-resize", "sb_h_double_arrow"},
        {"nwse-resize", "bottom_right_corner"}, {"nesw-resize", "bottom_left_corner"}, {"move", "fleur"},
        {"wait", "watch"}, {"not-allowed", "crossed_circle"}, {"crosshair", "cross"}};
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == std::size_t(CursorShape::Count));
    unsigned long& cur = cursors[std::size_t(c)];
    for (int k = 0; !cur && xcOk && k < 2; ++k) cur = xc.LibraryLoadCursor(dpy, kNames[std::size_t(c)][k]);
    if (!cur) cur = x.CreateFontCursor(dpy, kGlyph[std::size_t(c)]);
    x.DefineCursor(dpy, x11::Window(r.handle), cur);
    x.Flush(dpy);
    r.svc.cursor = c;
}
// XIM: opened with a UTF-8 LC_CTYPE (restored afterwards); XMODIFIERS picks a server IM (ibus, fcitx), else libX11's
// built-in compose IM. Pre-edit through callbacks when the IM offers them.
void NativePlatform::Impl::openXim() {
    if (!x.loadXim()) return;
    const char* cur = std::setlocale(LC_CTYPE, nullptr);
    const std::string saved = cur ? cur : "C";
    auto utf8 = [](const char* l) { return l && (std::strstr(l, "UTF-8") || std::strstr(l, "utf8") || std::strstr(l, "UTF8")); };
    // The environment's UTF-8 locale, else C.UTF-8 / en_US.UTF-8. A server IM ($XMODIFIERS) lists the locales it
    // serves and many (uim-xim, kinput2, older ibus / fcitx XIM front ends) list named locales only, not C.UTF-8:
    // when the first locale's server IM does not open, the next one is tried before libX11's built-in IM.
    std::string first;
    for (const char* want : {"", "C.UTF-8", "en_US.UTF-8"}) {
        const char* l = std::setlocale(LC_CTYPE, want);
        if (!utf8(l) || !x.SupportsLocale()) continue;
        if (first.empty()) first = l;
        x.SetLocaleModifiers("");
        xim = x.OpenIM(dpy, nullptr, nullptr, nullptr);
        if (xim) break;
    }
    if (!xim && !first.empty() && std::setlocale(LC_CTYPE, first.c_str())) { // $XMODIFIERS names no running IM
        x.SetLocaleModifiers("@im=none");
        xim = x.OpenIM(dpy, nullptr, nullptr, nullptr);
    }
    if (xim) {
        x11::XimStyles* st = nullptr;
        x.GetIMValues(xim, "queryInputStyle", &st, static_cast<void*>(nullptr));
        const x11::XimStyle prefer[] = {x11::kPreeditCallbacks | x11::kStatusNothing, x11::kPreeditCallbacks | x11::kStatusNone,
                                        x11::kPreeditPosition | x11::kStatusNothing, x11::kPreeditNothing | x11::kStatusNothing,
                                        x11::kPreeditNone | x11::kStatusNone, x11::kPreeditNothing | x11::kStatusNone};
        for (x11::XimStyle want : prefer) {
            for (unsigned short i = 0; st && i < st->count && !ximStyle; ++i)
                if (st->styles[i] == want) ximStyle = want;
            if (ximStyle) break;
        }
        if (st) x.Free(st);
        if (!ximStyle) {
            x.CloseIM(xim);
            xim = nullptr;
        }
    }
    std::setlocale(LC_CTYPE, saved.c_str());
}
// the XIM pre-edit (code points, caret in code points) -> UiContext::textComposition (UTF-8, caret in bytes)
void NativePlatform::Impl::publishPreedit(Rec& r) {
    std::string s;
    int caretBytes = 0;
    for (std::size_t i = 0; i < r.preedit.size(); ++i) {
        if (int(i) == r.preeditCaret) caretBytes = int(s.size());
        appendUtf8(s, u32(r.preedit[i]));
    }
    if (r.preeditCaret >= int(r.preedit.size())) caretBytes = int(s.size());
    r.win->ui().textComposition(s, caretBytes);
    r.win->requestRedraw();
}
void NativePlatform::Impl::createIc(Rec& r) {
    if (!xim) return;
    const x11::Window w = x11::Window(r.handle);
    void* term = nullptr;
    if (ximStyle & x11::kPreeditCallbacks) {
        // the start callback returns the pre-edit length limit (-1: none)
        r.cbStart = {reinterpret_cast<char*>(&r), reinterpret_cast<void (*)(void*, char*, void*)>(+[](void*, char*, void*) -> int { return -1; })};
        r.cbDone = {reinterpret_cast<char*>(&r), [](void*, char* c, void*) {
            Rec& rr = *reinterpret_cast<Rec*>(c);
            rr.preedit.clear();
            rr.preeditCaret = 0;
            rr.win->ui().textComposition({}, 0);
            rr.win->requestRedraw();
        }};
        r.cbDraw = {reinterpret_cast<char*>(&r), [](void*, char* c, void* call) {
            Rec& rr = *reinterpret_cast<Rec*>(c);
            const auto* d = static_cast<const x11::XimDraw*>(call);
            std::u32string ins;
            if (d->text && d->text->isWchar && d->text->s.wc) {
                for (unsigned short i = 0; i < d->text->length; ++i) ins += char32_t(d->text->s.wc[i]);
            } else if (d->text && d->text->s.mb) { // multi-byte in the IM's (UTF-8) locale
                const std::string_view mb = d->text->s.mb;
                for (std::size_t i = 0; i < mb.size();) {
                    const u8 b = u8(mb[i]);
                    const int len = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
                    u32 cp = len == 1 ? b : b & (0x7Fu >> len);
                    for (int k = 1; k < len && i + std::size_t(k) < mb.size(); ++k) cp = (cp << 6) | (u8(mb[i + std::size_t(k)]) & 0x3Fu);
                    ins += char32_t(cp);
                    i += std::size_t(len);
                }
            }
            const std::size_t first = std::min(std::size_t(std::max(d->chgFirst, 0)), rr.preedit.size());
            const std::size_t len = std::min(std::size_t(std::max(d->chgLength, 0)), rr.preedit.size() - first);
            rr.preedit.replace(first, len, ins);
            rr.preeditCaret = d->caret;
            publishPreedit(rr);
        }};
        r.cbCaret = {reinterpret_cast<char*>(&r), [](void*, char* c, void* call) {
            Rec& rr = *reinterpret_cast<Rec*>(c);
            auto* d = static_cast<x11::XimCaret*>(call);
            const int n = int(rr.preedit.size());
            switch (d->direction) { // XIMCaretDirection
            case 0: ++rr.preeditCaret; break;                    // XIMForwardChar
            case 1: --rr.preeditCaret; break;                    // XIMBackwardChar
            case 8: rr.preeditCaret = 0; break;                  // XIMLineStart
            case 9: rr.preeditCaret = n; break;                  // XIMLineEnd
            case 10: rr.preeditCaret = d->position; break;       // XIMAbsolutePosition
            default: break;                                      // word / line moves: not used by single-line pre-edits
            }
            rr.preeditCaret = std::clamp(rr.preeditCaret, 0, n);
            d->position = rr.preeditCaret;
            publishPreedit(rr);
        }};
        void* list = x.VaCreateNestedList(0, "preeditStartCallback", &r.cbStart, "preeditDoneCallback", &r.cbDone,
                                          "preeditDrawCallback", &r.cbDraw, "preeditCaretCallback", &r.cbCaret, term);
        r.xic = x.CreateIC(xim, "inputStyle", ximStyle, "clientWindow", w, "focusWindow", w, "preeditAttributes", list, term);
        if (list) x.Free(list);
    } else {
        r.xic = x.CreateIC(xim, "inputStyle", ximStyle, "clientWindow", w, "focusWindow", w, term);
    }
    if (r.xic) {
        long extra = 0; // events the IM wants to see as well
        x.GetICValues(r.xic, "filterEvents", &extra, term);
        x.SelectInput(dpy, w, x11::kMask | extra);
        x.UnsetICFocus(r.xic); // on with the first text field (syncTextInput)
    }
}
void NativePlatform::Impl::syncTextInput(Rec& r) {
    UiContext& ui = r.win->ui();
    const bool want = ui.wantsTextInput();
    if (!r.imeKnown || want != r.imeOn) {
        if (!want && !r.preedit.empty()) { // focus left the field: the IM drops its pre-edit (nothing is committed)
            if (r.xic && x.Utf8ResetIC) {
                if (char* left = x.Utf8ResetIC(r.xic)) x.Free(left);
            }
            r.preedit.clear();
            r.preeditCaret = 0;
            ui.textComposition({}, 0);
        }
        if (r.xic) want ? x.SetICFocus(r.xic) : x.UnsetICFocus(r.xic);
        r.imeOn = want;
        r.imeKnown = true;
        r.imeArea = {-1.f, -1.f, 0.f, 0.f};
    }
    if (!want) return;
    const f32 s = r.win->scale();
    const Rect c = ui.textCaret();
    const Rect px{std::round(c.x * s), std::round(c.y * s), std::max(1.f, std::round(c.w * s)), std::max(1.f, std::round(c.h * s))};
    if (px.x == r.imeArea.x && px.y == r.imeArea.y && px.w == r.imeArea.w && px.h == r.imeArea.h) return;
    r.imeArea = px;
    if (r.xic && (ximStyle & x11::kPreeditPosition)) { // over-the-spot IMs draw the pre-edit at the caret
        x11::XPoint spot{short(px.x), short(px.y + px.h)};
        void* term = nullptr;
        void* list = x.VaCreateNestedList(0, "spotLocation", &spot, term);
        x.SetICValues(r.xic, "preeditAttributes", list, term);
        if (list) x.Free(list);
    }
}
void NativePlatform::Impl::applyScale(Rec& r, f32 scale, const void*) {
    r.win->setScale(scale); // keeps the logical size; the window grows to logical * scale device pixels
    if (r.handle) {
        x.ResizeWindow(dpy, x11::Window(r.handle), std::max(1u, r.win->pixelSize().w), std::max(1u, r.win->pixelSize().h));
        x.Flush(dpy);
    }
    r.win->requestRedraw();
}
void NativePlatform::Impl::updateTitleBars() {} // the window manager draws X11 decorations

NativePlatform::NativePlatform(Application& app, const NativeOptions& o) : m(std::make_unique<Impl>(app, o)) {
    const char* disp = std::getenv("DISPLAY");
    if (disp && *disp && m->x.load()) {
        m->dpy = m->x.OpenDisplay(nullptr);
        if (m->dpy) {
            Impl& i = *m;
            auto& x = i.x;
            x.SetErrorHandler(&x11::ignoreErrors);
            const int scr = x.DefaultScreen(i.dpy);
            i.root = x.RootWindow(i.dpy, scr);
            auto atom = [&](const char* n) { return x.InternAtom(i.dpy, n, 0); };
            i.wmProtocols = atom("WM_PROTOCOLS");
            i.wmDelete = atom("WM_DELETE_WINDOW");
            i.netWmState = atom("_NET_WM_STATE");
            i.netMaxV = atom("_NET_WM_STATE_MAXIMIZED_VERT");
            i.netMaxH = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
            i.netMoveResize = atom("_NET_WM_MOVERESIZE");
            i.wmChangeState = atom("WM_CHANGE_STATE");
            i.motifHints = atom("_MOTIF_WM_HINTS");
            i.netWorkarea = atom("_NET_WORKAREA");
            i.resourceManager = atom("RESOURCE_MANAGER");
            i.manager = atom("MANAGER");
            i.xsSelection = atom(("_XSETTINGS_S" + std::to_string(scr)).c_str());
            i.xsSettings = atom("_XSETTINGS_SETTINGS");
            int evBase = 0, errBase = 0;
            i.xrOk = i.xr.load() && i.xr.QueryExtension(i.dpy, &evBase, &errBase);
            // root: XSETTINGS manager announcements (StructureNotify), RESOURCE_MANAGER changes (PropertyChange)
            x.SelectInput(i.dpy, i.root, x11::kStructureNotifyMask | x11::kPropertyChangeMask);
            i.readXSettings();
            i.xScale = o.dpiAware ? i.readXScale() : 1.f;
            i.openXim();
            i.xcOk = i.xc.load();
            i.updateCursorTheme();
            if (::pipe(i.wakeFd) == 0) {
                for (int fd : i.wakeFd) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
                for (int fd : i.wakeFd) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            } else {
                i.wakeFd[0] = i.wakeFd[1] = -1;
            }
        } else {
            m->error = "cannot open X display";
        }
    } else {
        m->error = disp && *disp ? "libX11.so.6 not found" : "no DISPLAY";
    }
    if (m->dpy) app.setEventSource({m.get(), &Impl::pump});
    app.setSystemTheme(m->theme());
}
bool NativePlatform::available() const { return m->dpy != nullptr; }
void* NativePlatform::nativeDisplay() const { return m->dpy; }
SystemTheme NativePlatform::Impl::theme() const { return detail::posixQueryTheme(opts, xsTheme); }
void NativePlatform::wake() {
    if (m->wakeFd[1] >= 0) {
        const char c = 1;
        [[maybe_unused]] const ssize_t n = ::write(m->wakeFd[1], &c, 1); // full pipe: a wake is pending anyway
    }
}
#endif

// ---- software presenter + Vulkan presenter creation (sync / async) -------------------------------------------------
void NativePlatform::Impl::blitSoftware(Rec& r) {
    // only the rect the last rasterize() redrew: the window keeps the rest of the previous frame
    const Image& img = r.win->image();
    if (!r.handle || img.pixels.empty()) return;
    const Rect rr = r.win->rasterized().intersect({0.f, 0.f, f32(img.width), f32(img.height)});
    if (rr.empty()) return;
    const int x0 = int(rr.x), y0 = int(rr.y), w = int(rr.w), h = int(rr.h);
#if defined(_WIN32)
    r.staged.resize(std::max(r.staged.size(), std::size_t(w) * std::size_t(h)));
    for (int y = 0; y < h; ++y) { // 0xAABBGGRR -> BGRA, packed to w x h
        const u32* src = img.pixels.data() + std::size_t(y0 + y) * std::size_t(img.width) + std::size_t(x0);
        u32* dst = r.staged.data() + std::size_t(y) * std::size_t(w);
        for (int x = 0; x < w; ++x) dst[x] = (src[x] & 0xFF00FF00u) | ((src[x] & 0xFFu) << 16) | ((src[x] >> 16) & 0xFFu);
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HWND hwnd = reinterpret_cast<HWND>(r.handle);
    HDC dc = GetDC(hwnd);
    SetDIBitsToDevice(dc, x0, y0, DWORD(w), DWORD(h), 0, 0, 0, UINT(h), r.staged.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(hwnd, dc);
#else
    if (!dpy) return;
    const int scr = x.DefaultScreen(dpy);
    if (x.DefaultDepth(dpy, scr) < 24) return;
    r.staged.resize(img.pixels.size());
    for (int y = y0; y < y0 + h; ++y) { // 0xAABBGGRR -> TrueColor 0x00RRGGBB
        const std::size_t row = std::size_t(y) * std::size_t(img.width);
        for (int i = x0; i < x0 + w; ++i) {
            const u32 q = img.pixels[row + std::size_t(i)];
            r.staged[row + std::size_t(i)] = ((q & 0xFFu) << 16) | (q & 0xFF00u) | ((q >> 16) & 0xFFu);
        }
    }
    x11::Image* xi = x.CreateImage(dpy, x.DefaultVisual(dpy, scr), unsigned(x.DefaultDepth(dpy, scr)), 2 /*ZPixmap*/, 0,
                                   reinterpret_cast<char*>(r.staged.data()), unsigned(img.width), unsigned(img.height), 32, 0);
    if (!xi) return;
    x.PutImage(dpy, x11::Window(r.handle), x.DefaultGC(dpy, scr), xi, x0, y0, x0, y0, unsigned(w), unsigned(h));
    xi->data = nullptr; // ours (XDestroyImage would free it)
    xi->f.destroy(xi);
    x.Flush(dpy);
#endif
}

SurfaceSource NativePlatform::Impl::surfaceOf(const Rec& r) const {
    SurfaceSource s;
#if defined(_WIN32)
    s.kind = SurfaceSource::Win32;
    s.hinstance = hinst;
    s.hwnd = reinterpret_cast<void*>(r.handle);
#else
    s.kind = SurfaceSource::Xlib;
    s.display = dpy;
    s.xwindow = static_cast<unsigned long>(r.handle);
#endif
    return s;
}

void NativePlatform::Impl::createVkSync(Rec& r) {
    if (detail::VkCtx* c = vkContext()) r.vk = detail::vkWinCreate(c, surfaceOf(r), opts, error);
}

void NativePlatform::Impl::startVkAsync(Rec& r) {
    SurfaceSource s = surfaceOf(r);
#if !defined(_WIN32)
    // the worker talks to the X server through its own connection; the swapchain keeps using it afterwards
    r.vkDpy = x.OpenDisplay(nullptr);
    if (!r.vkDpy) {
        createVkSync(r);
        return;
    }
    s.display = r.vkDpy;
#endif
    const AppWindow::PixelSize sz = r.win->pixelSize(); // swapchain in device pixels (DPI scale)
    const unsigned w = std::max(1u, sz.w), h = std::max(1u, sz.h);
    const NativeOptions o = opts;
    vkInFlight = true;
    r.vkPending = true;
    Rec* rec = &r;
    r.worker = std::thread([rec, s, o, w, h] {
        std::string err;
        detail::VkCtx* c = detail::vkCtxCreate(s.kind, err);
        detail::VkWin* vw = c ? detail::vkWinCreate(c, s, o, err) : nullptr;
        if (vw) {
            std::string prepErr; // a failed early swapchain is retried by the first present
            detail::vkWinPrepare(vw, w, h, prepErr);
        }
        rec->vkCtxNew = c;
        rec->vkNew = vw;
        rec->vkErr = std::move(err);
        rec->vkDone.store(true, std::memory_order_release);
    });
}

void NativePlatform::Impl::waitVk(Rec& r) {
    if (!r.worker.joinable()) return;
#if defined(_WIN32)
    // swapchain creation may send messages to the window (owned by this thread): keep serving them while waiting
    while (!r.vkDone.load(std::memory_order_acquire)) {
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_SENDMESSAGE);
    }
#endif
    r.worker.join();
}

void NativePlatform::Impl::takeOverVk(Rec& r) {
    waitVk(r);
    r.vkPending = false;
    vkInFlight = false;
    vkTried = true;
    vk = r.vkCtxNew;
    r.vkCtxNew = nullptr;
    if (r.vkNew) {
        r.vk = r.vkNew;
        r.vkNew = nullptr;
        r.kind = PresenterKind::Vulkan;
    } else {
        error = r.vkErr; // no Vulkan here: the window keeps the software presenter
    }
    if (r.win) r.win->requestRedraw(); // the next present (whole window) goes through the new presenter
}

void NativePlatform::Impl::pollVk() {
    for (Rec* r : recs)
        if (r->vkPending && r->vkDone.load(std::memory_order_acquire)) takeOverVk(*r);
    if (vkInFlight) return;
    for (Rec* r : recs)
        if (r->vkWanted) {
            r->vkWanted = false;
            createVkSync(*r);
            if (r->vk) {
                r->kind = PresenterKind::Vulkan;
                r->win->requestRedraw();
            }
        }
}

bool NativePlatform::presenterPending(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r && (r->vkPending || r->vkWanted);
}
PresenterKind NativePlatform::waitForPresenter(const AppWindow& w) {
    Impl::Rec* r = nullptr;
    for (Impl::Rec* x : m->recs)
        if (x->win == &w) r = x;
    if (!r) return PresenterKind::Headless;
    if (r->vkPending) m->takeOverVk(*r);
    for (Impl::Rec* x : m->recs) // a window whose own worker is pending elsewhere
        if (x->vkPending) m->takeOverVk(*x);
    m->pollVk();
    return r->kind;
}

NativePlatform::~NativePlatform() {
    // detach every shown window (restores its headless presenter) before the display / instance go away
    while (!m->recs.empty()) m->recs.back()->win->setPresenter({});
    detail::vkCtxDestroy(m->vk);
#if defined(_WIN32)
    m->app.setEventSource({});
#else
    if (m->dpy) {
        if (m->xim) m->x.CloseIM(m->xim);
        m->x.CloseDisplay(m->dpy);
        m->app.setEventSource({});
    }
    for (int fd : m->wakeFd)
        if (fd >= 0) ::close(fd);
#endif
}

PresenterKind NativePlatform::show(AppWindow& w) {
    if (const Impl::Rec* existing = m->find(w)) return existing->kind;
    if (!available()) return PresenterKind::Headless;
    auto* r = new Impl::Rec{};
    r->impl = m.get();
    r->win = &w;
    m->recs.push_back(r);
    if (!m->createOsWindow(*r)) {
        std::erase(m->recs, r);
        delete r;
        return PresenterKind::Headless;
    }
    const bool adopt = m->opts.adoptDevice.instance && m->opts.adoptDevice.device;
    if (m->vkInFlight) {
        r->vkWanted = true; // another window's worker is creating the device: take Vulkan when it is done
    } else if (m->opts.vulkan && m->opts.asyncVulkan && !adopt && !m->vkTried && FUSE_APP_ASYNC_VK) {
        m->startVkAsync(*r); // software frames now, Vulkan when the worker is done
    } else {
        m->createVkSync(*r);
    }
    r->kind = r->vk ? PresenterKind::Vulkan : PresenterKind::Software;
    if (m->opts.osPopups) w.ui().setOsPopups(true); // screen bounds arrive with the first present
    ClipboardHooks clip; // LineEdit Ctrl+C / X / V through the system clipboard
    clip.set = [](void*, std::string_view s) { return setClipboardText(s); };
    clip.get = [](void*, std::string& out) { return clipboardText(out); };
    w.ui().setClipboard(clip);
    w.setPresenter({r, &Impl::present, &Impl::destroy});
    return r->kind;
}
bool NativePlatform::textInputActive(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r && r->imeOn;
}
Rect NativePlatform::textInputArea(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r && r->imeOn ? r->imeArea : Rect{};
}
bool NativePlatform::inputMethodAvailable() const {
#if defined(_WIN32)
    return true; // IMM32 (TSF IMEs through its compatibility layer)
#else
    return m->xim != nullptr;
#endif
}
bool NativePlatform::inputMethodPreedit() const {
#if defined(_WIN32)
    return true;
#else
    return (m->ximStyle & x11::kPreeditCallbacks) != 0;
#endif
}

PresenterKind NativePlatform::kind(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r ? r->kind : PresenterKind::Headless;
}
std::string defaultCacheDir() {
#if defined(_WIN32)
    const char* base = std::getenv("LOCALAPPDATA");
    return base && *base ? std::string(base) + "\\FUSE\\cache" : std::string();
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/fuse";
    const char* home = std::getenv("HOME");
    return home && *home ? std::string(home) + "/.cache/fuse" : std::string();
#endif
}

std::string_view NativePlatform::gpuName() const { return detail::vkCtxDeviceName(m->vk); }
bool NativePlatform::gpuDraws(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r && detail::vkWinGpuDraws(r->vk);
}
std::string_view NativePlatform::lastError() const { return m->error; }
bool NativePlatform::setExternalTexture(const AppWindow& w, unsigned slot, const ExternalTexture& tex) {
    const Impl::Rec* r = m->find(w);
    return r && detail::vkWinSetExternal(r->vk, slot, tex);
}
bool NativePlatform::capture(const AppWindow& w, Image& out) {
    const Impl::Rec* r = m->find(w);
    return r && r->handle && m->captureOs(*r, out);
}
std::size_t NativePlatform::popupCount(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r ? r->popups.size() : 0;
}
unsigned long long NativePlatform::popupWindow(const AppWindow& w, std::size_t i) const {
    const Impl::Rec* r = m->find(w);
    return r && i < r->popups.size() ? r->popups[i].handle : 0;
}
Rect NativePlatform::popupScreenRect(const AppWindow& w, std::size_t i) const {
    const Impl::Rec* r = m->find(w);
    return r && i < r->popups.size() ? r->popups[i].screen : Rect{};
}
bool NativePlatform::capturePopup(const AppWindow& w, std::size_t i, Image& out) {
    const Impl::Rec* r = m->find(w);
    return r && i < r->popups.size() && m->capturePopupOs(r->popups[i], out);
}
Vec2 NativePlatform::screenOrigin(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    int ox = 0, oy = 0;
    Rect work;
    if (!r || !r->handle || !m->screenInfo(*r, ox, oy, work)) return {};
    return {f32(ox), f32(oy)};
}
Insets NativePlatform::safeAreaInsets(const AppWindow&) const { return {}; }
void NativePlatform::setSoftKeyboard(bool) {}
bool NativePlatform::imeTestInput(std::string_view, bool) { return false; }
void NativePlatform::setHooks(const NativeHooks& h) { m->hooks = h; }
LifecycleStats NativePlatform::lifecycle() const {
    LifecycleStats s;
    for (const Impl::Rec* r : m->recs) s.surfacesCreated += r->handle ? 1u : 0u;
    s.hasSurface = s.surfacesCreated > 0;
    return s;
}
void* NativePlatform::androidApp() { return nullptr; }
bool NativePlatform::postClick(unsigned long long nativeWin, Vec2 at) { return nativeWin && m->postClickOs(nativeWin, at); }
bool NativePlatform::setMessageHook(const AppWindow& w, const MessageHook& hook) {
    for (Impl::Rec* r : m->recs)
        if (r->win == &w && r->handle) { r->hook = hook; return true; }
    return false;
}
unsigned long long NativePlatform::nativeWindow(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r ? r->handle : 0;
}

// ---- platform services -------------------------------------------------------------------------------------------
std::vector<DisplayInfo> NativePlatform::displays() const {
#if defined(_WIN32)
    return detail::winDisplays();
#else
    return m->x11Displays();
#endif
}
SystemTheme NativePlatform::querySystemTheme() const { return m->theme(); }
bool NativePlatform::setWindowScale(const AppWindow& w, float scale) {
    for (Impl::Rec* r : m->recs)
        if (r->win == &w && r->handle) {
            m->applyScale(*r, scale, nullptr);
            return true;
        }
    return false;
}
void NativePlatform::refreshSystemTheme() { m->themeChanged(); }
bool NativePlatform::postPointerMove(unsigned long long h, Vec2 at) {
    if (!h) return false;
#if defined(_WIN32)
    return PostMessageW(reinterpret_cast<HWND>(h), WM_MOUSEMOVE, 0, MAKELPARAM(int(at.x), int(at.y))) != 0;
#else
    x11::Event ev{};
    ev.input.type = x11::MotionNotify;
    ev.input.display = m->dpy;
    ev.input.window = x11::Window(h);
    ev.input.root = m->x.RootWindow(m->dpy, m->x.DefaultScreen(m->dpy));
    ev.input.x = int(at.x);
    ev.input.y = int(at.y);
    ev.input.same_screen = 1;
    const bool ok = m->x.SendEvent(m->dpy, x11::Window(h), 0, 0, &ev) != 0;
    m->x.Flush(m->dpy);
    return ok;
#endif
}
bool NativePlatform::postClose(unsigned long long h) {
    if (!h) return false;
#if defined(_WIN32)
    return PostMessageW(reinterpret_cast<HWND>(h), WM_CLOSE, 0, 0) != 0;
#else
    x11::Event ev{};
    ev.client.type = x11::ClientMessage;
    ev.client.display = m->dpy;
    ev.client.window = x11::Window(h);
    ev.client.message_type = m->wmProtocols;
    ev.client.format = 32;
    ev.client.l[0] = long(m->wmDelete);
    const bool ok = m->x.SendEvent(m->dpy, x11::Window(h), 0, 0, &ev) != 0;
    m->x.Flush(m->dpy);
    return ok;
#endif
}
bool NativePlatform::windowFocused(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r && r->focused;
}
CursorShape NativePlatform::appliedCursor(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r ? r->svc.cursor : CursorShape::Auto;
}
bool NativePlatform::darkTitleBar(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r && r->svc.darkTitle;
}

namespace {
// fuse::ui::Image pixels are 0xAABBGGRR (R in the low byte); OS icons want 0xAARRGGBB
u32 toArgb(u32 p) { return (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu); }
} // namespace

#if defined(_WIN32)
namespace {
HICON makeIcon(const Image& img) {
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
    for (std::size_t i = 0; i < img.pixels.size(); ++i) dst[i] = toArgb(img.pixels[i]);
    HBITMAP mask = CreateBitmap(img.width, img.height, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}
const Image* closest(std::span<const Image> sizes, int want) {
    const Image* best = nullptr;
    for (const Image& i : sizes)
        if (i.width > 0 && (!best || std::abs(i.width - want) < std::abs(best->width - want))) best = &i;
    return best;
}
int iconWidth(HICON icon) {
    ICONINFO ii{};
    if (!icon || !GetIconInfo(icon, &ii)) return 0;
    BITMAP bm{};
    GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return bm.bmWidth;
}
} // namespace

bool NativePlatform::setIcon(const AppWindow& w, std::span<const Image> sizes) {
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle) return false;
    HWND hwnd = reinterpret_cast<HWND>(r->handle);
    bool ok = false;
    for (const auto& [which, metric] : {std::pair{ICON_BIG, SM_CXICON}, std::pair{ICON_SMALL, SM_CXSMICON}}) {
        const Image* img = closest(sizes, GetSystemMetrics(metric));
        if (!img) continue;
        if (HICON icon = makeIcon(*img)) {
            if (HICON old = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_SETICON, WPARAM(which), LPARAM(icon)))) DestroyIcon(old);
            ok = true;
        }
    }
    return ok;
}
bool NativePlatform::resizeWindow(const AppWindow& w, unsigned width, unsigned height) {
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle) return false;
    const f32 sc = r->win->scale(); // logical -> device pixels at the window's DPI
    const unsigned pw = unsigned(std::lround(f32(width) * sc)), ph = unsigned(std::lround(f32(height) * sc));
    int ow = int(pw), oh = int(ph); // custom frame: the client area is the whole window
    if (w.chrome().frame == WindowFrame::System)
        detail::winOuterSize(reinterpret_cast<HWND>(r->handle), pw, ph, unsigned(std::lround(sc * 96.f)), ow, oh);
    return SetWindowPos(reinterpret_cast<HWND>(r->handle), nullptr, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
}
std::vector<int> NativePlatform::iconSizes(const AppWindow& w) const {
    std::vector<int> out;
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle) return out;
    HWND hwnd = reinterpret_cast<HWND>(r->handle);
    for (WPARAM which : {WPARAM(ICON_BIG), WPARAM(ICON_SMALL)})
        if (const int wpx = iconWidth(reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, which, 0)))) out.push_back(wpx);
    return out;
}
#else
bool NativePlatform::setIcon(const AppWindow& w, std::span<const Image> sizes) {
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle || !m->dpy) return false;
    std::vector<unsigned long> data; // format-32 X properties are arrays of C long
    for (const Image& img : sizes) {
        if (img.width <= 0 || img.height <= 0 || img.pixels.size() != std::size_t(img.width) * std::size_t(img.height)) continue;
        data.push_back(static_cast<unsigned long>(img.width));
        data.push_back(static_cast<unsigned long>(img.height));
        for (u32 p : img.pixels) data.push_back(toArgb(p));
    }
    if (data.empty()) return false;
    const x11::Atom netIcon = m->x.InternAtom(m->dpy, "_NET_WM_ICON", 0);
    const x11::Atom cardinal = 6; // XA_CARDINAL
    m->x.ChangeProperty(m->dpy, x11::Window(r->handle), netIcon, cardinal, 32, 0 /*PropModeReplace*/,
                        reinterpret_cast<const unsigned char*>(data.data()), int(data.size()));
    m->x.Flush(m->dpy);
    return true;
}
bool NativePlatform::resizeWindow(const AppWindow& w, unsigned width, unsigned height) {
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle || !m->dpy) return false;
    const f32 sc = r->win->scale(); // logical -> device pixels
    m->x.ResizeWindow(m->dpy, x11::Window(r->handle), std::max(1u, unsigned(std::lround(f32(width) * sc))),
                      std::max(1u, unsigned(std::lround(f32(height) * sc))));
    m->x.Flush(m->dpy);
    return true;
}
std::vector<int> NativePlatform::iconSizes(const AppWindow& w) const {
    std::vector<int> out;
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle || !m->dpy) return out;
    const x11::Atom netIcon = m->x.InternAtom(m->dpy, "_NET_WM_ICON", 0);
    x11::Atom type = 0;
    int format = 0;
    unsigned long n = 0, after = 0;
    unsigned char* prop = nullptr;
    m->x.Sync(m->dpy, 0);
    if (m->x.GetWindowProperty(m->dpy, x11::Window(r->handle), netIcon, 0, 1L << 22, 0, 6, &type, &format, &n, &after, &prop) != 0 ||
        !prop)
        return out;
    const unsigned long* v = reinterpret_cast<const unsigned long*>(prop);
    for (unsigned long i = 0; i + 2 <= n;) {
        const unsigned long iw = v[i], ih = v[i + 1];
        if (!iw || !ih || i + 2 + iw * ih > n) break;
        out.push_back(int(iw));
        i += 2 + iw * ih;
    }
    m->x.Free(prop);
    return out;
}
#endif

} // namespace fuse::app
